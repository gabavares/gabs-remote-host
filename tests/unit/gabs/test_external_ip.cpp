/**
 * @file tests/unit/gabs/test_external_ip.cpp
 * @brief Gabs Remote: tests for the STUN lookup of the home public IPv4 (ExternalIP for WoL from outside).
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// local includes
#include <src/gabs/external_ip.h>

namespace ext = gabs::external_ip;

namespace {
  const ext::transaction_t txid {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

  std::vector<std::uint8_t> response(std::uint16_t attr_type, std::array<std::uint8_t, 4> ip, bool xored, std::uint8_t family = 0x01) {
    std::vector<std::uint8_t> r {0x01, 0x01, 0x00, 0x0C, 0x21, 0x12, 0xA4, 0x42};
    r.insert(r.end(), txid.begin(), txid.end());
    std::uint16_t port = 54321;
    std::array<std::uint8_t, 4> cookie {0x21, 0x12, 0xA4, 0x42};
    if (xored) {
      port ^= 0x2112;
      for (int i = 0; i < 4; ++i) {
        ip[i] ^= cookie[i];
      }
    }
    std::vector<std::uint8_t> attr {
      static_cast<std::uint8_t>(attr_type >> 8), static_cast<std::uint8_t>(attr_type & 0xFF), 0x00, 0x08,
      0x00, family, static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port & 0xFF),
      ip[0], ip[1], ip[2], ip[3]
    };
    r.insert(r.end(), attr.begin(), attr.end());
    return r;
  }
}  // namespace

TEST(Stun, RequestHasBindingHeader) {
  auto req = ext::make_request(txid);
  ASSERT_EQ(req.size(), 20u);
  EXPECT_EQ(req[0], 0x00);
  EXPECT_EQ(req[1], 0x01);
  EXPECT_EQ(req[2], 0x00);
  EXPECT_EQ(req[3], 0x00);
  EXPECT_EQ(req[4], 0x21);
  EXPECT_EQ(req[7], 0x42);
  EXPECT_EQ(req[8], 1);
  EXPECT_EQ(req[19], 12);
}

TEST(Stun, ParsesXorMappedAddress) {
  EXPECT_EQ(ext::parse_response(response(0x0020, {191, 32, 66, 196}, true), txid), std::optional<std::string> {"191.32.66.196"});
}

TEST(Stun, ParsesPlainMappedAddress) {
  EXPECT_EQ(ext::parse_response(response(0x0001, {191, 32, 66, 196}, false), txid), std::optional<std::string> {"191.32.66.196"});
}

TEST(Stun, RejectsOtherTransactionOrGarbage) {
  ext::transaction_t other {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
  EXPECT_FALSE(ext::parse_response(response(0x0020, {191, 32, 66, 196}, true), other).has_value());
  EXPECT_FALSE(ext::parse_response({0x01, 0x01, 0x00}, txid).has_value());
  EXPECT_FALSE(ext::parse_response(response(0x0020, {191, 32, 66, 196}, true, 0x02), txid).has_value());  // IPv6 family
}

TEST(ExternalIp, ConfiguredValueWins) {
  EXPECT_EQ(ext::report("1.2.3.4", "191.32.66.196"), "1.2.3.4");
  EXPECT_EQ(ext::report("", "191.32.66.196"), "191.32.66.196");
  EXPECT_EQ(ext::report("", ""), "");
}
