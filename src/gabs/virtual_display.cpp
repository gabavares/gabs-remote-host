/**
 * @file src/gabs/virtual_display.cpp
 * @brief Gabs Remote: per-client virtual display requested with `grDisplay`.
 */
// standard includes
#include <chrono>
#include <utility>

// lib includes
#include <openssl/evp.h>

// local includes
#include "src/logging.h"
#include "virtual_display.h"

using namespace std::literals;

namespace gabs::virtual_display {

  display_choice_e parse_display_choice(std::string_view arg) {
    if (arg == "virtual"sv) {
      return display_choice_e::virtual_display;
    }
    if (arg == "mirror"sv) {
      return display_choice_e::mirror;
    }
    return display_choice_e::upstream;
  }

  std::string client_uid_from(std::string_view client_cert, std::string_view unique_id) {
    return std::string {client_cert.empty() ? unique_id : client_cert};
  }

  int refresh_hz_from_fps(int fps) {
    if (fps <= 0) {
      return 60;
    }
    return fps >= 1000 ? fps / 1000 : fps;
  }

  std::array<std::uint8_t, 16> monitor_guid_for(std::string_view client_uid) {
    // Namespace UUID of the Gabs Remote monitors (random, fixed forever).
    static constexpr std::array<std::uint8_t, 16> ns {0x5b, 0x0e, 0x1c, 0x6a, 0x3f, 0x47, 0x4c, 0x2d, 0x9a, 0x51, 0x2e, 0x8b, 0x6f, 0x13, 0xd4, 0x07};

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned int digest_len = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, ns.data(), ns.size());
    EVP_DigestUpdate(ctx, client_uid.data(), client_uid.size());
    EVP_DigestFinal_ex(ctx, digest.data(), &digest_len);
    EVP_MD_CTX_free(ctx);

    std::array<std::uint8_t, 16> uuid {};
    std::copy_n(digest.begin(), uuid.size(), uuid.begin());
    uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0F) | 0x50);  // version 5
    uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3F) | 0x80);  // RFC 4122 variant
    return uuid;
  }

  manager_t::manager_t(std::unique_ptr<driver_t> driver, device_id_for_t device_id_for):
      driver {std::move(driver)},
      device_id_for {std::move(device_id_for)} {
  }

  manager_t::~manager_t() {
    stop_keep_alive();
  }

  std::optional<std::string> manager_t::ensure(const std::string &client_uid, const mode_t &mode) {
    std::unique_lock lock {mutex};
    if (!driver || !device_id_for) {
      return std::nullopt;
    }

    if (active_id && active_client == client_uid && active_mode == mode) {
      // Make sure the display still exists (the driver watchdog or a driver restart may have removed it).
      const auto name {*active_display_name};
      lock.unlock();
      const auto still_there {device_id_for(name)};
      lock.lock();
      if (still_there && active_id && active_client == client_uid && active_mode == mode) {
        active_id = still_there;
        BOOST_LOG(info) << "Gabs Remote: reusing the virtual display of this client ("sv << *active_id << ')';
        return active_id;
      }
      BOOST_LOG(warning) << "Gabs Remote: the virtual display of this client is gone; creating it again"sv;
    }

    if (active_client) {
      remove_active_unlocked();
    }

    // Ping the driver from now on: creating a display can take longer than the watchdog timeout.
    start_keep_alive();

    const auto display_name {driver->add(client_uid, mode)};
    if (!display_name) {
      BOOST_LOG(warning) << "Gabs Remote: the virtual display driver could not create a "sv << mode.width << 'x' << mode.height << '@' << mode.refresh_hz << " display"sv;
      lock.unlock();
      stop_keep_alive();
      return std::nullopt;
    }

    // The device id lookup may wait for Windows to activate the new display, so do it unlocked.
    lock.unlock();
    auto device_id {device_id_for(*display_name)};
    lock.lock();

    if (!device_id) {
      BOOST_LOG(warning) << "Gabs Remote: virtual display "sv << *display_name << " was created but did not show up as a display device; removing it"sv;
      driver->remove(client_uid);
      lock.unlock();
      stop_keep_alive();
      return std::nullopt;
    }

    BOOST_LOG(info) << "Gabs Remote: virtual display "sv << *display_name << " (" << *device_id << ") at "sv << mode.width << 'x' << mode.height << '@' << mode.refresh_hz;
    active_client = client_uid;
    active_mode = mode;
    active_display_name = display_name;
    active_id = device_id;
    return active_id;
  }

  void manager_t::release_all() {
    {
      std::lock_guard lock {mutex};
      if (!active_client) {
        return;
      }
      BOOST_LOG(info) << "Gabs Remote: removing the virtual display"sv;
      remove_active_unlocked();
    }
    stop_keep_alive();
  }

  void manager_t::startup_cleanup() {
    {
      std::lock_guard lock {mutex};
      if (driver) {
        driver->remove_all();
      }
      active_client.reset();
      active_mode.reset();
      active_display_name.reset();
      active_id.reset();
    }
    stop_keep_alive();
  }

  std::optional<std::string> manager_t::active_device_id() const {
    std::lock_guard lock {mutex};
    return active_id;
  }

  void manager_t::remove_active_unlocked() {
    if (driver && active_client) {
      driver->remove(*active_client);
    }
    active_client.reset();
    active_mode.reset();
    active_display_name.reset();
    active_id.reset();
  }

  void manager_t::start_keep_alive() {
    std::lock_guard lock {keep_alive_mutex};
    if (keep_alive_running || !driver) {
      return;
    }
    keep_alive_running = true;
    keep_alive_thread = std::thread {[this]() {
      std::unique_lock lock {keep_alive_mutex};
      while (keep_alive_running) {
        lock.unlock();
        driver->keep_alive();
        lock.lock();
        keep_alive_wake.wait_for(lock, 1s, [this]() {
          return !keep_alive_running;
        });
      }
    }};
  }

  void manager_t::stop_keep_alive() {
    std::thread thread;
    {
      std::lock_guard lock {keep_alive_mutex};
      keep_alive_running = false;
      thread = std::move(keep_alive_thread);
    }
    keep_alive_wake.notify_all();
    if (thread.joinable()) {
      thread.join();
    }
  }

  namespace {
    std::mutex slot_mutex;

    std::unique_ptr<manager_t> &slot() {
      static std::unique_ptr<manager_t> current {std::make_unique<manager_t>(nullptr, nullptr)};
      return current;
    }
  }  // namespace

  manager_t &instance() {
    std::lock_guard lock {slot_mutex};
    return *slot();
  }

  void install(std::unique_ptr<manager_t> manager) {
    std::unique_ptr<manager_t> old;
    {
      std::lock_guard lock {slot_mutex};
      old = std::exchange(slot(), manager ? std::move(manager) : std::make_unique<manager_t>(nullptr, nullptr));
    }
    // `old` is destroyed here, outside the lock (its destructor joins the keep-alive thread).
  }

  std::string effective_output_name(const std::string &configured) {
    if (const auto id {instance().active_device_id()}) {
      return *id;
    }
    return configured;
  }

  std::optional<config::video_t> video_config_for_launch(const config::video_t &base, display_choice_e choice, const std::string &client_uid, const mode_t &mode) {
    switch (choice) {
      case display_choice_e::upstream:
        return base;
      case display_choice_e::mirror:
        BOOST_LOG(info) << "Gabs Remote: client asked to mirror the physical monitor; displays will not be configured"sv;
        return std::nullopt;
      case display_choice_e::virtual_display:
        break;
    }

    const auto device_id {instance().ensure(client_uid, mode)};
    if (!device_id) {
      BOOST_LOG(warning) << "Gabs Remote: virtual display unavailable; streaming the physical monitor instead (mirror)"sv;
      return std::nullopt;
    }

    auto video {base};
    video.output_name = *device_id;
    video.dd.configuration_option = config::video_t::dd_t::config_option_e::ensure_only_display;
    video.dd.resolution_option = config::video_t::dd_t::resolution_option_e::automatic;
    video.dd.refresh_rate_option = config::video_t::dd_t::refresh_rate_option_e::automatic;
    return video;
  }

  bool should_revert_on_session_end(bool upstream_decision) {
    return upstream_decision || instance().active_device_id().has_value();
  }

  void prepare_launch(display_choice_e choice, const std::function<void()> &cancel_pending_revert, const std::function<void()> &revert_now) {
    cancel_pending_revert();
    if (choice != display_choice_e::virtual_display && instance().active_device_id()) {
      BOOST_LOG(info) << "Gabs Remote: this launch does not use the virtual display; restoring the physical displays first"sv;
      revert_now();
      instance().release_all();
    }
  }

#ifndef _WIN32
  std::unique_ptr<driver_t> make_platform_driver() {
    return nullptr;
  }
#endif

}  // namespace gabs::virtual_display
