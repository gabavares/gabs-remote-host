/**
 * @file src/gabs/mic_service.cpp
 * @brief Gabs Remote: tablet microphone service (UDP 48002 → jitter buffer → Opus → sink).
 */
#include "mic_service.h"

// standard includes
#include <atomic>
#include <chrono>
#include <functional>

// lib includes
#include <boost/asio.hpp>

// local includes
#include "mic.h"
#include "src/logging.h"

namespace gabs::mic {
  using namespace std::literals;
  namespace asio = boost::asio;
  using udp = asio::ip::udp;

  std::optional<std::string> choose_mic_sink(const std::vector<endpoint_t> &render_endpoints, const std::string &wanted, const std::string &capture_endpoint_id) {
    if (wanted.empty()) {
      return std::nullopt;
    }
    const endpoint_t *pick = nullptr;
    for (const auto &e : render_endpoints) {
      if (e.name.find(wanted) == std::string::npos) {
        continue;
      }
      // VB-CABLE also has a 16-channel input; the stereo one is the microphone's source.
      if (e.name.find("16 Ch") == std::string::npos) {
        pick = &e;
        break;
      }
      if (!pick) {
        pick = &e;
      }
    }
    if (!pick || pick->id == capture_endpoint_id) {
      return std::nullopt;
    }
    return pick->id;
  }

  struct service_t::impl_t {
    std::unique_ptr<sink_t> sink;
    std::uint16_t wanted_port {default_port};
    std::mutex m;  ///< Guards begin/end.
    std::thread th;
    std::shared_ptr<asio::io_context> io;
    std::atomic<std::uint16_t> bound {0};

    void stop() {
      if (io) {
        io->stop();
      }
      if (th.joinable()) {
        th.join();
      }
      io.reset();
      bound = 0;
    }

    void start(const crypto::aes_t &key) {
      io = std::make_shared<asio::io_context>();
      auto socket = std::make_shared<udp::socket>(*io);
      boost::system::error_code ec;
      // Dual stack: the tablet comes over IPv4 (home or Tailscale), sometimes IPv6.
      socket->open(udp::v6(), ec);
      if (!ec) {
        socket->set_option(asio::ip::v6_only(false), ec);
        socket->bind({udp::v6(), wanted_port}, ec);
      }
      if (ec) {
        boost::system::error_code ignored;
        socket->close(ignored);
        ec.clear();
        socket->open(udp::v4(), ec);
        if (!ec) {
          socket->bind({udp::v4(), wanted_port}, ec);
        }
      }
      if (ec) {
        BOOST_LOG(error) << "Gabs Remote: microfone: não foi possível abrir a porta UDP "sv << wanted_port << ": "sv << ec.message();
        io.reset();
        return;
      }
      bound = socket->local_endpoint().port();
      BOOST_LOG(info) << "Gabs Remote: microfone do tablet esperando na porta UDP "sv << bound.load();

      th = std::thread {[this, io = io, socket, key]() {
        const bool opened = sink->open();
        jitter_t jitter;
        decoder_t decoder;
        std::array<char, 1500> buf {};
        udp::endpoint from;  // may change mid-session (network switch): any source with the key is accepted
        bool bad_logged = false;
        asio::steady_timer timer {*io};
        auto next = std::chrono::steady_clock::now() + frame_duration;

        std::function<void()> receive;
        receive = [&]() {
          socket->async_receive_from(asio::buffer(buf), from, [&](const boost::system::error_code &e, std::size_t n) {
            if (e == asio::error::operation_aborted) {
              return;
            }
            if (!e) {
              if (auto p = open_packet(std::string_view {buf.data(), n}, key)) {
                jitter.push(std::move(*p), std::chrono::steady_clock::now());
              } else if (!bad_logged) {
                // Once per session: wrong key, forged or garbled packets are dropped silently afterwards.
                bad_logged = true;
                BOOST_LOG(warning) << "Gabs Remote: microfone: pacote inválido descartado (chave de outra sessão?)"sv;
              }
            }
            receive();
          });
        };

        std::function<void()> tick;
        tick = [&]() {
          timer.expires_at(next);
          timer.async_wait([&](const boost::system::error_code &e) {
            if (e) {
              return;
            }
            auto now = std::chrono::steady_clock::now();
            auto frame = jitter.pop(now);
            auto pcm = frame ? decoder.decode(*frame) : std::array<std::int16_t, frame_samples> {};
            if (opened) {
              sink->write(pcm);
            }
            next += frame_duration;
            if (now - next > 100ms) {
              next = now + frame_duration;  // fell behind (PC busy or asleep): don't burst
            }
            tick();
          });
        };

        receive();
        tick();
        io->run();
        boost::system::error_code ignored;
        socket->close(ignored);
        if (opened) {
          sink->close();
        }
      }};
    }
  };

  service_t::service_t(std::unique_ptr<sink_t> sink, std::uint16_t port):
      impl_ {std::make_unique<impl_t>()} {
    impl_->sink = std::move(sink);
    impl_->wanted_port = port;
  }

  service_t::~service_t() {
    end();
  }

  void service_t::begin(const crypto::aes_t &rikey) {
    std::lock_guard lg {impl_->m};
    impl_->stop();
    if (!impl_->sink || rikey.size() != 16) {
      return;
    }
    impl_->start(rikey);
  }

  void service_t::end() {
    std::lock_guard lg {impl_->m};
    if (impl_->io) {
      BOOST_LOG(info) << "Gabs Remote: microfone do tablet encerrado"sv;
    }
    impl_->stop();
  }

  std::uint16_t service_t::port() const {
    return impl_->bound.load();
  }
}  // namespace gabs::mic
