#pragma once

#ifdef USE_ESP32

#include "esphome/components/i2c/i2c_bus.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <driver/i2s_tdm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <audio_codec_ctrl_if.h>
#include <audio_codec_data_if.h>
#include <audio_codec_if.h>
#include <esp_codec_dev.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace esphome::audio_io {

// Output channel order every consumer sees, independent of the TDM slot layout.
enum OutputChannel : uint8_t { CH_MIC1 = 0, CH_MIC2 = 1, CH_REF = 2, NUM_OUTPUT_CHANNELS = 3 };
static constexpr uint8_t TDM_SLOTS = 4;
// Octave bands the playback meter splits audio into: 100 Hz .. 6.4 kHz.
static constexpr uint8_t NUM_METER_BANDS = 7;

class AudioIOMicrophone;
class AudioIOSpeaker;

struct ChannelStats {
  float rms_dbfs{-120.0f};
  float peak_dbfs{-120.0f};
  float dc_dbfs{-120.0f};
  uint32_t clipped{0};
};

// Owns the I2S TDM bus, the ES7210 (capture) and the ES8311 (playback).
//
// Capture: a dedicated task reads whole TDM frames, de-interleaves the slots
// into a fixed [mic1, mic2, ref] order, keeps per-channel statistics and hands
// each frame to its listeners. Nothing here filters, gains or mixes: what comes
// out is what the ADC produced.
//
// Playback: a second task writes one DMA descriptor (10 ms) at a time, always,
// silence when the speaker has nothing - so the bus clock never stops and the
// pacing comes from the DMA itself. Volume is the ES8311's own register, so
// nothing in software ever attenuates (or boosts) samples.
class AudioIO : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_i2c_bus(i2c::I2CBus *bus) { this->i2c_bus_ = bus; }
  void set_pins(int mclk, int bclk, int lrclk, int din, int dout) {
    this->mclk_pin_ = mclk;
    this->bclk_pin_ = bclk;
    this->lrclk_pin_ = lrclk;
    this->din_pin_ = din;
    this->dout_pin_ = dout;
  }
  void set_es8311(uint8_t address, float volume_max_db) {
    this->es8311_address_ = address;
    this->volume_max_db_ = volume_max_db;
  }
  void set_speaker(AudioIOSpeaker *speaker) { this->speaker_ = speaker; }
  void set_sample_rate(uint32_t rate) { this->sample_rate_ = rate; }
  void set_frame_ms(uint32_t ms) { this->frame_ms_ = ms; }
  void set_es7210(uint8_t address, float mic_gain_db, uint8_t ref_adc_channel, float ref_gain_db) {
    this->es7210_address_ = address;
    this->mic_gain_db_ = mic_gain_db;
    this->ref_adc_channel_ = ref_adc_channel;
    this->ref_gain_db_ = ref_gain_db;
  }
  void set_slots(uint8_t mic1, uint8_t mic2, uint8_t ref) {
    this->slot_of_[CH_MIC1] = mic1;
    this->slot_of_[CH_MIC2] = mic2;
    this->slot_of_[CH_REF] = ref;
  }
  void set_stats_interval(uint32_t ms) { this->stats_interval_ms_ = ms; }

  void set_mic_hpf_coefficient(uint8_t coef) { this->mic_hpf_coef_ = coef; }

  uint32_t get_sample_rate() const { return this->sample_rate_; }

  // Called from the capture task for every read: NUM_OUTPUT_CHANNELS
  // interleaved int32 samples in [mic1, mic2, ref] order. Listeners must not
  // block or allocate. Register during setup only.
  using FrameListener = std::function<void(const int32_t *frames, size_t count)>;
  void add_frame_listener(FrameListener listener) { this->listeners_.push_back(std::move(listener)); }

  // --- Runtime controls (call from the main loop) ---
  // Analog PGA on every mic channel (not the reference). 0..37.5 dB.
  bool set_mic_gain_db(float db);
  // Analog PGA on one physical ADC channel, MIC1=0 .. MIC4=3.
  bool set_adc_channel_gain_db(uint8_t adc_channel, float db);
  float get_mic_gain_db() const { return this->mic_gain_db_; }
  // ES7210 high-pass on the two mic ADCs (registers 0x22/0x23). The datasheet
  // calls it a DC filter and documents only 6 ("fast") and 13 ("slow"); measured
  // on this board, lower values raise the cutoff: 3 removes ~18 dB below 200 Hz
  // for ~2 dB of speech band. 0 is unstable and >= 20 locks the output, so this
  // only accepts 1..13. 10 is what esp_codec_dev programs by default.
  bool set_mic_hpf(uint8_t coef);
  uint8_t get_mic_hpf() const { return this->mic_hpf_coef_; }
  // Raw ES7210 register access, for experiments the codec library doesn't
  // expose (its high-pass filter, mic bias, ...). Returns -1 on failure.
  int read_es7210_reg(uint8_t reg);
  bool write_es7210_reg(uint8_t reg, uint8_t value);

  // --- Playback (call from the main loop) ---
  // ES8311 DAC volume in dB, 0.5 dB steps, -95.5 .. +32. Clamped to volume_max_db.
  bool set_output_db(float db);
  float get_output_db() const { return this->output_db_; }
  float get_volume_max_db() const { return this->volume_max_db_; }
  bool set_output_mute(bool mute);
  bool has_playback() const { return this->tx_dev_ != nullptr; }

  // Playback meter, for LED effects. 0..1, computed every 10 ms from the
  // samples going to the DAC (before its volume), auto-ranged so a quiet track
  // moves the ring as much as a loud one. Fast attack, ~150 ms release, 0 when
  // nothing plays. Lock-free.
  float get_playback_meter() const { return this->meter_level_.load(std::memory_order_relaxed); }
  float get_playback_band_meter(uint8_t band) const {
    return band < NUM_METER_BANDS ? this->meter_band_[band].load(std::memory_order_relaxed) : 0.0f;
  }

  // RMS of the most recent read (one frame_ms), dBFS, indexed by OutputChannel.
  // Lock-free; cheap enough to poll from an LED effect.
  float get_level_dbfs(uint8_t channel) const { return this->fast_level_[channel].load(std::memory_order_relaxed); }

  // Latest per-channel statistics, indexed by OutputChannel.
  const ChannelStats &get_stats(uint8_t channel) const { return this->stats_[channel]; }
  uint32_t get_read_errors() const { return this->read_errors_.load(); }
  uint32_t get_rx_overflows() const { return this->rx_overflows_.load(); }

 protected:
  bool init_i2s_();
  bool init_codec_();
  const audio_codec_ctrl_if_t *new_ctrl_(uint8_t address);
  static void capture_task_(void *arg);
  void capture_loop_();
  static void playback_task_(void *arg);
  void playback_loop_();
  void update_meter_(const int16_t *pcm, size_t n);
  void accumulate_stats_(const int32_t *frames, size_t count);
  static bool IRAM_ATTR on_rx_overflow_(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx);

  i2c::I2CBus *i2c_bus_{nullptr};
  int mclk_pin_{-1}, bclk_pin_{-1}, lrclk_pin_{-1}, din_pin_{-1}, dout_pin_{-1};
  uint32_t sample_rate_{16000};
  uint32_t frame_ms_{20};
  uint8_t es7210_address_{0x40};
  float mic_gain_db_{30.0f};
  uint8_t ref_adc_channel_{2};
  float ref_gain_db_{0.0f};
  uint8_t slot_of_[NUM_OUTPUT_CHANNELS]{0, 2, 1};
  uint32_t stats_interval_ms_{10000};
  uint8_t mic_hpf_coef_{10};
  uint8_t es8311_address_{0x18};
  float volume_max_db_{0.0f};
  float output_db_{0.0f};
  AudioIOSpeaker *speaker_{nullptr};

  i2s_chan_handle_t rx_handle_{nullptr};
  i2s_chan_handle_t tx_handle_{nullptr};
  const audio_codec_ctrl_if_t *ctrl_if_{nullptr};  // ES7210
  const audio_codec_ctrl_if_t *dac_ctrl_if_{nullptr};  // ES8311
  const audio_codec_data_if_t *data_if_{nullptr};
  const audio_codec_if_t *codec_if_{nullptr};
  const audio_codec_if_t *dac_codec_if_{nullptr};
  esp_codec_dev_handle_t rx_dev_{nullptr};
  esp_codec_dev_handle_t tx_dev_{nullptr};

  TaskHandle_t task_{nullptr};
  TaskHandle_t tx_task_{nullptr};
  std::vector<FrameListener> listeners_;

  size_t frames_per_read_{0};
  int32_t *tdm_buf_{nullptr};  // interleaved TDM_SLOTS x frames
  int32_t *out_buf_{nullptr};  // interleaved NUM_OUTPUT_CHANNELS x frames

  // Accumulators, touched only by the capture task.
  double acc_sq_[NUM_OUTPUT_CHANNELS]{};
  double acc_sum_[NUM_OUTPUT_CHANNELS]{};
  int64_t acc_peak_[NUM_OUTPUT_CHANNELS]{};
  uint32_t acc_clipped_[NUM_OUTPUT_CHANNELS]{};
  uint64_t acc_frames_{0};
  uint32_t acc_started_ms_{0};

  // Published by the capture task, read by loop().
  ChannelStats stats_[NUM_OUTPUT_CHANNELS];
  std::atomic<float> fast_level_[NUM_OUTPUT_CHANNELS]{-120.0f, -120.0f, -120.0f};

  // Playback meter state, touched only by the playback task.
  struct Biquad {
    float b0, b1, b2, a1, a2, z1, z2;
  };
  Biquad meter_filters_[NUM_METER_BANDS]{};
  float meter_ceiling_db_{-30.0f};
  float meter_shown_[NUM_METER_BANDS + 1]{};
  std::atomic<float> meter_level_{0.0f};
  std::atomic<float> meter_band_[NUM_METER_BANDS]{};
  std::atomic<bool> stats_ready_{false};
  std::atomic<uint32_t> read_errors_{0};
  std::atomic<uint32_t> rx_overflows_{0};
  std::atomic<uint32_t> write_errors_{0};
  bool ok_{false};
};

class AudioIOMicrophone : public microphone::Microphone, public Component, public Parented<AudioIO> {
 public:
  void setup() override;
  void start() override;
  void stop() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1; }

  // Called from the capture task with NUM_OUTPUT_CHANNELS interleaved int32 samples.
  void on_frames(const int32_t *frames, size_t count);

 protected:
  std::vector<uint8_t> buffer_;
  std::atomic<bool> running_{false};
};

// Stock ESPHome speaker on top of AudioIO: 16-bit mono at the bus rate, into a
// PSRAM ring buffer the playback task drains. The mixer, resamplers, media
// players and sendspin sit above this unchanged.
//
// The playback task always runs, so start() and stop() only change what it
// finds in the ring buffer; nothing here touches the I2S driver.
class AudioIOSpeaker : public speaker::Speaker, public Component, public Parented<AudioIO> {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::DATA - 1; }

  void set_buffer_duration(uint32_t ms) { this->buffer_duration_ms_ = ms; }

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }
  void start() override;
  void stop() override;
  void finish() override;
  bool has_buffered_data() const override;

  // Cubic: 50% is -18 dB below volume_max_db, the same curve PipeWire, PulseAudio and HA use.
  void set_volume(float volume) override;
  void set_mute_state(bool mute_state) override;

  // --- Playback task side ---
  // Copies up to `frames` samples into `out` without blocking; returns how many.
  size_t read_for_playback(int16_t *out, size_t frames);
  void report_played(uint32_t frames, int64_t finish_us) { this->audio_output_callback_.call(frames, finish_us); }

 protected:
  uint32_t buffer_duration_ms_{500};
  std::unique_ptr<ring_buffer::RingBuffer> ring_;
  // stop() asks the playback task to empty the ring buffer, rather than
  // resetting it under the task's feet.
  std::atomic<bool> discard_{false};
  std::atomic<bool> finishing_{false};
};

}  // namespace esphome::audio_io

#endif  // USE_ESP32
