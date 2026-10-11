#pragma once

#ifdef USE_ESP32

#include "esphome/components/audio_io/audio_io.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <esp_aec.h>

#include <atomic>
#include <vector>

namespace esphome::voice_frontend {

enum class MicMix : uint8_t { MIC1 = 0, MIC2 = 1, AVERAGE = 2 };
// Stages in chain order; also what the debug tap can show.
enum class Tap : uint8_t { MIX = 0, AEC = 1, NS = 2, OUTPUT = 3 };
static constexpr uint8_t NUM_STAGES = 4;

class VoiceFrontendMicrophone;

// Mono voice chain fed by audio_io: mic mix -> AEC -> NS -> AGC/limiter.
//
// Every stage exists from boot and can be bypassed at runtime without
// restarting anything; a bypassed stage copies its input through. Settings
// that need a stage rebuilt (NS mode, AGC config) are applied by the
// processing task itself between frames, never from another thread.
class VoiceFrontend : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA - 2; }

  void set_audio_io(audio_io::AudioIO *io) { this->io_ = io; }
  void set_microphone(VoiceFrontendMicrophone *mic) { this->mic_ = mic; }

  // --- Runtime controls (safe from the main loop) ---
  void set_mic_mix(MicMix mix) { this->mix_.store(mix); }
  // Digital gain on the mic mix, applied in the 24 -> 16 bit conversion -
  // before AEC, so keep it low enough that our own playback never clips.
  void set_input_gain_db(float db);
  float get_input_gain_db() const { return this->input_gain_db_; }
  // Digital gain after AEC, before NS: brings speech up to working level.
  void set_makeup_gain_db(float db);
  float get_makeup_gain_db() const { return this->makeup_gain_db_; }
  void configure_aec(int mode, int filter_length, int nlp_level) {
    this->aec_mode_ = mode;
    this->aec_filter_length_ = filter_length;
    this->aec_nlp_level_ = nlp_level;
  }
  void set_aec_enabled(bool on) { this->aec_on_.store(on); }
  void set_ns_enabled(bool on) { this->ns_on_.store(on); }
  void set_ns_mode(int mode) {
    this->ns_mode_.store(mode);
    this->ns_dirty_.store(true);
  }
  void set_agc_enabled(bool on) { this->agc_on_.store(on); }
  void set_agc_config(int target_level_dbfs, int compression_gain_db, bool limiter) {
    this->agc_target_.store(target_level_dbfs);
    this->agc_gain_.store(compression_gain_db);
    this->agc_limiter_.store(limiter);
    this->agc_dirty_.store(true);
  }
  void set_tap(Tap tap) { this->tap_.store(tap); }
  // Muted: both output channels carry silence. The chain keeps running, so AEC
  // stays converged and unmuting is instant.
  void set_muted(bool muted) { this->muted_.store(muted); }
  bool is_muted() const { return this->muted_.load(); }

  bool is_aec_enabled() const { return this->aec_on_.load(); }
  bool is_ns_enabled() const { return this->ns_on_.load(); }
  bool is_agc_enabled() const { return this->agc_on_.load(); }
  int get_ns_mode() const { return this->ns_mode_.load(); }
  int get_agc_target() const { return this->agc_target_.load(); }
  int get_agc_gain() const { return this->agc_gain_.load(); }
  MicMix get_mic_mix() const { return this->mix_.load(); }
  Tap get_tap() const { return this->tap_.load(); }

  // RMS of each stage's output over the last stats window, dBFS.
  float get_stage_level(Tap stage) const { return this->levels_[(uint8_t) stage]; }
  uint32_t get_dropped_frames() const { return this->dropped_frames_.load(); }

  // How well this device is hearing speech right now: the loudest 100 ms of
  // the processed output over the last 1.5 s, minus the tracked background
  // level, dB. Read at wake-word time it says how clearly the wake word came
  // through - the figure wake_arbiter compares between speakers.
  float get_speech_snr_db() const { return this->wake_peak_db_.load() - this->noise_floor_db_.load(); }
  float get_noise_floor_db() const { return this->noise_floor_db_.load(); }

 protected:
  void on_capture_(const int32_t *frames, size_t count);
  static void process_task_(void *arg);
  void process_loop_();
  void run_ns_agc_(int16_t *mix, int16_t *aec, size_t n);
  void apply_pending_();
  void update_speech_score_(const int16_t *out, size_t n);

  audio_io::AudioIO *io_{nullptr};
  VoiceFrontendMicrophone *mic_{nullptr};

  // Configuration / runtime switches.
  int aec_mode_{0}, aec_filter_length_{4}, aec_nlp_level_{1};
  std::atomic<MicMix> mix_{MicMix::AVERAGE};
  float input_gain_db_{0};
  std::atomic<int32_t> input_gain_q16_{65536};
  std::atomic<uint32_t> input_clipped_{0};
  float makeup_gain_db_{0};
  std::atomic<int32_t> makeup_gain_q16_{65536};
  std::atomic<uint32_t> makeup_clipped_{0};
  std::atomic<Tap> tap_{Tap::MIX};
  std::atomic<bool> aec_on_{true}, ns_on_{true}, agc_on_{true};
  std::atomic<int> ns_mode_{2}, agc_target_{3}, agc_gain_{24};
  std::atomic<bool> agc_limiter_{true};
  std::atomic<bool> muted_{false};
  std::atomic<bool> ns_dirty_{false}, agc_dirty_{false};

  // Capture task -> processing task: interleaved int16 [mic, ref] frames.
  StreamBufferHandle_t queue_{nullptr};
  StaticStreamBuffer_t queue_struct_;
  int16_t *capture_scratch_{nullptr};
  size_t capture_scratch_frames_{0};
  std::atomic<uint32_t> dropped_frames_{0};

  // Processing state, touched only by the processing task.
  TaskHandle_t task_{nullptr};
  aec_handle_t *aec_{nullptr};
  void *ns_{nullptr};
  void *agc_{nullptr};
  size_t chunk_{0};  // AEC frame size, samples
  int16_t *in_{nullptr};     // interleaved [mic, ref] x chunk_
  int16_t *mic_buf_{nullptr}, *ref_buf_{nullptr}, *aec_buf_{nullptr};
  int16_t *fifo_mix_{nullptr}, *fifo_aec_{nullptr};  // chunk_ + one NS frame
  size_t fifo_len_{0};
  int16_t *ns_buf_{nullptr}, *out_buf_{nullptr}, *emit_buf_{nullptr};

  // Speech score, processing task only (atomics are the published results).
  static constexpr uint8_t SCORE_BLOCKS = 15;  // x 100 ms
  double score_acc_{0};
  uint32_t score_n_{0};
  float score_blocks_[SCORE_BLOCKS]{-120, -120, -120, -120, -120, -120, -120, -120,
                                    -120, -120, -120, -120, -120, -120, -120};
  uint8_t score_head_{0};
  float floor_db_{-60.0f};
  std::atomic<float> wake_peak_db_{-120.0f}, noise_floor_db_{-60.0f};

  // Per-stage levels.
  double acc_sq_[NUM_STAGES]{};
  uint32_t acc_n_{0};
  uint32_t acc_started_ms_{0};
  float levels_[NUM_STAGES]{-120, -120, -120, -120};
  std::atomic<bool> levels_ready_{false};
};

class VoiceFrontendMicrophone : public microphone::Microphone, public Component, public Parented<VoiceFrontend> {
 public:
  void setup() override;
  void start() override;
  void stop() override;
  float get_setup_priority() const override { return setup_priority::DATA - 3; }

  // Called from the processing task: interleaved int16 [output, tap] x count.
  void on_frames(const int16_t *frames, size_t count);

 protected:
  std::vector<uint8_t> buffer_;
  std::atomic<bool> running_{false};
  uint8_t users_{0};  // started MicrophoneSources; start()/stop() come from the main loop
};

}  // namespace esphome::voice_frontend

#endif  // USE_ESP32
