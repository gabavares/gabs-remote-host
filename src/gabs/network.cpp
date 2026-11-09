/**
 * @file src/gabs/network.cpp
 * @brief Gabs Remote: network adapters of the PC (platform independent part).
 */
// standard includes
#include <algorithm>
#include <mutex>
#include <set>

// local includes
#include "network.h"
#include "src/logging.h"

using namespace std::literals;

namespace gabs::network {

  namespace {
    constexpr auto zero_mac = "00:00:00:00:00:00"sv;
  }

  std::string wol_mac_for(std::string_view local_address, const std::vector<adapter_t> &adapters) {
    for (const auto &adapter : adapters) {
      if (adapter.mac.empty()) {
        continue;
      }
      if (std::find(adapter.addresses.begin(), adapter.addresses.end(), local_address) != adapter.addresses.end()) {
        return adapter.mac;
      }
    }

    for (const auto &adapter : adapters) {
      if (!adapter.mac.empty() && adapter.physical && adapter.up && adapter.has_ipv4_gateway) {
        return adapter.mac;
      }
    }

    return std::string {zero_mac};
  }

  std::string wol_mac_address(std::string_view local_address) {
    auto adapters = enumerate_adapters();
    auto mac = wol_mac_for(local_address, adapters);

    // Log once per address when the request did not arrive on a card with a MAC (Tailscale, for example).
    bool direct = std::any_of(adapters.begin(), adapters.end(), [&](const adapter_t &a) {
      return !a.mac.empty() && std::find(a.addresses.begin(), a.addresses.end(), local_address) != a.addresses.end();
    });
    if (!direct) {
      static std::mutex mutex;
      static std::set<std::string, std::less<>> logged;
      std::lock_guard lock {mutex};
      if (logged.emplace(local_address).second) {
        if (mac == zero_mac) {
          BOOST_LOG(warning) << "Gabs Remote: no network card with a MAC address for Wake-on-LAN ("sv << local_address << ')';
        } else {
          BOOST_LOG(info) << "Gabs Remote: "sv << local_address << " has no MAC address; reporting the physical card "sv << mac << " for Wake-on-LAN"sv;
        }
      }
    }

    return mac;
  }

}  // namespace gabs::network
