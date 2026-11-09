/**
 * @file src/gabs/presence.h
 * @brief Gabs Remote: tells the presence page (on Gab's hosting) where the PC is.
 *
 * The page keeps the home public IPv4 (it sees the request's source address), so the app can send the
 * Wake-on-LAN packet to the router from outside even with the PC off (decision 0005). The host posts
 * its addresses when they change and every 5 minutes. Without `gabs_presence.json` in the config
 * folder the feature is off.
 */
#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "network.h"

namespace gabs::presence {

  struct config_t {
    std::string url;
    std::string token;
  };

  /**
   * @brief Read `{"url": "...", "token": "..."}`.
   * @return Empty when the file is missing, invalid or has an empty field (presence off, no error).
   */
  std::optional<config_t> load_config(const std::filesystem::path &file);

  struct addresses_t {
    std::vector<std::string> ipv6;
    std::vector<std::string> tailscale;
    bool operator==(const addresses_t &) const = default;
  };

  /**
   * @brief Global IPv6 (2000::/3) that are not deprecated, at most 8, and Tailscale IPv4 (100.64.0.0/10),
   *        both sorted and without repeats.
   */
  addresses_t select_addresses(const std::vector<network::ip_address_t> &all);

  /**
   * @brief JSON body of the POST: `uuid`, `porta`, `ipv6`, `tailscale`, `versao`.
   */
  std::string make_body(const std::string &uuid, int port, const addresses_t &addresses, const std::string &version);

  /**
   * @brief Start the background thread (checks every 30 s, posts on change or every 5 minutes).
   * @return The thread (stops and joins when destroyed); not joinable when presence is off.
   */
  std::jthread start();

}  // namespace gabs::presence
