/**
 * @file src/gabs/presence.cpp
 * @brief Gabs Remote: presence page client.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>

// lib includes
#include <boost/asio/ip/address.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

// local includes
#include "presence.h"
#include "src/config.h"
#include "src/httpcommon.h"
#include "src/logging.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace gabs::presence {

  namespace {
    constexpr std::size_t max_ipv6 = 8;
    constexpr auto check_interval = 30s;
    constexpr auto refresh_interval = 5min;

    bool is_tailscale(const boost::asio::ip::address &a) {
      if (!a.is_v4()) {
        return false;
      }
      auto bytes = a.to_v4().to_bytes();
      return bytes[0] == 100 && (bytes[1] & 0xC0) == 64;
    }

    bool is_global_ipv6(const boost::asio::ip::address &a) {
      if (!a.is_v6() || a.to_v6().is_v4_mapped()) {
        return false;
      }
      return (a.to_v6().to_bytes()[0] & 0xE0) == 0x20;  // 2000::/3
    }

    void sort_unique(std::vector<std::string> &v) {
      std::sort(v.begin(), v.end());
      v.erase(std::unique(v.begin(), v.end()), v.end());
    }

    /**
     * @return HTTP status, or 0 when the request did not complete.
     */
    long post(const config_t &config, const std::string &body) {
      CURL *curl = curl_easy_init();
      if (!curl) {
        return 0;
      }
      auto auth = "Authorization: Bearer "s + config.token;
      curl_slist *headers = nullptr;
      headers = curl_slist_append(headers, auth.c_str());
      headers = curl_slist_append(headers, "Content-Type: application/json");

      curl_easy_setopt(curl, CURLOPT_URL, config.url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
      curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);  // the page must see the public IPv4
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char *, size_t size, size_t n, void *) {
        return size * n;
      });

      long status = 0;
      if (curl_easy_perform(curl) == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      }
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      return status;
    }

    void run(std::stop_token stop, config_t config) {
      std::mutex mutex;
      std::condition_variable_any cv;
      std::optional<addresses_t> sent;
      auto last_sent = std::chrono::steady_clock::time_point {};
      bool failing = false;

      BOOST_LOG(info) << "Gabs Remote: presence on ("sv << config.url << ')';
      while (!stop.stop_requested()) {
        auto addresses = select_addresses(network::enumerate_ip_addresses());
        auto now = std::chrono::steady_clock::now();
        if (!sent || *sent != addresses || now - last_sent >= refresh_interval) {
          auto status = post(config, make_body(http::unique_id, config::sunshine.port, addresses, PROJECT_VERSION));
          if (status == 200) {
            if (failing) {
              BOOST_LOG(info) << "Gabs Remote: presence updated again"sv;
            }
            failing = false;
            sent = addresses;
            last_sent = now;
          } else if (!failing) {
            failing = true;
            BOOST_LOG(warning) << "Gabs Remote: could not update the presence page (HTTP "sv << status << "); retrying"sv;
          }
        }

        std::unique_lock lock {mutex};
        cv.wait_for(lock, stop, check_interval, [] {
          return false;
        });
      }
    }
  }  // namespace

  std::optional<config_t> load_config(const std::filesystem::path &file) {
    std::ifstream in {file};
    if (!in) {
      return std::nullopt;
    }
    auto json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object()) {
      return std::nullopt;
    }
    auto url = json.value("url", nlohmann::json {});
    auto token = json.value("token", nlohmann::json {});
    if (!url.is_string() || !token.is_string() || url.get<std::string>().empty() || token.get<std::string>().empty()) {
      return std::nullopt;
    }
    return config_t {url.get<std::string>(), token.get<std::string>()};
  }

  addresses_t select_addresses(const std::vector<network::ip_address_t> &all) {
    addresses_t result;
    for (const auto &entry : all) {
      boost::system::error_code ec;
      auto address = boost::asio::ip::make_address(entry.address, ec);
      if (ec) {
        continue;
      }
      if (is_tailscale(address)) {
        result.tailscale.push_back(address.to_string());
      } else if (is_global_ipv6(address) && !entry.deprecated) {
        result.ipv6.push_back(address.to_string());
      }
    }
    sort_unique(result.ipv6);
    sort_unique(result.tailscale);
    if (result.ipv6.size() > max_ipv6) {
      result.ipv6.resize(max_ipv6);
    }
    return result;
  }

  std::string make_body(const std::string &uuid, int port, const addresses_t &addresses, const std::string &version) {
    nlohmann::json body {
      {"uuid", uuid},
      {"porta", port},
      {"ipv6", addresses.ipv6},
      {"tailscale", addresses.tailscale},
      {"versao", version},
    };
    return body.dump();
  }

  std::jthread start() {
    auto config = load_config(platf::appdata() / "gabs_presence.json");
    if (!config) {
      return {};
    }
    return std::jthread {run, std::move(*config)};
  }

}  // namespace gabs::presence
