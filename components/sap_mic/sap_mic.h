#pragma once

#ifdef USE_ESP32

#include "esphome/components/microphone/microphone.h"
#include "esphome/core/component.h"

#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>

#include <string>

namespace esphome::sap_mic {

// Streams a microphone as RTP (L16) multicast and announces it with SAP, so a
// receiver like PipeWire's module-rtp-sap picks it up with no configuration.
//
// Passive: it forwards whatever the microphone delivers and never starts
// capture itself. If the mic is stopped (muted, diagnostics), nothing is sent
// except the announcement.
class SapMic : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_microphone(microphone::Microphone *mic) { this->mic_ = mic; }
  void set_session_name(const std::string &name) { this->session_name_ = name; }
  void set_group(const std::string &group) { this->group_config_ = group; }
  void set_port(uint16_t port) { this->port_ = port; }
  void set_packet_ms(uint32_t ms) { this->packet_ms_ = ms; }
  void set_buffer_ms(uint32_t ms) { this->buffer_ms_ = ms; }

  // Stopping sends a SAP deletion, so the receiver drops the source at once
  // instead of waiting out its cleanup timeout.
  void set_enabled(bool enabled);
  bool is_enabled() const { return this->enabled_; }

 protected:
  void on_audio_(const std::vector<uint8_t> &data);
  bool open_socket_();
  void build_announcement_(uint32_t local_ip);
  void send_sap_(bool deletion);
  void send_rtp_();

  microphone::Microphone *mic_{nullptr};
  std::string session_name_;
  std::string group_config_;
  uint16_t port_{46000};
  uint32_t packet_ms_{20};
  uint32_t buffer_ms_{500};
  bool enabled_{true};

  // Written by the mic callback (audio task), drained by loop(). One writer and
  // one reader, which is the case FreeRTOS stream buffers are built for.
  StreamBufferHandle_t stream_{nullptr};
  StaticStreamBuffer_t stream_struct_;
  uint8_t *packet_{nullptr};
  size_t payload_bytes_{0};
  uint32_t samples_per_packet_{0};
  uint32_t sample_rate_{0};
  uint8_t channels_{0};

  int sock_{-1};
  uint32_t local_ip_{0};  // network byte order; 0 until we have an address
  uint32_t group_ip_{0};  // network byte order
  std::string sap_packet_;
  uint32_t last_sap_ms_{0};
  bool announced_{false};

  uint16_t seq_{0};
  uint32_t rtp_ts_{0};
  uint32_t ssrc_{0};

  uint32_t dropped_bytes_{0};  // written from the audio task; read for logging only
  uint32_t send_errors_{0};
  uint32_t last_stats_ms_{0};
};

}  // namespace esphome::sap_mic

#endif  // USE_ESP32
