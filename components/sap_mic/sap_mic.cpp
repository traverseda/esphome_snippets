#include "sap_mic.h"

#ifdef USE_ESP32

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>

#include <cerrno>
#include <cstring>

namespace esphome::sap_mic {

static const char *const TAG = "sap_mic";

// The address and port PipeWire's module-rtp-sap (and AES67 gear generally)
// listens on. 224.0.0.x is link-local: never routed, and flooded rather than
// IGMP-filtered by switches, so announcements reach every host on the segment.
static constexpr const char *SAP_GROUP = "224.0.0.56";
static constexpr uint16_t SAP_PORT = 9875;
// PipeWire drops a session 90 s after its last announcement.
static constexpr uint32_t SAP_INTERVAL_MS = 5000;
static constexpr uint8_t RTP_PAYLOAD_TYPE = 96;  // dynamic, mapped to L16 by the SDP
static constexpr size_t RTP_HEADER_BYTES = 12;
// Bound the work done per loop() so a backlog after a stall can't hog the main loop.
static constexpr int MAX_PACKETS_PER_LOOP = 8;

void SapMic::setup() {
  const audio::AudioStreamInfo info = this->source_->get_audio_stream_info();
  if (info.get_bits_per_sample() != 16) {
    ESP_LOGE(TAG, "Only 16-bit microphones are supported (got %u-bit)", info.get_bits_per_sample());
    this->mark_failed();
    return;
  }
  this->sample_rate_ = info.get_sample_rate();
  this->channels_ = info.get_channels();
  if (this->sample_rate_ == 0 || this->channels_ == 0) {
    ESP_LOGE(TAG, "Microphone has no stream format yet; is it set up before this component?");
    this->mark_failed();
    return;
  }
  const size_t frame_bytes = 2 * this->channels_;
  // Keep each RTP packet inside one Ethernet frame: IP fragments of a
  // multicast stream over WiFi are lost far more often than whole packets.
  // Multi-channel debug streams therefore get shorter packets.
  static constexpr size_t MAX_PAYLOAD_BYTES = 1400;
  while (this->packet_ms_ > 5 && this->sample_rate_ * this->packet_ms_ / 1000 * frame_bytes > MAX_PAYLOAD_BYTES)
    this->packet_ms_ /= 2;
  this->samples_per_packet_ = this->sample_rate_ * this->packet_ms_ / 1000;
  this->payload_bytes_ = this->samples_per_packet_ * frame_bytes;
  const size_t buffer_bytes = this->sample_rate_ * this->buffer_ms_ / 1000 * frame_bytes;

  // PSRAM first: internal RAM is the scarce resource on the boards this runs on.
  const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  auto *storage = static_cast<uint8_t *>(heap_caps_malloc_prefer(buffer_bytes + 1, 2, caps, MALLOC_CAP_8BIT));
  this->packet_ = static_cast<uint8_t *>(
      heap_caps_malloc_prefer(RTP_HEADER_BYTES + this->payload_bytes_, 2, caps, MALLOC_CAP_8BIT));
  if (storage == nullptr || this->packet_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate %u-byte audio buffer", (unsigned) buffer_bytes);
    this->mark_failed();
    return;
  }
  this->stream_ = xStreamBufferCreateStatic(buffer_bytes, 1, storage, &this->stream_struct_);

  if (this->session_name_.empty())
    this->session_name_ = App.get_friendly_name().str() + " mic";
  // Stable across reboots, unlike the timestamp and sequence start. A receiver
  // locks onto the first SSRC it sees, and the SAP announcement is identical
  // after a reboot, so a fresh random SSRC would leave PipeWire holding the old
  // session and silently rejecting every new packet ("unexpected SSRC").
  this->ssrc_ = fnv1_hash(App.get_name().str() + "/sap_mic") | 1;
  this->rtp_ts_ = esp_random();
  this->seq_ = esp_random() & 0xFFFF;

  this->source_->add_data_callback([this](const std::vector<uint8_t> &data) { this->on_audio_(data); });
}

// Runs on the audio task: no blocking, no allocation, no network I/O.
void SapMic::on_audio_(const std::vector<uint8_t> &data) {
  if (!this->enabled_ || this->stream_ == nullptr)
    return;
  // All or nothing. A stream buffer accepts partial writes, and a write cut
  // mid-sample shifts every later sample by a byte - the receiver then decodes
  // low bytes as high bytes and hears noise at full scale. This task is the
  // only writer, so the free space can only grow between check and send.
  if (xStreamBufferSpacesAvailable(this->stream_) < data.size()) {
    this->dropped_bytes_ += data.size();
    return;
  }
  xStreamBufferSend(this->stream_, data.data(), data.size(), 0);
}

void SapMic::drain_() {
  uint8_t scratch[64];
  while (xStreamBufferReceive(this->stream_, scratch, sizeof(scratch), 0) > 0) {
  }
}

void SapMic::set_enabled(bool enabled) {
  if (enabled == this->enabled_)
    return;
  this->enabled_ = enabled;
  if (!enabled && this->announced_) {
    this->send_sap_(true);
    this->announced_ = false;
  }
  ESP_LOGI(TAG, "Streaming %s", enabled ? "enabled" : "disabled");
}

void SapMic::loop() {
  if (this->stream_ == nullptr)
    return;

  if (this->gain_source_ != nullptr && this->gain_source_->get_gain_factor() != this->source_->get_gain_factor())
    this->source_->set_gain_factor(this->gain_source_->get_gain_factor());

  if (!this->enabled_) {
    // Drain anything left from before the switch-off, so re-enabling starts
    // with fresh audio rather than a stale half second.
    this->drain_();
    return;
  }

  if (!network::is_connected()) {
    this->local_ip_ = 0;
    this->announced_ = false;
    this->drain_();
    return;
  }

  uint32_t ip = 0;
  for (auto &addr : network::get_ip_addresses()) {
    if (addr.is_set() && addr.is_ip4()) {
      char buf[network::IP_ADDRESS_BUFFER_SIZE];
      addr.str_to(buf);
      ip = inet_addr(buf);
      break;
    }
  }
  if (ip == 0)
    return;
  if (ip != this->local_ip_) {
    if (this->sock_ < 0 && !this->open_socket_())
      return;
    this->build_announcement_(ip);
    this->announced_ = false;
  }

  const uint32_t now = millis();
  if (!this->announced_ || now - this->last_sap_ms_ >= SAP_INTERVAL_MS) {
    this->send_sap_(false);
    this->last_sap_ms_ = now;
    this->announced_ = true;
  }

  this->send_rtp_();

  if (now - this->last_stats_ms_ >= 10000) {
    if (this->dropped_bytes_ > 0 || this->send_errors_ > 0) {
      ESP_LOGW(TAG, "Last 10s: dropped %u ms of audio (buffer full), %u send errors",
               (unsigned) (this->dropped_bytes_ / (2 * this->channels_) * 1000 / this->sample_rate_),
               (unsigned) this->send_errors_);
      this->dropped_bytes_ = 0;
      this->send_errors_ = 0;
    }
    this->last_stats_ms_ = now;
  }
}

bool SapMic::open_socket_() {
  this->sock_ = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (this->sock_ < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    return false;
  }
  // TTL 1: this is for the local network only, never routed.
  uint8_t ttl = 1;
  lwip_setsockopt(this->sock_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
  int flags = lwip_fcntl(this->sock_, F_GETFL, 0);
  lwip_fcntl(this->sock_, F_SETFL, flags | O_NONBLOCK);
  return true;
}

void SapMic::build_announcement_(uint32_t local_ip) {
  this->local_ip_ = local_ip;
  const uint8_t *ip = reinterpret_cast<const uint8_t *>(&local_ip);
  if (!this->group_config_.empty()) {
    this->group_ip_ = inet_addr(this->group_config_.c_str());
  } else {
    const uint8_t group[4] = {239, 69, ip[2], ip[3]};
    memcpy(&this->group_ip_, group, 4);
  }

  char local_str[16], group_str[16];
  inet_ntoa_r(*reinterpret_cast<const struct in_addr *>(&this->local_ip_), local_str, sizeof(local_str));
  inet_ntoa_r(*reinterpret_cast<const struct in_addr *>(&this->group_ip_), group_str, sizeof(group_str));

  // Stable session id: a receiver that already knows this device keeps the
  // same source across reboots instead of growing a duplicate.
  const uint32_t sess_id = fnv1_hash(App.get_name().str());
  // The version must change whenever the description does (here: the address).
  const uint32_t sess_version = ntohl(local_ip);

  char sdp[512];
  int len = snprintf(sdp, sizeof(sdp),
                     "v=0\r\n"
                     "o=- %u %u IN IP4 %s\r\n"
                     "s=%s\r\n"
                     "c=IN IP4 %s/1\r\n"
                     "t=0 0\r\n"
                     "m=audio %u RTP/AVP %u\r\n"
                     "a=rtpmap:%u L16/%u/%u\r\n"
                     "a=ptime:%u\r\n"
                     "a=recvonly\r\n",
                     (unsigned) sess_id, (unsigned) sess_version, local_str, this->session_name_.c_str(), group_str,
                     this->port_, RTP_PAYLOAD_TYPE, RTP_PAYLOAD_TYPE, (unsigned) this->sample_rate_,
                     this->channels_, (unsigned) this->packet_ms_);
  if (len < 0 || len >= (int) sizeof(sdp))
    len = sizeof(sdp) - 1;

  // SAP header (RFC 2974): V=1, IPv4 origin, no auth, no compression, then a
  // 16-bit message id hash, then the originating address. The deletion flag
  // is patched into byte 0 at send time.
  const uint16_t msg_hash = fnv1_hash(std::string(sdp, len)) & 0xFFFF;
  this->sap_packet_.clear();
  this->sap_packet_.push_back(0x20);
  this->sap_packet_.push_back(0);
  this->sap_packet_.push_back(msg_hash >> 8);
  this->sap_packet_.push_back(msg_hash & 0xFF);
  this->sap_packet_.append(reinterpret_cast<const char *>(&this->local_ip_), 4);
  this->sap_packet_.append("application/sdp", sizeof("application/sdp"));  // includes the NUL
  this->sap_packet_.append(sdp, len);

  ESP_LOGI(TAG, "Announcing '%s' as RTP L16/%u/%u to %s:%u", this->session_name_.c_str(),
           (unsigned) this->sample_rate_, this->channels_, group_str, this->port_);
}

void SapMic::send_sap_(bool deletion) {
  if (this->sock_ < 0 || this->sap_packet_.empty())
    return;
  this->sap_packet_[0] = deletion ? 0x24 : 0x20;
  struct sockaddr_in dest {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(SAP_PORT);
  dest.sin_addr.s_addr = inet_addr(SAP_GROUP);
  if (lwip_sendto(this->sock_, this->sap_packet_.data(), this->sap_packet_.size(), MSG_DONTWAIT,
                  reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest)) < 0)
    this->send_errors_++;
}

void SapMic::send_rtp_() {
  struct sockaddr_in dest {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(this->port_);
  dest.sin_addr.s_addr = this->group_ip_;

  uint8_t *payload = this->packet_ + RTP_HEADER_BYTES;
  for (int i = 0; i < MAX_PACKETS_PER_LOOP; i++) {
    if (xStreamBufferBytesAvailable(this->stream_) < this->payload_bytes_)
      return;
    xStreamBufferReceive(this->stream_, payload, this->payload_bytes_, 0);

    // L16 is big-endian on the wire; the mic hands us little-endian.
    for (size_t j = 0; j < this->payload_bytes_; j += 2)
      std::swap(payload[j], payload[j + 1]);

    uint8_t *h = this->packet_;
    h[0] = 0x80;  // V=2, no padding/extension/CSRC
    h[1] = RTP_PAYLOAD_TYPE;
    h[2] = this->seq_ >> 8;
    h[3] = this->seq_ & 0xFF;
    h[4] = this->rtp_ts_ >> 24;
    h[5] = this->rtp_ts_ >> 16;
    h[6] = this->rtp_ts_ >> 8;
    h[7] = this->rtp_ts_;
    h[8] = this->ssrc_ >> 24;
    h[9] = this->ssrc_ >> 16;
    h[10] = this->ssrc_ >> 8;
    h[11] = this->ssrc_;

    // Sequence and timestamp advance even if the send fails, so the receiver
    // sees a gap (and conceals it) rather than audio slipping out of time.
    this->seq_++;
    this->rtp_ts_ += this->samples_per_packet_;
    if (lwip_sendto(this->sock_, this->packet_, RTP_HEADER_BYTES + this->payload_bytes_, MSG_DONTWAIT,
                    reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest)) < 0)
      this->send_errors_++;
  }
}

void SapMic::on_shutdown() {
  if (this->announced_)
    this->send_sap_(true);
}

void SapMic::dump_config() {
  ESP_LOGCONFIG(TAG,
                "SAP mic:\n"
                "  Session: %s\n"
                "  Format: L16/%u/%u, %u ms packets\n"
                "  Port: %u\n"
                "  Gain factor: %d%s\n"
                "  Enabled: %s",
                this->session_name_.c_str(), (unsigned) this->sample_rate_, this->channels_,
                (unsigned) this->packet_ms_, this->port_, (int) this->source_->get_gain_factor(),
                this->gain_source_ != nullptr ? " (follows gain_source)" : "", YESNO(this->enabled_));
}

}  // namespace esphome::sap_mic

#endif  // USE_ESP32
