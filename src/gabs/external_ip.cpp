/**
 * @file src/gabs/external_ip.cpp
 * @brief Gabs Remote: STUN lookup of the home public IPv4.
 */
// standard includes
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "external_ip.h"
#include "src/logging.h"

using namespace std::literals;

namespace gabs::external_ip {

  namespace {
    constexpr std::uint32_t magic_cookie = 0x2112A442;
    constexpr std::uint16_t binding_success = 0x0101;
    constexpr std::uint16_t attr_mapped = 0x0001;
    constexpr std::uint16_t attr_xor_mapped = 0x0020;
    constexpr auto stun_host = "stun.moonlight-stream.org";
    constexpr auto stun_port = "3478";
    constexpr auto refresh_interval = 5min;
    constexpr auto retry_interval = 30s;

    std::mutex mutex;
    std::string discovered;

    std::uint16_t be16(const std::vector<std::uint8_t> &p, std::size_t i) {
      return static_cast<std::uint16_t>((p[i] << 8) | p[i + 1]);
    }

    std::optional<std::string> lookup_once() {
      using boost::asio::ip::udp;
      boost::asio::io_context io;
      boost::system::error_code ec;
      udp::resolver resolver {io};
      auto endpoints = resolver.resolve(udp::v4(), stun_host, stun_port, ec);
      if (ec || endpoints.empty()) {
        return std::nullopt;
      }

      transaction_t txid;
      std::random_device rd;
      for (auto &b : txid) {
        b = static_cast<std::uint8_t>(rd());
      }
      auto request = make_request(txid);

      udp::socket socket {io};
      socket.open(udp::v4(), ec);
      if (ec) {
        return std::nullopt;
      }
      std::vector<std::uint8_t> buffer(512);
      std::optional<std::string> result;
      for (int attempt = 0; attempt < 3 && !result; ++attempt) {
        socket.send_to(boost::asio::buffer(request), *endpoints.begin(), 0, ec);
        if (ec) {
          return std::nullopt;
        }
        udp::endpoint from;
        std::size_t received = 0;
        bool done = false;
        socket.async_receive_from(boost::asio::buffer(buffer), from, [&](const boost::system::error_code &e, std::size_t n) {
          if (!e) {
            received = n;
          }
          done = true;
        });
        io.restart();
        io.run_for(2s);
        if (!done) {
          socket.cancel(ec);
          io.restart();
          io.run();
          continue;
        }
        if (received > 0) {
          result = parse_response(std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + received), txid);
        }
      }
      return result;
    }

    void run(std::stop_token stop) {
      std::mutex wait_mutex;
      std::condition_variable_any cv;
      bool failing = false;
      while (!stop.stop_requested()) {
        auto ip = lookup_once();
        if (ip) {
          std::lock_guard lock {mutex};
          if (discovered != *ip) {
            BOOST_LOG(info) << "Gabs Remote: home public IPv4 is "sv << *ip << " (STUN)"sv;
            discovered = *ip;
          }
          failing = false;
        } else if (!failing) {
          failing = true;
          BOOST_LOG(warning) << "Gabs Remote: STUN lookup of the public IPv4 failed; retrying"sv;
        }
        std::unique_lock lock {wait_mutex};
        cv.wait_for(lock, stop, ip ? std::chrono::duration_cast<std::chrono::seconds>(refresh_interval) : retry_interval, [] {
          return false;
        });
      }
    }
  }  // namespace

  std::vector<std::uint8_t> make_request(const transaction_t &txid) {
    std::vector<std::uint8_t> r {0x00, 0x01, 0x00, 0x00};
    r.push_back(static_cast<std::uint8_t>(magic_cookie >> 24));
    r.push_back(static_cast<std::uint8_t>(magic_cookie >> 16));
    r.push_back(static_cast<std::uint8_t>(magic_cookie >> 8));
    r.push_back(static_cast<std::uint8_t>(magic_cookie));
    r.insert(r.end(), txid.begin(), txid.end());
    return r;
  }

  std::optional<std::string> parse_response(const std::vector<std::uint8_t> &p, const transaction_t &txid) {
    if (p.size() < 20 || be16(p, 0) != binding_success) {
      return std::nullopt;
    }
    std::uint32_t cookie = (std::uint32_t {p[4]} << 24) | (std::uint32_t {p[5]} << 16) | (std::uint32_t {p[6]} << 8) | p[7];
    if (cookie != magic_cookie || !std::equal(txid.begin(), txid.end(), p.begin() + 8)) {
      return std::nullopt;
    }
    std::size_t end = std::min<std::size_t>(p.size(), 20 + be16(p, 2));
    std::optional<std::string> mapped;
    for (std::size_t i = 20; i + 4 <= end;) {
      auto type = be16(p, i);
      auto len = be16(p, i + 2);
      auto value = i + 4;
      if (value + len > end) {
        break;
      }
      if ((type == attr_xor_mapped || type == attr_mapped) && len >= 8 && p[value + 1] == 0x01) {
        std::array<std::uint8_t, 4> ip {p[value + 4], p[value + 5], p[value + 6], p[value + 7]};
        if (type == attr_xor_mapped) {
          for (int k = 0; k < 4; ++k) {
            ip[k] ^= static_cast<std::uint8_t>(magic_cookie >> (24 - 8 * k));
          }
        }
        auto text = std::to_string(ip[0]) + '.' + std::to_string(ip[1]) + '.' + std::to_string(ip[2]) + '.' + std::to_string(ip[3]);
        if (type == attr_xor_mapped) {
          return text;
        }
        mapped = text;
      }
      i = value + ((len + 3) & ~std::size_t {3});
    }
    return mapped;
  }

  std::string report(std::string_view configured, std::string_view discovered_ip) {
    return std::string {configured.empty() ? discovered_ip : configured};
  }

  std::string current() {
    std::lock_guard lock {mutex};
    return discovered;
  }

  std::jthread start() {
    return std::jthread {run};
  }

}  // namespace gabs::external_ip
