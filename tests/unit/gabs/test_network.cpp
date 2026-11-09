/**
 * @file tests/unit/gabs/test_network.cpp
 * @brief Gabs Remote: tests for the MAC address reported for Wake-on-LAN.
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <string>
#include <vector>

// local includes
#include <src/gabs/network.h>

namespace net_gr = gabs::network;

namespace {
  net_gr::adapter_t ethernet() {
    return {"18:c0:4d:6a:9b:2d", {"192.168.15.2", "fe80::1"}, true, true, true};
  }
}  // namespace

TEST(WolMac, UsesAdapterOfLocalAddress) {
  std::vector<net_gr::adapter_t> adapters {
    {"aa:bb:cc:dd:ee:01", {"192.168.15.10"}, true, true, true},
    ethernet(),
  };
  EXPECT_EQ(net_gr::wol_mac_for("192.168.15.10", adapters), "aa:bb:cc:dd:ee:01");
  EXPECT_EQ(net_gr::wol_mac_for("192.168.15.2", adapters), "18:c0:4d:6a:9b:2d");
}

TEST(WolMac, FallsBackToPhysicalAdapter) {
  std::vector<net_gr::adapter_t> adapters {
    {"", {"100.99.120.91", "fd7a:115c:a1e0::5"}, false, true, false},  // Tailscale (wintun, no MAC)
    ethernet(),
  };
  EXPECT_EQ(net_gr::wol_mac_for("100.99.120.91", adapters), "18:c0:4d:6a:9b:2d");
}

TEST(WolMac, SkipsDownOrGatewaylessAdapters) {
  std::vector<net_gr::adapter_t> adapters {
    {"", {"100.99.120.91"}, false, true, false},
    {"11:11:11:11:11:11", {}, true, false, true},  // Wi-Fi, disconnected
    {"22:22:22:22:22:22", {"172.20.0.1"}, false, true, false},  // vEthernet (Hyper-V)
    ethernet(),
  };
  EXPECT_EQ(net_gr::wol_mac_for("100.99.120.91", adapters), "18:c0:4d:6a:9b:2d");
}

TEST(WolMac, ZeroWhenNothingFits) {
  EXPECT_EQ(net_gr::wol_mac_for("100.99.120.91", {}), "00:00:00:00:00:00");
  std::vector<net_gr::adapter_t> adapters {
    {"", {"100.99.120.91"}, false, true, false},
  };
  EXPECT_EQ(net_gr::wol_mac_for("100.99.120.91", adapters), "00:00:00:00:00:00");
}

TEST(SendSource, UnspecifiedAddressIsNotUsed) {
  EXPECT_FALSE(net_gr::use_as_source(boost::asio::ip::address {}));
  EXPECT_FALSE(net_gr::use_as_source(boost::asio::ip::make_address("0.0.0.0")));
  EXPECT_FALSE(net_gr::use_as_source(boost::asio::ip::make_address("::")));
}

TEST(SendSource, ConnectedAddressIsUsed) {
  EXPECT_TRUE(net_gr::use_as_source(boost::asio::ip::make_address("192.168.15.2")));
  EXPECT_TRUE(net_gr::use_as_source(boost::asio::ip::make_address("100.99.120.91")));
  EXPECT_TRUE(net_gr::use_as_source(boost::asio::ip::make_address("fd7a:115c:a1e0::5")));
}
