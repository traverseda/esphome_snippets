#include "voice_frontend.h"

#ifdef USE_ESP32

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_agc.h>
#include <esp_heap_caps.h>
#include <esp_ns.h>

#include <cmath>
#include <cstring>

namespace esphome::voice_frontend {

static const char *const TAG = "voice_frontend";

static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr size_t NS_FRAME = SAMPLE_RATE / 100;  // 10 ms: NS and AGC both work in these
static constexpr size_t QUEUE_MS = 300;
static constexpr uint32_t STATS_WINDOW_MS = 10000;
static const char *const STAGE_NAMES[NUM_STAGES] = {"mix", "aec", "ns", "out"};

namespace {
void *psram_alloc(size_t bytes) {
  return heap_caps_malloc_prefer(bytes, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}
// 24-bit audio in the top of an int32 -> int16, keeping the top 16 bits.
inline int16_t to16(int32_t v) { return (int16_t) (v >> 16); }
// Same, with a Q16 gain applied first, rounded and saturated. Gain has to go
// in here, at full precision: a quiet mic truncated straight to 16 bits leaves
// speech a few hundred counts tall, and every later stage amplifies the grit.
inline int16_t to16_gain(int32_t v, int32_t gain_q16, uint32_t &clipped) {
  int64_t x = ((int64_t) v * gain_q16 + (1LL << 31)) >> 32;
  if (x > INT16_MAX) {
    clipped++;
    return INT16_MAX;
  }
  if (x < INT16_MIN) {
    clipped++;
    return INT16_MIN;
  }
  return (int16_t) x;
}
// In-place Q16 gain with saturation; returns the number of clipped samples.
uint32_t apply_gain(int16_t *buf, size_t n, int32_t gain_q16) {
  if (gain_q16 == 65536)
    return 0;
  uint32_t clipped = 0;
  for (size_t i = 0; i < n; i++)
    buf[i] = to16_gain((int32_t) buf[i] << 16, gain_q16, clipped);
  return clipped;
}
}  // namespace

void VoiceFrontend::setup() {
  // AEC first: it decides the chunk size everything else is fed in.
  aec_config_t cfg = {};
  cfg.mic_num = 1;
  cfg.ref_num = 1;
  cfg.out_num = 1;
  cfg.filter_length = this->aec_filter_length_;
  cfg.sample_rate = SAMPLE_RATE;
  cfg.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  cfg.mode = (aec_mode_t) this->aec_mode_;
  cfg.nlp_level = (aec_nlp_level_t) this->aec_nlp_level_;
  this->aec_ = aec_create_from_config(&cfg);
  if (this->aec_ == nullptr) {
    ESP_LOGE(TAG, "aec_create failed");
    this->mark_failed();
    return;
  }
  this->chunk_ = aec_get_chunksize(this->aec_);

  this->ns_ = ns_pro_create(10, this->ns_mode_.load(), SAMPLE_RATE);
  this->agc_ = esp_agc_open(AGC_MODE_2, SAMPLE_RATE);
  if (this->ns_ == nullptr || this->agc_ == nullptr) {
    ESP_LOGE(TAG, "NS/AGC create failed");
    this->mark_failed();
    return;
  }
  ::set_agc_config(this->agc_, this->agc_gain_.load(), this->agc_limiter_.load(), this->agc_target_.load());
  this->agc_dirty_.store(false);
  this->ns_dirty_.store(false);

  const size_t c = this->chunk_;
  this->in_ = (int16_t *) psram_alloc(c * 2 * sizeof(int16_t));
  this->mic_buf_ = (int16_t *) psram_alloc(c * sizeof(int16_t));
  this->ref_buf_ = (int16_t *) psram_alloc(c * sizeof(int16_t));
  this->aec_buf_ = (int16_t *) psram_alloc(c * sizeof(int16_t));
  this->fifo_mix_ = (int16_t *) psram_alloc((c + NS_FRAME) * sizeof(int16_t));
  this->fifo_aec_ = (int16_t *) psram_alloc((c + NS_FRAME) * sizeof(int16_t));
  this->ns_buf_ = (int16_t *) psram_alloc(NS_FRAME * sizeof(int16_t));
  this->out_buf_ = (int16_t *) psram_alloc(NS_FRAME * sizeof(int16_t));
  this->emit_buf_ = (int16_t *) psram_alloc(NS_FRAME * 2 * sizeof(int16_t));
  const size_t queue_bytes = SAMPLE_RATE * QUEUE_MS / 1000 * 2 * sizeof(int16_t);
  auto *queue_storage = (uint8_t *) psram_alloc(queue_bytes + 1);
  // audio_io hands over up to 40 ms per read.
  this->capture_scratch_frames_ = SAMPLE_RATE / 1000 * 40;
  this->capture_scratch_ = (int16_t *) psram_alloc(this->capture_scratch_frames_ * 2 * sizeof(int16_t));
  if (!this->in_ || !this->mic_buf_ || !this->ref_buf_ || !this->aec_buf_ || !this->fifo_mix_ || !this->fifo_aec_ ||
      !this->ns_buf_ || !this->out_buf_ || !this->emit_buf_ || !queue_storage || !this->capture_scratch_) {
    ESP_LOGE(TAG, "Buffer allocation failed");
    this->mark_failed();
    return;
  }
  this->queue_ = xStreamBufferCreateStatic(queue_bytes, c * 2 * sizeof(int16_t), queue_storage, &this->queue_struct_);

  this->acc_started_ms_ = millis();
  this->io_->add_frame_listener([this](const int32_t *frames, size_t count) { this->on_capture_(frames, count); });
  // Core 1: the capture task and the radio live on core 0.
  xTaskCreatePinnedToCore(VoiceFrontend::process_task_, "voice_fe", 6144, this, 15, &this->task_, 1);
}

// Capture task: mix down, convert, queue. Whole reads or nothing - a partial
// write would desynchronise the [mic, ref] interleave for good.
void VoiceFrontend::on_capture_(const int32_t *frames, size_t count) {
  if (this->queue_ == nullptr || count > this->capture_scratch_frames_)
    return;
  const MicMix mix = this->mix_.load(std::memory_order_relaxed);
  const int32_t gain = this->input_gain_q16_.load(std::memory_order_relaxed);
  uint32_t clipped = 0;
  int16_t *out = this->capture_scratch_;
  for (size_t i = 0; i < count; i++) {
    const int32_t *f = frames + i * audio_io::NUM_OUTPUT_CHANNELS;
    int32_t m;
    switch (mix) {
      case MicMix::MIC1:
        m = f[audio_io::CH_MIC1];
        break;
      case MicMix::MIC2:
        m = f[audio_io::CH_MIC2];
        break;
      default:
        m = (int32_t) (((int64_t) f[audio_io::CH_MIC1] + f[audio_io::CH_MIC2]) / 2);
        break;
    }
    out[i * 2] = to16_gain(m, gain, clipped);
    out[i * 2 + 1] = to16(f[audio_io::CH_REF]);
  }
  if (clipped)
    this->input_clipped_.fetch_add(clipped, std::memory_order_relaxed);
  const size_t bytes = count * 2 * sizeof(int16_t);
  if (xStreamBufferSpacesAvailable(this->queue_) < bytes) {
    this->dropped_frames_.fetch_add(count, std::memory_order_relaxed);
    return;
  }
  xStreamBufferSend(this->queue_, out, bytes, 0);
}

void VoiceFrontend::process_task_(void *arg) {
  ((VoiceFrontend *) arg)->process_loop_();
  vTaskDelete(nullptr);
}

void VoiceFrontend::set_input_gain_db(float db) {
  this->input_gain_db_ = db;
  this->input_gain_q16_.store((int32_t) lroundf(powf(10.0f, db / 20.0f) * 65536.0f));
}

void VoiceFrontend::set_makeup_gain_db(float db) {
  this->makeup_gain_db_ = db;
  this->makeup_gain_q16_.store((int32_t) lroundf(powf(10.0f, db / 20.0f) * 65536.0f));
}

void VoiceFrontend::apply_pending_() {
  if (this->ns_dirty_.exchange(false)) {
    ns_destroy(this->ns_);
    this->ns_ = ns_pro_create(10, this->ns_mode_.load(), SAMPLE_RATE);
  }
  if (this->agc_dirty_.exchange(false))
    ::set_agc_config(this->agc_, this->agc_gain_.load(), this->agc_limiter_.load(), this->agc_target_.load());
}

void VoiceFrontend::process_loop_() {
  const size_t c = this->chunk_;
  const size_t chunk_bytes = c * 2 * sizeof(int16_t);
  while (true) {
    // Block until a whole AEC chunk is queued (the trigger level).
    size_t got = 0;
    while (got < chunk_bytes)
      got += xStreamBufferReceive(this->queue_, (uint8_t *) this->in_ + got, chunk_bytes - got, portMAX_DELAY);

    this->apply_pending_();
    for (size_t i = 0; i < c; i++) {
      this->mic_buf_[i] = this->in_[i * 2];
      this->ref_buf_[i] = this->in_[i * 2 + 1];
    }
    if (this->aec_on_.load(std::memory_order_relaxed)) {
      aec_process(this->aec_, this->mic_buf_, this->ref_buf_, this->aec_buf_);
    } else {
      memcpy(this->aec_buf_, this->mic_buf_, c * sizeof(int16_t));
    }

    // AEC chunks and 10 ms NS frames don't divide evenly; a short FIFO joins them.
    // Makeup gain goes on here, after AEC: echo from our own speaker is far
    // louder than speech, and AEC can't model echo that clipped on the way in.
    // The mix tap gets it too, so every tap is at the same scale.
    memcpy(this->fifo_mix_ + this->fifo_len_, this->mic_buf_, c * sizeof(int16_t));
    memcpy(this->fifo_aec_ + this->fifo_len_, this->aec_buf_, c * sizeof(int16_t));
    const int32_t makeup = this->makeup_gain_q16_.load(std::memory_order_relaxed);
    apply_gain(this->fifo_mix_ + this->fifo_len_, c, makeup);
    const uint32_t clipped = apply_gain(this->fifo_aec_ + this->fifo_len_, c, makeup);
    if (clipped)
      this->makeup_clipped_.fetch_add(clipped, std::memory_order_relaxed);
    this->fifo_len_ += c;
    size_t pos = 0;
    while (this->fifo_len_ - pos >= NS_FRAME) {
      this->run_ns_agc_(this->fifo_mix_ + pos, this->fifo_aec_ + pos, NS_FRAME);
      pos += NS_FRAME;
    }
    this->fifo_len_ -= pos;
    memmove(this->fifo_mix_, this->fifo_mix_ + pos, this->fifo_len_ * sizeof(int16_t));
    memmove(this->fifo_aec_, this->fifo_aec_ + pos, this->fifo_len_ * sizeof(int16_t));
  }
}

void VoiceFrontend::run_ns_agc_(int16_t *mix, int16_t *aec, size_t n) {
  if (this->ns_on_.load(std::memory_order_relaxed)) {
    ns_process(this->ns_, aec, this->ns_buf_);
  } else {
    memcpy(this->ns_buf_, aec, n * sizeof(int16_t));
  }
  if (this->agc_on_.load(std::memory_order_relaxed)) {
    esp_agc_process(this->agc_, this->ns_buf_, this->out_buf_, n, SAMPLE_RATE);
  } else {
    memcpy(this->out_buf_, this->ns_buf_, n * sizeof(int16_t));
  }

  const int16_t *stages[NUM_STAGES] = {mix, aec, this->ns_buf_, this->out_buf_};
  const int16_t *tap = stages[(uint8_t) this->tap_.load(std::memory_order_relaxed)];
  const bool muted = this->muted_.load(std::memory_order_relaxed);
  for (size_t i = 0; i < n; i++) {
    this->emit_buf_[i * 2] = muted ? 0 : this->out_buf_[i];
    this->emit_buf_[i * 2 + 1] = muted ? 0 : tap[i];
    for (uint8_t s = 0; s < NUM_STAGES; s++)
      this->acc_sq_[s] += (double) stages[s][i] * stages[s][i];
  }
  this->acc_n_ += n;
  if (this->mic_ != nullptr)
    this->mic_->on_frames(this->emit_buf_, n);

  const uint32_t now = millis();
  if (now - this->acc_started_ms_ >= STATS_WINDOW_MS && !this->levels_ready_.load()) {
    for (uint8_t s = 0; s < NUM_STAGES; s++) {
      const double rms = sqrt(this->acc_sq_[s] / this->acc_n_);
      this->levels_[s] = rms > 0 ? 20.0f * log10f((float) (rms / 32768.0)) : -120.0f;
      this->acc_sq_[s] = 0;
    }
    this->acc_n_ = 0;
    this->acc_started_ms_ = now;
    this->levels_ready_.store(true);
  }
}

void VoiceFrontend::loop() {
  if (!this->levels_ready_.load())
    return;
  ESP_LOGD(TAG, "levels dBFS: mix %.1f  aec %.1f  ns %.1f  out %.1f   [aec %s, ns %s(mode %d), agc %s(-%d dBFS, %d dB)]",
           this->levels_[0], this->levels_[1], this->levels_[2], this->levels_[3], ONOFF(this->aec_on_.load()),
           ONOFF(this->ns_on_.load()), this->ns_mode_.load(), ONOFF(this->agc_on_.load()), this->agc_target_.load(),
           this->agc_gain_.load());
  const uint32_t clipped = this->input_clipped_.exchange(0);
  if (clipped)
    ESP_LOGW(TAG, "input gain %.1f dB clipped %u samples in the last window", this->input_gain_db_, (unsigned) clipped);
  const uint32_t mclipped = this->makeup_clipped_.exchange(0);
  if (mclipped)
    ESP_LOGW(TAG, "makeup gain %.1f dB clipped %u samples in the last window", this->makeup_gain_db_,
             (unsigned) mclipped);
  const uint32_t dropped = this->dropped_frames_.load();
  if (dropped)
    ESP_LOGW(TAG, "%u frames dropped since boot (processing fell behind)", (unsigned) dropped);
  this->levels_ready_.store(false);
}

void VoiceFrontend::dump_config() {
  static const char *const MIXES[] = {"mic1", "mic2", "average"};
  ESP_LOGCONFIG(TAG,
                "Voice front end:\n"
                "  Mic mix: %s\n"
                "  Gain: %.1f dB before AEC, %.1f dB after\n"
                "  AEC: %s (mode %d, filter length %d, NLP level %d, chunk %u samples)\n"
                "  NS: %s (mode %d)\n"
                "  AGC: %s (target -%d dBFS, compression gain %d dB, limiter %s)\n"
                "  Debug tap: %s",
                MIXES[(uint8_t) this->mix_.load()], this->input_gain_db_, this->makeup_gain_db_, ONOFF(this->aec_on_.load()), this->aec_mode_,
                this->aec_filter_length_, this->aec_nlp_level_, (unsigned) this->chunk_, ONOFF(this->ns_on_.load()),
                this->ns_mode_.load(), ONOFF(this->agc_on_.load()), this->agc_target_.load(), this->agc_gain_.load(),
                YESNO(this->agc_limiter_.load()), STAGE_NAMES[(uint8_t) this->tap_.load()]);
}

// ---------------------------------------------------------------------------

void VoiceFrontendMicrophone::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 2, SAMPLE_RATE);
  this->buffer_.reserve(NS_FRAME * 2 * sizeof(int16_t));
  this->parent_->set_microphone(this);
}

void VoiceFrontendMicrophone::start() {
  if (this->is_failed() || this->parent_->is_failed())
    return;
  this->state_ = microphone::STATE_RUNNING;
  this->running_.store(true);
}

void VoiceFrontendMicrophone::stop() {
  this->running_.store(false);
  this->state_ = microphone::STATE_STOPPED;
}

void VoiceFrontendMicrophone::on_frames(const int16_t *frames, size_t count) {
  if (!this->running_.load() || this->data_callbacks_.size() == 0)
    return;
  const size_t bytes = count * 2 * sizeof(int16_t);
  if (bytes > this->buffer_.capacity())
    return;
  this->buffer_.resize(bytes);
  memcpy(this->buffer_.data(), frames, bytes);
  this->data_callbacks_.call(this->buffer_);
}

}  // namespace esphome::voice_frontend

#endif  // USE_ESP32
