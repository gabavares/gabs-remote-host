/**
 * @file src/gabs/mic.cpp
 * @brief Gabs Remote: tablet microphone packet format, jitter buffer and decoder.
 */
#include "mic.h"

// standard includes
#include <cstring>

// lib includes
#include <opus/opus.h>

namespace gabs::mic {
  namespace {
    constexpr char magic[4] = {'G', 'R', 'M', '1'};
    constexpr std::size_t tag_size = 16;
    constexpr auto silence_after = 200ms;
    constexpr int max_conceals = 5;

    crypto::aes_t make_iv(std::uint32_t seq) {
      crypto::aes_t iv(12, 0);
      iv[0] = static_cast<std::uint8_t>(seq);
      iv[1] = static_cast<std::uint8_t>(seq >> 8);
      iv[2] = static_cast<std::uint8_t>(seq >> 16);
      iv[3] = static_cast<std::uint8_t>(seq >> 24);
      iv[10] = 'C';  // Client originated
      iv[11] = 'M';  // Microphone
      return iv;
    }

    std::uint32_t read_seq(std::string_view d) {
      return static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[4])) |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[5])) << 8 |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[6])) << 16 |
             static_cast<std::uint32_t>(static_cast<std::uint8_t>(d[7])) << 24;
    }
  }  // namespace

  std::optional<packet_t> open_packet(std::string_view datagram, const crypto::aes_t &key) {
    if (datagram.size() < header_size + 1 || datagram.size() > header_size + max_opus || key.size() != 16) {
      return std::nullopt;
    }
    if (std::memcmp(datagram.data(), magic, sizeof(magic)) != 0) {
      return std::nullopt;
    }
    auto seq = read_seq(datagram);
    auto iv = make_iv(seq);
    crypto::cipher::gcm_t cipher {key, false};
    std::vector<std::uint8_t> plain;
    if (cipher.decrypt(datagram.substr(8), plain, &iv) != 0) {
      return std::nullopt;
    }
    return packet_t {seq, std::move(plain)};
  }

  std::vector<std::uint8_t> seal_packet(std::uint32_t seq, std::string_view opus, const crypto::aes_t &key) {
    std::vector<std::uint8_t> out(header_size + opus.size());
    std::memcpy(out.data(), magic, sizeof(magic));
    out[4] = static_cast<std::uint8_t>(seq);
    out[5] = static_cast<std::uint8_t>(seq >> 8);
    out[6] = static_cast<std::uint8_t>(seq >> 16);
    out[7] = static_cast<std::uint8_t>(seq >> 24);
    auto iv = make_iv(seq);
    crypto::cipher::gcm_t cipher {key, false};
    if (cipher.encrypt(opus, out.data() + 8, out.data() + header_size, &iv) < 0) {
      return {};
    }
    return out;
  }

  jitter_t::jitter_t(std::chrono::milliseconds target, std::chrono::milliseconds max):
      target_ {target},
      max_ {max} {
  }

  void jitter_t::push(packet_t p, std::chrono::steady_clock::time_point now) {
    if (played_ && p.seq <= *played_) {
      return;  // already played (or recovered by FEC): late or repeated
    }
    if (playing_ && p.seq < next_) {
      return;  // its slot already went by (concealed)
    }
    if (buffer_.contains(p.seq)) {
      return;  // duplicate
    }
    if (buffer_.empty() && !playing_) {
      first_wait_ = now;
    }
    last_push_ = now;
    buffer_.emplace(p.seq, std::move(p.opus));
    cap();
  }

  std::chrono::milliseconds jitter_t::delay() const {
    if (buffer_.empty()) {
      return 0ms;
    }
    auto first = playing_ ? next_ : buffer_.begin()->first;
    auto last = buffer_.rbegin()->first;
    if (last < first) {
      return 0ms;
    }
    return frame_duration * static_cast<int>(last - first + 1);
  }

  void jitter_t::cap() {
    while (!buffer_.empty() && delay() > max_) {
      auto oldest = buffer_.begin()->first;
      if (playing_ && next_ < oldest) {
        // The gap before the oldest packet counts as delay: skip it first.
        next_ = oldest;
        continue;
      }
      buffer_.erase(buffer_.begin());
      if (playing_) {
        next_ = buffer_.empty() ? oldest + 1 : buffer_.begin()->first;
      }
    }
  }

  std::optional<frame_t> jitter_t::pop(std::chrono::steady_clock::time_point now) {
    if (playing_ && now - last_push_ > silence_after) {
      // Packets stopped: hand out silence and start over from whatever arrives next.
      playing_ = false;
      buffer_.clear();
      played_.reset();
      conceals_ = 0;
      return frame_t {frame_t::kind_e::silence, {}};
    }

    if (!playing_) {
      if (!buffer_.empty() && (delay() >= target_ || now - first_wait_ >= target_)) {
        playing_ = true;
        next_ = buffer_.begin()->first;
        conceals_ = 0;
      } else if (started_) {
        return frame_t {frame_t::kind_e::silence, {}};
      } else {
        return std::nullopt;
      }
    }
    started_ = true;

    auto seq = next_++;
    if (auto it = buffer_.find(seq); it != buffer_.end()) {
      frame_t f {frame_t::kind_e::normal, std::move(it->second)};
      buffer_.erase(it);
      played_ = seq;
      conceals_ = 0;
      return f;
    }
    if (auto it = buffer_.find(seq + 1); it != buffer_.end()) {
      // The next packet carries a low-bitrate copy of this one (in-band FEC).
      conceals_ = 0;
      return frame_t {frame_t::kind_e::fec, it->second};
    }
    if (++conceals_ > max_conceals) {
      return frame_t {frame_t::kind_e::silence, {}};
    }
    return frame_t {frame_t::kind_e::conceal, {}};
  }

  struct decoder_t::impl_t {
    OpusDecoder *dec {nullptr};

    ~impl_t() {
      if (dec) {
        opus_decoder_destroy(dec);
      }
    }
  };

  decoder_t::decoder_t():
      impl_ {std::make_unique<impl_t>()} {
    int err = 0;
    impl_->dec = opus_decoder_create(sample_rate, 1, &err);
    if (err != OPUS_OK) {
      impl_->dec = nullptr;
    }
  }

  decoder_t::~decoder_t() = default;

  std::array<std::int16_t, frame_samples> decoder_t::decode(const frame_t &f) {
    std::array<std::int16_t, frame_samples> pcm {};
    if (!impl_->dec || f.kind == frame_t::kind_e::silence) {
      return pcm;
    }
    int n;
    if (f.kind == frame_t::kind_e::conceal || f.opus.empty()) {
      n = opus_decode(impl_->dec, nullptr, 0, pcm.data(), frame_samples, 0);
    } else {
      n = opus_decode(impl_->dec, f.opus.data(), static_cast<opus_int32>(f.opus.size()), pcm.data(), frame_samples, f.kind == frame_t::kind_e::fec ? 1 : 0);
    }
    if (n < 0) {
      pcm.fill(0);
    }
    return pcm;
  }
}  // namespace gabs::mic
