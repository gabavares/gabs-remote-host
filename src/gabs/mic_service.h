/**
 * @file src/gabs/mic_service.h
 * @brief Gabs Remote: receives the tablet microphone during a session and plays it into the virtual
 * microphone (VB-CABLE, decision 0007).
 */
#pragma once

// standard includes
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "src/crypto.h"

namespace gabs::mic {
  constexpr std::uint16_t default_port = 48002;

  /**
   * @brief Where the decoded microphone goes (the virtual cable's playback endpoint on Windows).
   *
   * All calls happen on the service thread.
   */
  class sink_t {
  public:
    virtual ~sink_t() = default;
    virtual bool open() = 0;
    virtual void write(std::span<const std::int16_t> pcm48k_mono) = 0;
    virtual void close() = 0;
  };

  /**
   * @brief A playback endpoint as Windows lists it.
   */
  struct endpoint_t {
    std::string id;
    std::string name;
  };

  /**
   * @brief Pick the endpoint the microphone is written to.
   * @param render_endpoints Active playback endpoints.
   * @param wanted Part of the name (`gr_mic_sink`, default `VB-Audio Virtual Cable`).
   * @param capture_endpoint_id The endpoint whose sound the host is capturing for the session (default output).
   * @return The id; nullopt if nothing matches or if the match is the endpoint being captured
   * (the tablet would hear its own microphone). The 16-channel endpoint of VB-CABLE is skipped when
   * another one matches.
   */
  std::optional<std::string> choose_mic_sink(const std::vector<endpoint_t> &render_endpoints, const std::string &wanted, const std::string &capture_endpoint_id);

  /**
   * @brief WASAPI shared-mode sink for the endpoint named `endpoint_name` (Windows), or a sink that
   * never opens elsewhere.
   */
  std::unique_ptr<sink_t> make_platform_sink(const std::string &endpoint_name);

  /**
   * @brief UDP receiver + jitter buffer + decoder feeding a sink every 20 ms.
   */
  class service_t {
  public:
    /**
     * @param sink Where the audio goes.
     * @param port UDP port (0 picks a free one, for tests).
     */
    explicit service_t(std::unique_ptr<sink_t> sink, std::uint16_t port = default_port);
    ~service_t();
    service_t(const service_t &) = delete;
    service_t &operator=(const service_t &) = delete;

    /**
     * @brief Start (or restart with a new key) receiving for a session.
     * @param rikey Session key (16 bytes).
     */
    void begin(const crypto::aes_t &rikey);

    /**
     * @brief Stop receiving, close the socket and release the device.
     */
    void end();

    /**
     * @brief Bound UDP port while running (0 when stopped).
     */
    std::uint16_t port() const;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  /**
   * @brief The host's service, with the platform sink named in `gr_mic_sink`.
   */
  service_t &instance();
}  // namespace gabs::mic
