/**
 * @file src/platform/windows/gabs/adapters_win.cpp
 * @brief Gabs Remote: network adapters of the PC on Windows (GetAdaptersAddresses).
 */
// platform includes
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>

// standard includes
#include <cstdio>
#include <cstdlib>
#include <memory>

// local includes
#include "src/gabs/network.h"
#include "src/platform/common.h"

namespace gabs::network {

  namespace {
    struct free_deleter_t {
      void operator()(void *p) const {
        std::free(p);
      }
    };

    using adapters_ptr_t = std::unique_ptr<IP_ADAPTER_ADDRESSES, free_deleter_t>;

    adapters_ptr_t get_adapters(ULONG flags) {
      adapters_ptr_t list;
      ULONG size = 16 * 1024;
      for (int attempt = 0; attempt < 4; ++attempt) {
        list.reset(static_cast<IP_ADAPTER_ADDRESSES *>(std::malloc(size)));
        if (!list) {
          return {};
        }
        auto result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, list.get(), &size);
        if (result == NO_ERROR) {
          return list;
        }
        if (result != ERROR_BUFFER_OVERFLOW) {
          return {};
        }
      }
      return {};
    }

    std::string mac_string(const IP_ADAPTER_ADDRESSES &adapter) {
      if (adapter.PhysicalAddressLength == 0) {
        return {};
      }
      std::string mac;
      char part[4];
      for (ULONG i = 0; i < adapter.PhysicalAddressLength; ++i) {
        std::snprintf(part, sizeof(part), i == 0 ? "%02x" : ":%02x", adapter.PhysicalAddress[i]);
        mac += part;
      }
      return mac;
    }
  }  // namespace

  std::vector<adapter_t> enumerate_adapters() {
    std::vector<adapter_t> adapters;
    auto list = get_adapters(GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER);
    for (auto a = list.get(); a != nullptr; a = a->Next) {
      adapter_t adapter;
      adapter.mac = mac_string(*a);
      adapter.physical = a->IfType == IF_TYPE_ETHERNET_CSMACD || a->IfType == IF_TYPE_IEEE80211;
      adapter.up = a->OperStatus == IfOperStatusUp;
      adapter.has_ipv4_gateway = false;
      for (auto g = a->FirstGatewayAddress; g != nullptr; g = g->Next) {
        if (g->Address.lpSockaddr && g->Address.lpSockaddr->sa_family == AF_INET) {
          adapter.has_ipv4_gateway = true;
          break;
        }
      }
      for (auto u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
        adapter.addresses.push_back(platf::from_sockaddr(u->Address.lpSockaddr));
      }
      adapters.push_back(std::move(adapter));
    }
    return adapters;
  }

  std::vector<ip_address_t> enumerate_ip_addresses() {
    std::vector<ip_address_t> addresses;
    auto list = get_adapters(GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER);
    for (auto a = list.get(); a != nullptr; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp) {
        continue;
      }
      for (auto u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
        addresses.push_back({platf::from_sockaddr(u->Address.lpSockaddr), u->DadState == IpDadStateDeprecated});
      }
    }
    return addresses;
  }

}  // namespace gabs::network
