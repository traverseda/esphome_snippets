#include "audio_io.h"

#ifdef USE_ESP32

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <esp_codec_dev_defaults.h>
#include <es7210_adc.h>
#include <es8311_codec.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace esphome::audio_io {

static const char *const TAG = "audio_io";

// ~60 ms of DMA buffering. A descriptor must stay under 4092 bytes, and a
// 4-slot 32-bit frame is 16 bytes, so 160 frames (2560 B) per descriptor.
static constexpr uint32_t DMA_DESC_NUM = 6;
static constexpr uint32_t DMA_FRAME_NUM = 160;
static constexpr uint32_t MCLK_MULTIPLE = 256;
static constexpr int32_t FULL_SCALE_CLIP = INT32_MAX - (1 << 8);  // 24-bit ADC in a 32-bit slot
// The ES8311 reads the bus as plain I2S: with Philips TDM's 50% word clock, its
// left channel is slot 0 and its right channel is slot 2. It's a mono DAC on
// the left; the right gets the same samples so a codec misconfigured to the
// right channel still plays.
static constexpr uint8_t TX_SLOT_LEFT = 0, TX_SLOT_RIGHT = 2;
// ES8311 register 0x32: 0x00 = -95.5 dB, 0xBF = 0 dB, 0xFF = +32 dB, 0.5 dB steps.
static constexpr uint8_t ES8311_REG_DAC_VOLUME = 0x32;
static constexpr float ES8311_MIN_DB = -95.5f, ES8311_MAX_DB = 32.0f;
// Playback meter. Band centres an octave apart; Q of sqrt(2) gives octave-wide
// bands, so pink-ish music lights every band about equally.
static constexpr float METER_BAND_HZ[NUM_METER_BANDS] = {100, 200, 400, 800, 1600, 3200, 6400};
static constexpr float METER_Q = 1.414f;
static constexpr float METER_RANGE_DB = 30.0f;        // spread from ceiling to dark
static constexpr float METER_CEILING_FALL_DB = 0.03f;  // per 10 ms: 3 dB/s, so a quieter track takes over in seconds
static constexpr float METER_CEILING_MIN_DB = -45.0f;  // never auto-range up into near-silence
static constexpr float METER_RELEASE = 0.15f;          // per 10 ms: ~150 ms visual decay

// ---------------------------------------------------------------------------
// esp_codec_dev control interface over ESPHome's I2C bus. The codec library
// only needs open/read_reg/write_reg; this routes them through the bus ESPHome
// already owns instead of opening a second I2C driver on the same pins.
// ---------------------------------------------------------------------------
namespace {
struct I2cCtrl {
  audio_codec_ctrl_if_t base;
  i2c::I2CBus *bus;
  uint8_t address;
  bool open;
};

int ctrl_open(const audio_codec_ctrl_if_t *ctrl, void *, int) {
  auto *self = (I2cCtrl *) ctrl;
  self->open = self->bus != nullptr;
  return self->open ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_INVALID_ARG;
}
bool ctrl_is_open(const audio_codec_ctrl_if_t *ctrl) { return ((I2cCtrl *) ctrl)->open; }
int ctrl_read_reg(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len) {
  auto *self = (I2cCtrl *) ctrl;
  if (reg_len != 1 || data == nullptr || data_len <= 0)
    return ESP_CODEC_DEV_INVALID_ARG;
  uint8_t r = reg;
  return self->bus->write_readv(self->address, &r, 1, (uint8_t *) data, data_len) == i2c::NO_ERROR
             ? ESP_CODEC_DEV_OK
             : ESP_CODEC_DEV_READ_FAIL;
}
int ctrl_write_reg(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len) {
  auto *self = (I2cCtrl *) ctrl;
  if (reg_len != 1 || data_len < 0 || data_len > 8)
    return ESP_CODEC_DEV_INVALID_ARG;
  uint8_t buf[9];
  buf[0] = reg;
  memcpy(buf + 1, data, data_len);
  return self->bus->write_readv(self->address, buf, 1 + data_len, nullptr, 0) == i2c::NO_ERROR
             ? ESP_CODEC_DEV_OK
             : ESP_CODEC_DEV_WRITE_FAIL;
}
int ctrl_close(const audio_codec_ctrl_if_t *ctrl) {
  ((I2cCtrl *) ctrl)->open = false;
  return ESP_CODEC_DEV_OK;
}

float to_dbfs(double linear) { return linear > 0.0 ? 20.0f * log10f((float) (linear / 2147483648.0)) : -120.0f; }
}  // namespace

// ---------------------------------------------------------------------------

const audio_codec_ctrl_if_t *AudioIO::new_ctrl_(uint8_t address) {
  auto *ctrl = (I2cCtrl *) calloc(1, sizeof(I2cCtrl));
  if (ctrl == nullptr)
    return nullptr;
  ctrl->base.open = ctrl_open;
  ctrl->base.is_open = ctrl_is_open;
  ctrl->base.read_reg = ctrl_read_reg;
  ctrl->base.write_reg = ctrl_write_reg;
  ctrl->base.close = ctrl_close;
  ctrl->bus = this->i2c_bus_;
  ctrl->address = address;
  ctrl->open = true;  // the codec constructors require an already-open ctrl
  return &ctrl->base;
}

void AudioIO::setup() {
  this->frames_per_read_ = this->sample_rate_ * this->frame_ms_ / 1000;
  const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  this->tdm_buf_ = (int32_t *) heap_caps_malloc_prefer(this->frames_per_read_ * TDM_SLOTS * 4, 2, caps, MALLOC_CAP_8BIT);
  this->out_buf_ =
      (int32_t *) heap_caps_malloc_prefer(this->frames_per_read_ * NUM_OUTPUT_CHANNELS * 4, 2, caps, MALLOC_CAP_8BIT);
  if (this->tdm_buf_ == nullptr || this->out_buf_ == nullptr) {
    ESP_LOGE(TAG, "Buffer allocation failed");
    this->mark_failed();
    return;
  }
  if (!this->init_i2s_() || !this->init_codec_()) {
    this->mark_failed();
    return;
  }
  this->ok_ = true;
  this->acc_started_ms_ = millis();
  // Core 0 with the radio, like the old stack's audio task; priority above
  // everything ESPHome runs so a slow main loop never costs us samples.
  xTaskCreatePinnedToCore(AudioIO::capture_task_, "audio_io", 4096, this, 18, &this->task_, 0);
  if (this->tx_dev_ != nullptr)
    xTaskCreatePinnedToCore(AudioIO::playback_task_, "audio_tx", 4096, this, 18, &this->tx_task_, 0);
}

bool AudioIO::init_i2s_() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = DMA_DESC_NUM;
  chan_cfg.dma_frame_num = DMA_FRAME_NUM;
  // One port, both directions: TX and RX share BCLK/LRCLK/MCLK.
  const bool tx = this->dout_pin_ >= 0;
  esp_err_t err = i2s_new_channel(&chan_cfg, tx ? &this->tx_handle_ : nullptr, &this->rx_handle_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
    return false;
  }

  // Same bus shape the external stack used: Philips TDM, four 32-bit slots,
  // STEREO so every active slot lands in DMA (MONO keeps only slot 0).
  const i2s_tdm_slot_mask_t mask = (i2s_tdm_slot_mask_t) (I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3);
  i2s_tdm_config_t tdm_cfg = {
      .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(this->sample_rate_),
      .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO, mask),
      .gpio_cfg =
          {
              .mclk = (gpio_num_t) this->mclk_pin_,
              .bclk = (gpio_num_t) this->bclk_pin_,
              .ws = (gpio_num_t) this->lrclk_pin_,
              .dout = tx ? (gpio_num_t) this->dout_pin_ : GPIO_NUM_NC,
              .din = (gpio_num_t) this->din_pin_,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (tx) {
    err = i2s_channel_init_tdm_mode(this->tx_handle_, &tdm_cfg);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "i2s_channel_init_tdm_mode (TX): %s", esp_err_to_name(err));
      return false;
    }
  }
  err = i2s_channel_init_tdm_mode(this->rx_handle_, &tdm_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "i2s_channel_init_tdm_mode: %s", esp_err_to_name(err));
    return false;
  }

  // Must be registered before the channel is enabled (esp_codec_dev_open does that).
  i2s_event_callbacks_t cbs = {};
  cbs.on_recv_q_ovf = AudioIO::on_rx_overflow_;
  i2s_channel_register_event_callback(this->rx_handle_, &cbs, this);
  return true;
}

bool AudioIO::init_codec_() {
  this->ctrl_if_ = this->new_ctrl_(this->es7210_address_);

  audio_codec_i2s_cfg_t i2s_cfg = {
      .port = I2S_NUM_0, .rx_handle = this->rx_handle_, .tx_handle = this->tx_handle_, .clk_src = 0};
  this->data_if_ = audio_codec_new_i2s_data(&i2s_cfg);

  es7210_codec_cfg_t es_cfg = {};
  es_cfg.ctrl_if = this->ctrl_if_;
  es_cfg.master_mode = false;
  // All four ADCs on: three or more is what puts the ES7210 into TDM mode.
  es_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
  es_cfg.mclk_div = MCLK_MULTIPLE;
  this->codec_if_ = es7210_codec_new(&es_cfg);
  if (this->data_if_ == nullptr || this->codec_if_ == nullptr) {
    ESP_LOGE(TAG, "esp_codec_dev ES7210 setup failed (I2C address 0x%02X?)", this->es7210_address_);
    return false;
  }

  esp_codec_dev_sample_info_t fs = {
      .bits_per_sample = 32,
      .channel = TDM_SLOTS,
      .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                      ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
      .sample_rate = this->sample_rate_,
      .mclk_multiple = (int) MCLK_MULTIPLE,
  };

  // Playback first, then capture: the order Espressif's TDM codec tests use, so
  // TX has configured the shared clock before RX starts consuming it.
  if (this->tx_handle_ != nullptr) {
    this->dac_ctrl_if_ = this->new_ctrl_(this->es8311_address_);
    es8311_codec_cfg_t dac_cfg = {};
    dac_cfg.ctrl_if = this->dac_ctrl_if_;
    dac_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    dac_cfg.pa_pin = -1;  // the amplifier enable is on the TCA9555; YAML owns it
    dac_cfg.master_mode = false;
    dac_cfg.use_mclk = true;
    dac_cfg.mclk_div = MCLK_MULTIPLE;
    this->dac_codec_if_ = es8311_codec_new(&dac_cfg);
    if (this->dac_codec_if_ == nullptr) {
      ESP_LOGE(TAG, "esp_codec_dev ES8311 setup failed (I2C address 0x%02X?)", this->es8311_address_);
      return false;
    }
    esp_codec_dev_cfg_t tx_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = this->dac_codec_if_, .data_if = this->data_if_};
    this->tx_dev_ = esp_codec_dev_new(&tx_cfg);
    int ret = this->tx_dev_ ? esp_codec_dev_open(this->tx_dev_, &fs) : ESP_CODEC_DEV_NOT_SUPPORT;
    if (ret != ESP_CODEC_DEV_OK) {
      ESP_LOGE(TAG, "esp_codec_dev_open (TX): %d", ret);
      this->tx_dev_ = nullptr;
      return false;
    }
    // The codec library programs its own default volume on open; ours wins.
    this->set_output_db(this->output_db_);
  }

  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_IN,
      .codec_if = this->codec_if_,
      .data_if = this->data_if_,
  };
  this->rx_dev_ = esp_codec_dev_new(&dev_cfg);
  int ret = esp_codec_dev_open(this->rx_dev_, &fs);
  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "esp_codec_dev_open: %d", ret);
    return false;
  }
  this->set_mic_gain_db(this->mic_gain_db_);
  this->set_adc_channel_gain_db(this->ref_adc_channel_, this->ref_gain_db_);
  // After open: es7210_codec_new() has already written its own HPF defaults.
  this->set_mic_hpf(this->mic_hpf_coef_);
  return true;
}

bool AudioIO::set_output_db(float db) {
  const float max_db = std::min(this->volume_max_db_, ES8311_MAX_DB);
  if (db > max_db)
    db = max_db;
  if (db < ES8311_MIN_DB)
    db = ES8311_MIN_DB;
  this->output_db_ = db;
  if (this->dac_ctrl_if_ == nullptr)
    return false;
  uint8_t reg = (uint8_t) lroundf((db - ES8311_MIN_DB) * 2.0f);
  return this->dac_ctrl_if_->write_reg(this->dac_ctrl_if_, ES8311_REG_DAC_VOLUME, 1, &reg, 1) == ESP_CODEC_DEV_OK;
}

bool AudioIO::set_output_mute(bool mute) {
  return this->tx_dev_ != nullptr && esp_codec_dev_set_out_mute(this->tx_dev_, mute) == ESP_CODEC_DEV_OK;
}

bool AudioIO::set_mic_hpf(uint8_t coef) {
  if (coef < 1 || coef > 13) {
    ESP_LOGW(TAG, "Mic HPF coefficient %u out of the safe range 1..13; ignored", coef);
    return false;
  }
  // Keep the other bits in each register (channel cross/polarity/offset freeze).
  const int hpf1 = this->read_es7210_reg(0x22), hpf2 = this->read_es7210_reg(0x23);
  if (hpf1 < 0 || hpf2 < 0)
    return false;
  this->mic_hpf_coef_ = coef;
  return this->write_es7210_reg(0x22, (hpf1 & 0xE0) | coef) && this->write_es7210_reg(0x23, (hpf2 & 0xE0) | coef);
}

bool IRAM_ATTR AudioIO::on_rx_overflow_(i2s_chan_handle_t, i2s_event_data_t *, void *ctx) {
  ((AudioIO *) ctx)->rx_overflows_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void AudioIO::capture_task_(void *arg) {
  ((AudioIO *) arg)->capture_loop_();
  vTaskDelete(nullptr);
}

void AudioIO::capture_loop_() {
  const size_t bytes = this->frames_per_read_ * TDM_SLOTS * 4;
  while (true) {
    if (esp_codec_dev_read(this->rx_dev_, this->tdm_buf_, (int) bytes) != ESP_CODEC_DEV_OK) {
      this->read_errors_.fetch_add(1, std::memory_order_relaxed);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    const size_t n = this->frames_per_read_;
    for (size_t f = 0; f < n; f++) {
      const int32_t *in = this->tdm_buf_ + f * TDM_SLOTS;
      int32_t *out = this->out_buf_ + f * NUM_OUTPUT_CHANNELS;
      out[CH_MIC1] = in[this->slot_of_[CH_MIC1]];
      out[CH_MIC2] = in[this->slot_of_[CH_MIC2]];
      out[CH_REF] = in[this->slot_of_[CH_REF]];
    }
    this->accumulate_stats_(this->out_buf_, n);
    for (auto &listener : this->listeners_)
      listener(this->out_buf_, n);
  }
}

void AudioIO::playback_task_(void *arg) {
  ((AudioIO *) arg)->playback_loop_();
  vTaskDelete(nullptr);
}

// One DMA descriptor per write. i2s_channel_write blocks until a descriptor is
// free, so this loop runs exactly at the bus rate and is always DMA_DESC_NUM
// descriptors ahead of the DAC - which is also how the play-out time of each
// write is known without per-buffer bookkeeping.
void AudioIO::playback_loop_() {
  const size_t n = DMA_FRAME_NUM;
  const int64_t frame_us = 1000000LL / this->sample_rate_;
  const int64_t queue_us = (int64_t) DMA_DESC_NUM * n * frame_us;
  auto *pcm = (int16_t *) heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  auto *tdm = (int32_t *) heap_caps_malloc(n * TDM_SLOTS * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (pcm == nullptr || tdm == nullptr) {
    ESP_LOGE(TAG, "Playback buffer allocation failed");
    return;
  }
  memset(tdm, 0, n * TDM_SLOTS * 4);
  // RBJ band-pass (constant 0 dB peak) for each meter band.
  for (uint8_t b = 0; b < NUM_METER_BANDS; b++) {
    const float w0 = 2.0f * (float) M_PI * METER_BAND_HZ[b] / (float) this->sample_rate_;
    const float alpha = sinf(w0) / (2.0f * METER_Q);
    const float a0 = 1.0f + alpha;
    this->meter_filters_[b] = {alpha / a0, 0.0f, -alpha / a0, -2.0f * cosf(w0) / a0, (1.0f - alpha) / a0, 0.0f, 0.0f};
  }
  while (true) {
    const size_t got = this->speaker_ != nullptr ? this->speaker_->read_for_playback(pcm, n) : 0;
    this->update_meter_(pcm, got);
    for (size_t i = 0; i < n; i++) {
      const int32_t v = i < got ? (int32_t) pcm[i] << 16 : 0;
      tdm[i * TDM_SLOTS + TX_SLOT_LEFT] = v;
      tdm[i * TDM_SLOTS + TX_SLOT_RIGHT] = v;
    }
    if (esp_codec_dev_write(this->tx_dev_, tdm, (int) (n * TDM_SLOTS * 4)) != ESP_CODEC_DEV_OK) {
      this->write_errors_.fetch_add(1, std::memory_order_relaxed);
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    // Real samples sit at the start of the buffer, silence after them.
    if (got > 0)
      this->speaker_->report_played(got, esp_timer_get_time() + queue_us - (int64_t) (n - got) * frame_us);
  }
}

void AudioIO::update_meter_(const int16_t *pcm, size_t n) {
  float db[NUM_METER_BANDS + 1];  // bands, then the whole signal
  if (n == 0) {
    for (auto &d : db)
      d = -120.0f;
  } else {
    float sq[NUM_METER_BANDS + 1]{};
    for (size_t i = 0; i < n; i++) {
      const float x = pcm[i] * (1.0f / 32768.0f);
      sq[NUM_METER_BANDS] += x * x;
      for (uint8_t b = 0; b < NUM_METER_BANDS; b++) {
        Biquad &f = this->meter_filters_[b];
        const float y = f.b0 * x + f.z1;
        f.z1 = f.b1 * x - f.a1 * y + f.z2;
        f.z2 = f.b2 * x - f.a2 * y;
        sq[b] += y * y;
      }
    }
    for (uint8_t b = 0; b <= NUM_METER_BANDS; b++)
      db[b] = 10.0f * log10f(sq[b] / n + 1e-12f);
  }
  // One ceiling for everything: it follows the loudest thing playing and sinks
  // slowly, so loudness auto-ranges but the spectrum keeps its shape.
  float loudest = db[NUM_METER_BANDS];
  for (uint8_t b = 0; b < NUM_METER_BANDS; b++)
    loudest = std::max(loudest, db[b]);
  this->meter_ceiling_db_ = std::max({loudest, this->meter_ceiling_db_ - METER_CEILING_FALL_DB, METER_CEILING_MIN_DB});
  for (uint8_t b = 0; b <= NUM_METER_BANDS; b++) {
    const float target = clamp((db[b] - (this->meter_ceiling_db_ - METER_RANGE_DB)) / METER_RANGE_DB, 0.0f, 1.0f);
    float &shown = this->meter_shown_[b];
    shown = target > shown ? target : shown + (target - shown) * METER_RELEASE;
  }
  for (uint8_t b = 0; b < NUM_METER_BANDS; b++)
    this->meter_band_[b].store(this->meter_shown_[b], std::memory_order_relaxed);
  this->meter_level_.store(this->meter_shown_[NUM_METER_BANDS], std::memory_order_relaxed);
}

void AudioIO::accumulate_stats_(const int32_t *frames, size_t count) {
  double read_sq[NUM_OUTPUT_CHANNELS]{};
  for (size_t f = 0; f < count; f++) {
    for (uint8_t c = 0; c < NUM_OUTPUT_CHANNELS; c++) {
      const double v = frames[f * NUM_OUTPUT_CHANNELS + c];
      read_sq[c] += v * v;
    }
  }
  for (uint8_t c = 0; c < NUM_OUTPUT_CHANNELS; c++)
    this->fast_level_[c].store(count ? to_dbfs(sqrt(read_sq[c] / count)) : -120.0f, std::memory_order_relaxed);
  for (size_t f = 0; f < count; f++) {
    for (uint8_t c = 0; c < NUM_OUTPUT_CHANNELS; c++) {
      const int64_t v = frames[f * NUM_OUTPUT_CHANNELS + c];
      this->acc_sq_[c] += (double) v * (double) v;
      this->acc_sum_[c] += (double) v;
      const int64_t a = v < 0 ? -v : v;
      if (a > this->acc_peak_[c])
        this->acc_peak_[c] = a;
      if (a >= FULL_SCALE_CLIP)
        this->acc_clipped_[c]++;
    }
  }
  this->acc_frames_ += count;

  const uint32_t now = millis();
  const uint32_t window = this->stats_interval_ms_ > 0 ? this->stats_interval_ms_ : 1000;
  if (now - this->acc_started_ms_ < window || this->stats_ready_.load())
    return;
  for (uint8_t c = 0; c < NUM_OUTPUT_CHANNELS; c++) {
    const double mean = this->acc_sum_[c] / (double) this->acc_frames_;
    const double var = this->acc_sq_[c] / (double) this->acc_frames_ - mean * mean;
    this->stats_[c].rms_dbfs = to_dbfs(sqrt(var > 0 ? var : 0));  // AC only: DC reported separately
    this->stats_[c].peak_dbfs = to_dbfs((double) this->acc_peak_[c]);
    this->stats_[c].dc_dbfs = to_dbfs(fabs(mean));
    this->stats_[c].clipped = this->acc_clipped_[c];
    this->acc_sq_[c] = this->acc_sum_[c] = 0;
    this->acc_peak_[c] = 0;
    this->acc_clipped_[c] = 0;
  }
  this->acc_frames_ = 0;
  this->acc_started_ms_ = now;
  this->stats_ready_.store(true);
}

void AudioIO::loop() {
  if (!this->stats_ready_.load())
    return;
  if (this->stats_interval_ms_ > 0) {
    static const char *const names[NUM_OUTPUT_CHANNELS] = {"mic1", "mic2", "ref "};
    for (uint8_t c = 0; c < NUM_OUTPUT_CHANNELS; c++) {
      const auto &s = this->stats_[c];
      ESP_LOGD(TAG, "%s: rms %6.1f  peak %6.1f  dc %6.1f dBFS  clipped %u", names[c], s.rms_dbfs, s.peak_dbfs,
               s.dc_dbfs, (unsigned) s.clipped);
    }
    const uint32_t ovf = this->rx_overflows_.load(), err = this->read_errors_.load(), werr = this->write_errors_.load();
    if (ovf || err || werr)
      ESP_LOGW(TAG, "rx overflows %u, read errors %u, write errors %u (since boot)", (unsigned) ovf, (unsigned) err,
               (unsigned) werr);
  }
  this->stats_ready_.store(false);
}

bool AudioIO::set_mic_gain_db(float db) {
  this->mic_gain_db_ = db;
  bool ok = true;
  for (uint8_t ch = 0; ch < 4; ch++) {
    if (ch != this->ref_adc_channel_)
      ok &= this->set_adc_channel_gain_db(ch, db);
  }
  return ok;
}

bool AudioIO::set_adc_channel_gain_db(uint8_t adc_channel, float db) {
  if (this->rx_dev_ == nullptr || adc_channel > 3)
    return false;
  const int ret = esp_codec_dev_set_in_channel_gain(this->rx_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(adc_channel), db);
  ESP_LOGI(TAG, "ADC channel %u PGA -> %.1f dB%s", adc_channel, db, ret == ESP_CODEC_DEV_OK ? "" : " FAILED");
  return ret == ESP_CODEC_DEV_OK;
}

int AudioIO::read_es7210_reg(uint8_t reg) {
  uint8_t v = 0;
  if (this->ctrl_if_ == nullptr || this->ctrl_if_->read_reg(this->ctrl_if_, reg, 1, &v, 1) != ESP_CODEC_DEV_OK)
    return -1;
  return v;
}

bool AudioIO::write_es7210_reg(uint8_t reg, uint8_t value) {
  if (this->ctrl_if_ == nullptr)
    return false;
  const bool ok = this->ctrl_if_->write_reg(this->ctrl_if_, reg, 1, &value, 1) == ESP_CODEC_DEV_OK;
  ESP_LOGI(TAG, "ES7210 reg 0x%02X <- 0x%02X%s", reg, value, ok ? "" : " FAILED");
  return ok;
}

void AudioIO::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Audio I/O:\n"
                "  Bus: I2S0 TDM, 4 x 32-bit slots, %u Hz, MCLK x%u\n"
                "  Slots: mic1=%u mic2=%u ref=%u\n"
                "  ES7210 @0x%02X: mic PGA %.1f dB, ref ADC ch %u at %.1f dB, mic HPF coef %u\n"
                "  ES8311 @0x%02X: %s, volume %.1f dB (max %.1f dB)\n"
                "  Read size: %u frames (%u ms)\n"
                "  Status: %s",
                (unsigned) this->sample_rate_, (unsigned) MCLK_MULTIPLE, this->slot_of_[CH_MIC1],
                this->slot_of_[CH_MIC2], this->slot_of_[CH_REF], this->es7210_address_, this->mic_gain_db_,
                this->ref_adc_channel_, this->ref_gain_db_, this->mic_hpf_coef_, this->es8311_address_,
                this->tx_dev_ ? "playing" : "no playback", this->output_db_, this->volume_max_db_,
                (unsigned) this->frames_per_read_,
                (unsigned) this->frame_ms_, this->ok_ ? "running" : "FAILED");
}

// ---------------------------------------------------------------------------

void AudioIOMicrophone::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(32, NUM_OUTPUT_CHANNELS, this->parent_->get_sample_rate());
  // Sized once here so the capture task never allocates.
  this->buffer_.reserve(this->parent_->get_sample_rate() / 1000 * 40 * NUM_OUTPUT_CHANNELS * 4);
  this->parent_->add_frame_listener([this](const int32_t *frames, size_t count) { this->on_frames(frames, count); });
}

void AudioIOMicrophone::start() {
  if (this->is_failed() || this->parent_->is_failed())
    return;
  this->state_ = microphone::STATE_RUNNING;
  this->running_.store(true);
}

void AudioIOMicrophone::stop() {
  this->running_.store(false);
  this->state_ = microphone::STATE_STOPPED;
}

void AudioIOMicrophone::on_frames(const int32_t *frames, size_t count) {
  if (!this->running_.load() || this->data_callbacks_.size() == 0)
    return;
  const size_t bytes = count * NUM_OUTPUT_CHANNELS * 4;
  if (bytes > this->buffer_.capacity())
    return;
  this->buffer_.resize(bytes);
  memcpy(this->buffer_.data(), frames, bytes);
  this->data_callbacks_.call(this->buffer_);
}

// ---------------------------------------------------------------------------

void AudioIOSpeaker::setup() {
  const uint32_t rate = this->parent_->get_sample_rate();
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, rate);
  this->ring_ = ring_buffer::RingBuffer::create(rate / 1000 * this->buffer_duration_ms_ * sizeof(int16_t));
  if (this->ring_ == nullptr || !this->parent_->has_playback()) {
    ESP_LOGE(TAG, "Speaker unavailable (%s)", this->ring_ ? "no playback path" : "ring buffer allocation failed");
    this->mark_failed();
    return;
  }
  this->parent_->set_speaker(this);
  this->set_volume(this->volume_);
}

void AudioIOSpeaker::loop() {
  if (this->finishing_.load() && !this->has_buffered_data()) {
    this->finishing_.store(false);
    this->state_ = speaker::STATE_STOPPED;
  }
}

size_t AudioIOSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_failed())
    return 0;
  if (this->state_ != speaker::STATE_RUNNING)
    this->start();
  // Whole samples only, so the ring buffer never holds half of one.
  length &= ~(size_t) 1;
  return this->ring_->write_without_replacement(data, length, ticks_to_wait);
}

void AudioIOSpeaker::start() {
  if (this->is_failed())
    return;
  this->finishing_.store(false);
  this->state_ = speaker::STATE_RUNNING;
}

void AudioIOSpeaker::stop() {
  if (this->state_ == speaker::STATE_STOPPED)
    return;
  this->finishing_.store(false);
  this->discard_.store(true);
  this->state_ = speaker::STATE_STOPPED;
}

void AudioIOSpeaker::finish() {
  if (this->state_ == speaker::STATE_STOPPED)
    return;
  this->state_ = speaker::STATE_STOPPING;
  this->finishing_.store(true);
}

bool AudioIOSpeaker::has_buffered_data() const { return this->ring_ != nullptr && this->ring_->available() > 0; }

void AudioIOSpeaker::set_volume(float volume) {
  this->volume_ = clamp(volume, 0.0f, 1.0f);
  const float max_db = this->parent_->get_volume_max_db();
  this->parent_->set_output_db(this->volume_ > 0.0f ? max_db + 60.0f * log10f(this->volume_) : ES8311_MIN_DB);
}

void AudioIOSpeaker::set_mute_state(bool mute_state) {
  this->mute_state_ = mute_state;
  this->parent_->set_output_mute(mute_state);
}

size_t AudioIOSpeaker::read_for_playback(int16_t *out, size_t frames) {
  if (this->discard_.exchange(false)) {
    uint8_t scratch[256];
    while (this->ring_->read(scratch, sizeof(scratch), 0) > 0) {
    }
    return 0;
  }
  return this->ring_->read(out, frames * sizeof(int16_t), 0) / sizeof(int16_t);
}

}  // namespace esphome::audio_io

#endif  // USE_ESP32
