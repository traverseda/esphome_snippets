#pragma once

#ifdef USE_ESP32

#include "esphome/components/voice_frontend/voice_frontend.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <string>
#include <vector>

namespace esphome::wake_arbiter {

// Decides which speaker answers a wake word that several of them heard.
//
// claim() multicasts this speaker's score (voice_frontend's speech SNR at the
// moment of detection) three times, 30 ms apart, then waits `window` for other
// speakers' claims. Highest score wins; equal scores go to the higher device
// id. A claim that arrives after another speaker's window has already closed
// loses outright - that speaker has decided and is answering. Every decision is
// logged with every score seen.
//
// No coordinator, no state between wake words: each speaker reaches the same
// answer from the same claims. If the network loses every copy of a claim, two
// speakers can both win; Home Assistant's own duplicate-wake-word check (first
// one in wins) is still there behind this.
class WakeArbiter : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_voice_frontend(voice_frontend::VoiceFrontend *vfe) { this->vfe_ = vfe; }
  void set_group(const std::string &group, uint16_t port) {
    this->group_ = group;
    this->port_ = port;
  }
  void set_window_ms(uint32_t ms) { this->window_ms_ = ms; }
  void set_enabled(bool enabled) { this->enabled_ = enabled; }
  bool is_enabled() const { return this->enabled_; }

  // Called on wake word detection. Ends in exactly one of on_win / on_lose.
  void claim(const std::string &wake_word);
  bool is_pending() const { return this->pending_; }
  // Score of this speaker's most recent claim, dB.
  float get_last_score() const { return this->last_score_; }

  void add_on_win_callback(std::function<void(std::string)> &&cb) { this->win_cb_.add(std::move(cb)); }
  void add_on_lose_callback(std::function<void(std::string)> &&cb) { this->lose_cb_.add(std::move(cb)); }

 protected:
  struct Claim {
    uint32_t device;
    uint32_t wake_hash;
    int16_t score_tenths;
    uint32_t received_ms;
  };

  bool open_socket_();
  void send_claim_();
  void receive_();
  void decide_();
  void finish_(bool won);

  voice_frontend::VoiceFrontend *vfe_{nullptr};
  std::string group_;
  uint16_t port_{46969};
  uint32_t window_ms_{200};
  bool enabled_{true};

  int sock_{-1};
  uint32_t group_ip_{0};
  uint32_t device_id_{0};

  bool pending_{false};
  std::string wake_word_;
  uint32_t wake_hash_{0};
  int16_t score_tenths_{0};
  float last_score_{0};
  uint32_t started_ms_{0};
  uint8_t sends_{0};
  std::vector<Claim> claims_;  // other speakers', last couple of seconds
  HighFrequencyLoopRequester high_freq_;

  CallbackManager<void(std::string)> win_cb_, lose_cb_;
};

class WinTrigger : public Trigger<std::string> {
 public:
  explicit WinTrigger(WakeArbiter *parent) {
    parent->add_on_win_callback([this](const std::string &w) { this->trigger(w); });
  }
};

class LoseTrigger : public Trigger<std::string> {
 public:
  explicit LoseTrigger(WakeArbiter *parent) {
    parent->add_on_lose_callback([this](const std::string &w) { this->trigger(w); });
  }
};

}  // namespace esphome::wake_arbiter

#endif  // USE_ESP32
