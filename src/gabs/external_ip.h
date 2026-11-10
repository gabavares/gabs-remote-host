/**
 * @file src/gabs/external_ip.h
 * @brief Gabs Remote: the home public IPv4, found by STUN and reported as `ExternalIP` in /serverinfo.
 *
 * The app keeps `ExternalIP` as the PC's remote address. With it, the app knows where to send the
 * Wake-on-LAN packet from outside (router UDP 8080, decision 0005) even after the PC is off, as long
 * as it saw the PC once since the IP last changed. Uses the same STUN server as Moonlight.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace gabs::external_ip {

  using transaction_t = std::array<std::uint8_t, 12>;

  /**
   * @brief STUN binding request (RFC 5389), 20 bytes.
   */
  std::vector<std::uint8_t> make_request(const transaction_t &txid);

  /**
   * @brief IPv4 from a binding success response (XOR-MAPPED-ADDRESS, else MAPPED-ADDRESS).
   * @return Empty for another transaction, IPv6, or a malformed packet.
   */
  std::optional<std::string> parse_response(const std::vector<std::uint8_t> &packet, const transaction_t &txid);

  /**
   * @brief Value for `ExternalIP`: the configured `external_ip` if set, else the discovered one.
   */
  std::string report(std::string_view configured, std::string_view discovered);

  /**
   * @brief Last discovered IPv4 (empty until the first answer).
   */
  std::string current();

  /**
   * @brief Background lookup: right away, then every 5 minutes.
   * @return The thread (stops and joins when destroyed).
   */
  std::jthread start();

}  // namespace gabs::external_ip
