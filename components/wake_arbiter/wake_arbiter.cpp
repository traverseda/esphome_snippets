#include "wake_arbiter.h"

#ifdef USE_ESP32

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <lwip/inet.h>
#include <lwip/sockets.h>

#include <cerrno>
#include <cmath>
#include <cstring>

namespace esphome::wake_arbiter {

static const char *const TAG = "wake_arbiter";

static constexpr uint32_t MAGIC = 0x57415242;  // "WARB"
static constexpr uint8_t VERSION = 1;
static constexpr uint8_t SENDS = 3;
static constexpr uint32_t RESEND_MS = 30;
// Claims older than this are history, whatever wake word they were for.
static constexpr uint32_t CLAIM_MEMORY_MS = 2000;

// On the wire: all integers big-endian.
struct __attribute__((packed)) Packet {
  uint32_t magic;
  uint8_t version;
  uint8_t reserved[3];
  uint32_t device;
  uint32_t wake_hash;
  int16_t score_tenths;  // dB x 10
  uint16_t reserved2;
};

void WakeArbiter::setup() { this->device_id_ = fnv1_hash(App.get_name().str()); }

bool WakeArbiter::open_socket_() {
  if (this->sock_ >= 0)
    return true;
  if (!network::is_connected())
    return false;
  this->sock_ = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (this->sock_ < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    return false;
  }
  int one = 1;
  lwip_setsockopt(this->sock_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(this->port_);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (lwip_bind(this->sock_, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind(%u) failed: errno %d", this->port_, errno);
    lwip_close(this->sock_);
    this->sock_ = -1;
    return false;
  }
  this->group_ip_ = inet_addr(this->group_.c_str());
  struct ip_mreq mreq = {};
  mreq.imr_multiaddr.s_addr = this->group_ip_;
  mreq.imr_interface.s_addr = htonl(INADDR_ANY);
  if (lwip_setsockopt(this->sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
    ESP_LOGW(TAG, "Joining %s failed: errno %d", this->group_.c_str(), errno);
  uint8_t ttl = 1, loop = 0;
  lwip_setsockopt(this->sock_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  lwip_setsockopt(this->sock_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
  int flags = lwip_fcntl(this->sock_, F_GETFL, 0);
  lwip_fcntl(this->sock_, F_SETFL, flags | O_NONBLOCK);
  ESP_LOGD(TAG, "Listening on %s:%u", this->group_.c_str(), this->port_);
  return true;
}

void WakeArbiter::loop() {
  if (!this->open_socket_())
    return;
  this->receive_();
  if (!this->pending_)
    return;
  const uint32_t elapsed = millis() - this->started_ms_;
  if (this->sends_ < SENDS && elapsed >= this->sends_ * RESEND_MS)
    this->send_claim_();
  if (elapsed >= this->window_ms_)
    this->decide_();
}

void WakeArbiter::claim(const std::string &wake_word) {
  if (this->pending_) {
    ESP_LOGD(TAG, "Already arbitrating '%s'; ignoring another detection", this->wake_word_.c_str());
    return;
  }
  const float score = this->vfe_ != nullptr ? this->vfe_->get_speech_snr_db() : 0.0f;
  this->last_score_ = score;
  this->wake_word_ = wake_word;
  if (!this->enabled_ || !this->open_socket_()) {
    ESP_LOGI(TAG, "'%s' heard at %.1f dB SNR; arbitration %s, answering", wake_word.c_str(), score,
             this->enabled_ ? "unavailable (no network)" : "off");
    this->win_cb_.call(wake_word);
    return;
  }
  this->wake_hash_ = fnv1_hash(wake_word);
  this->score_tenths_ = (int16_t) clamp(lroundf(score * 10.0f), -32000L, 32000L);
  this->started_ms_ = millis();
  this->sends_ = 0;
  this->pending_ = true;
  this->high_freq_.start();
  this->send_claim_();
}

void WakeArbiter::send_claim_() {
  Packet p = {};
  p.magic = htonl(MAGIC);
  p.version = VERSION;
  p.device = htonl(this->device_id_);
  p.wake_hash = htonl(this->wake_hash_);
  p.score_tenths = (int16_t) htons((uint16_t) this->score_tenths_);
  struct sockaddr_in dest = {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(this->port_);
  dest.sin_addr.s_addr = this->group_ip_;
  if (lwip_sendto(this->sock_, &p, sizeof(p), MSG_DONTWAIT, (struct sockaddr *) &dest, sizeof(dest)) < 0)
    ESP_LOGW(TAG, "sendto failed: errno %d", errno);
  this->sends_++;
}

void WakeArbiter::receive_() {
  const uint32_t now = millis();
  Packet p;
  while (true) {
    const int len = lwip_recvfrom(this->sock_, &p, sizeof(p), MSG_DONTWAIT, nullptr, nullptr);
    if (len < 0)
      break;
    if (len != (int) sizeof(p) || ntohl(p.magic) != MAGIC || p.version != VERSION)
      continue;
    const uint32_t device = ntohl(p.device);
    if (device == this->device_id_)
      continue;
    const uint32_t wake_hash = ntohl(p.wake_hash);
    const int16_t score = (int16_t) ntohs((uint16_t) p.score_tenths);
    bool known = false;
    for (auto &c : this->claims_) {
      // A resend of the same claim: keep the first arrival time.
      if (c.device == device && c.wake_hash == wake_hash && now - c.received_ms < CLAIM_MEMORY_MS) {
        c.score_tenths = score;
        known = true;
        break;
      }
    }
    if (!known)
      this->claims_.push_back({device, wake_hash, score, now});
  }
  // Forget old claims.
  for (size_t i = 0; i < this->claims_.size();) {
    if (now - this->claims_[i].received_ms > CLAIM_MEMORY_MS) {
      this->claims_[i] = this->claims_.back();
      this->claims_.pop_back();
    } else {
      i++;
    }
  }
}

void WakeArbiter::decide_() {
  bool won = true;
  bool late = false;
  std::string seen;
  for (const auto &c : this->claims_) {
    if (c.wake_hash != this->wake_hash_)
      continue;
    // Arrived before our claim by more than a window: that speaker's window
    // closed before ours opened, so it has already decided.
    const int32_t lead = (int32_t) (this->started_ms_ - c.received_ms);
    if (lead > (int32_t) this->window_ms_) {
      late = true;
      won = false;
    } else if (c.score_tenths > this->score_tenths_ ||
               (c.score_tenths == this->score_tenths_ && c.device > this->device_id_)) {
      won = false;
    }
    char buf[48];
    snprintf(buf, sizeof(buf), " %08X:%.1f%s", (unsigned) c.device, c.score_tenths / 10.0f,
             lead > (int32_t) this->window_ms_ ? "(earlier)" : "");
    seen += buf;
  }
  ESP_LOGI(TAG, "'%s': us %.1f dB, others [%s ] -> %s", this->wake_word_.c_str(), this->score_tenths_ / 10.0f,
           seen.c_str(), won ? "ANSWER" : (late ? "stand down (another speaker already answered)" : "stand down"));
  this->finish_(won);
}

void WakeArbiter::finish_(bool won) {
  this->pending_ = false;
  this->high_freq_.stop();
  const std::string word = this->wake_word_;
  if (won) {
    this->win_cb_.call(word);
  } else {
    this->lose_cb_.call(word);
  }
}

void WakeArbiter::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Wake arbiter:\n"
                "  Group: %s:%u\n"
                "  Window: %u ms\n"
                "  Device id: %08X\n"
                "  Enabled: %s",
                this->group_.c_str(), this->port_, (unsigned) this->window_ms_, (unsigned) this->device_id_,
                YESNO(this->enabled_));
}

}  // namespace esphome::wake_arbiter

#endif  // USE_ESP32
