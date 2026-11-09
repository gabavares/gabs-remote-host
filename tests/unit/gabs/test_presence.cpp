/**
 * @file tests/unit/gabs/test_presence.cpp
 * @brief Gabs Remote: tests for the presence page client (where the PC is, for WoL from outside).
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include <src/gabs/presence.h>

namespace pr = gabs::presence;

TEST(Presence, SelectsGlobalIpv6AndTailscale) {
  std::vector<gabs::network::ip_address_t> all {
    {"2804:14d:1::5", false},
    {"fe80::1", false},
    {"fd7a:115c:a1e0::1", false},
    {"192.168.15.10", false},
    {"100.99.120.91", false},
    {"2804:14d:1::6", true},
    {"2804:14d:1::5", false},
  };
  auto a = pr::select_addresses(all);
  EXPECT_EQ(a.ipv6, (std::vector<std::string> {"2804:14d:1::5"}));
  EXPECT_EQ(a.tailscale, (std::vector<std::string> {"100.99.120.91"}));
}

TEST(Presence, CapsIpv6AtEight) {
  std::vector<gabs::network::ip_address_t> all;
  for (int i = 0; i < 10; ++i) {
    all.push_back({"2804:14d:1::" + std::to_string(i + 1), false});
  }
  EXPECT_EQ(pr::select_addresses(all).ipv6.size(), 8u);
}

TEST(Presence, BodyMatchesContract) {
  pr::addresses_t a {{"2804:14d:1::5"}, {"100.99.120.91"}};
  auto body = nlohmann::json::parse(pr::make_body("0F3A6C2B-1D4E-4F5A-8B9C-0D1E2F3A4B5C", 47989, a, "2026.1109"));
  EXPECT_EQ(body.size(), 5u);
  EXPECT_EQ(body["uuid"], "0F3A6C2B-1D4E-4F5A-8B9C-0D1E2F3A4B5C");
  EXPECT_EQ(body["porta"], 47989);
  EXPECT_EQ(body["ipv6"], nlohmann::json::array({"2804:14d:1::5"}));
  EXPECT_EQ(body["tailscale"], nlohmann::json::array({"100.99.120.91"}));
  EXPECT_EQ(body["versao"], "2026.1109");
}

TEST(Presence, ConfigMissingOrEmptyIsOff) {
  auto dir = std::filesystem::temp_directory_path() / "gabs_presence_test";
  std::filesystem::create_directories(dir);
  auto file = dir / "gabs_presence.json";
  std::filesystem::remove(file);
  EXPECT_FALSE(pr::load_config(file).has_value());

  auto write = [&](const std::string &text) {
    std::ofstream out {file, std::ios::trunc};
    out << text;
  };
  write("{}");
  EXPECT_FALSE(pr::load_config(file).has_value());
  write(R"({"url":"x","token":""})");
  EXPECT_FALSE(pr::load_config(file).has_value());
  write("{nao e json");
  EXPECT_FALSE(pr::load_config(file).has_value());
  write(R"({"url":"https://remote.gabtavares.com/presenca.php","token":"abc"})");
  auto config = pr::load_config(file);
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->url, "https://remote.gabtavares.com/presenca.php");
  EXPECT_EQ(config->token, "abc");
  std::filesystem::remove_all(dir);
}
