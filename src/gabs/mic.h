/**
 * @file src/gabs/mic.h
 * @brief Gabs Remote: tablet microphone (decision 0007).
 *
 * The app sends one Opus frame (48 kHz mono, 20 ms) per UDP datagram to port 48002, encrypted with
 * AES-128-GCM under the session key (`rikey`). This file holds the parts without I/O: the packet format,
 * the jitter buffer and the Opus decoder. The UDP socket and the virtual microphone live in `mic_service`.
 *
 * Packet: "GRM1" | seq (uint32 LE) | GCM tag (16) | encrypted Opus frame (1..1000 bytes).
 * IV (12 bytes): seq LE in bytes 0-3, zeros in 4-9, 'C' (client) and 'M' (microphone) in 10-11.
 */
#pragma once

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

// local includes
#include "src/crypto.h"

namespace gabs::mic {
  using namespace std::chrono_literals;

  constexpr std::size_t header_size = 24;  ///< magic + seq + tag.
  constexpr std::size_t max_opus = 1000;  ///< Largest Opus frame accepted.
  constexpr int sample_rate = 48000;
  constexpr int frame_samples = 960;  ///< 20 ms at 48 kHz.
  constexpr auto frame_duration = 20ms;

  /**
   * @brief A packet that passed the size, magic and tag checks.
   */
  struct packet_t {
    std::uint32_t seq;
    std::vector<std::uint8_t> opus;
  };

  /**
   * @brief Decrypt and check one datagram.
   * @param datagram Bytes received from the app.
   * @param key Session key (16 bytes).
   * @return The packet, or nullopt for any failure (size, magic, tag, key).
   */
  std::optional<packet_t> open_packet(std::string_view datagram, const crypto::aes_t &key);

  /**
   * @brief Build a packet (the inverse of open_packet; used by tests and to check the app's format).
   */
  std::vector<std::uint8_t> seal_packet(std::uint32_t seq, std::string_view opus, const crypto::aes_t &key);

  /**
   * @brief One 20 ms frame for the decoder.
   */
  struct frame_t {
    enum class kind_e {
      normal,  ///< The expected packet.
      fec,  ///< The expected packet is missing; `opus` is the next one, decoded with FEC.
      conceal,  ///< Missing and nothing to recover from: Opus packet loss concealment.
      silence,  ///< No packets for a while: zeros.
    } kind;

    std::vector<std::uint8_t> opus;
  };

  /**
   * @brief Reorders packets and hands out one frame every 20 ms.
   *
   * Playback starts once `target` worth of audio is buffered (or the first packet waited `target`).
   * Duplicates and packets older than what was already played are dropped; when the buffer grows
   * beyond `max`, the oldest packets are dropped so the delay never keeps growing. With no packets
   * for more than 200 ms it hands out silence, and restarts from whatever arrives next.
   */
  class jitter_t {
  public:
    explicit jitter_t(std::chrono::milliseconds target = 60ms, std::chrono::milliseconds max = 120ms);

    void push(packet_t p, std::chrono::steady_clock::time_point now);

    /**
     * @brief Next frame, to be called every 20 ms.
     * @return The frame, or nullopt while buffering before playback starts.
     */
    std::optional<frame_t> pop(std::chrono::steady_clock::time_point now);

    /**
     * @brief Audio currently waiting in the buffer.
     */
    std::chrono::milliseconds delay() const;

  private:
    std::chrono::milliseconds target_;
    std::chrono::milliseconds max_;
    std::map<std::uint32_t, std::vector<std::uint8_t>> buffer_;
    bool playing_ {false};
    std::uint32_t next_ {0};  ///< Next seq to play (valid while playing).
    std::optional<std::uint32_t> played_;  ///< Last seq handed out (normal or fec source).
    std::chrono::steady_clock::time_point last_push_ {};
    std::chrono::steady_clock::time_point first_wait_ {};
    int conceals_ {0};
    bool started_ {false};  ///< Playback started at least once (silence instead of nullopt while rebuffering).

    void cap();
  };

  /**
   * @brief libopus decoder, 48 kHz mono.
   */
  class decoder_t {
  public:
    decoder_t();
    ~decoder_t();
    decoder_t(const decoder_t &) = delete;
    decoder_t &operator=(const decoder_t &) = delete;

    /**
     * @brief Decode one frame (normal, FEC, concealment or silence) into 960 samples.
     */
    std::array<std::int16_t, frame_samples> decode(const frame_t &f);

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace gabs::mic
