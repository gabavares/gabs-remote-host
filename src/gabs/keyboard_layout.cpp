/**
 * @file src/gabs/keyboard_layout.cpp
 * @brief Gabs Remote: Windows keyboard layout switched during a session (`grKeyboard`).
 */
// standard includes
#include <cctype>
#include <utility>

// local includes
#include "keyboard_layout.h"
#include "src/logging.h"
#include "src/utility.h"

using namespace std::literals;

namespace gabs::keyboard_layout {

  std::optional<std::string> parse_layout(std::string_view arg) {
    if (arg.size() != 8) {
      return std::nullopt;
    }
    std::string klid;
    for (const char c : arg) {
      if (!std::isxdigit(static_cast<unsigned char>(c))) {
        return std::nullopt;
      }
      klid += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return klid;
  }

  switcher_t::switcher_t(std::unique_ptr<backend_t> backend, std::chrono::milliseconds watch_interval, int watch_checks):
      backend {std::move(backend)},
      watch_interval {watch_interval},
      watch_checks {watch_checks} {
    if (this->backend && watch_interval.count() > 0) {
      watcher = std::thread {&switcher_t::watcher_loop, this};
    }
  }

  switcher_t::~switcher_t() {
    {
      std::lock_guard lock {mutex};
      stopping = true;
    }
    wake.notify_all();
    if (watcher.joinable()) {
      watcher.join();
    }
  }

  void switcher_t::apply(const std::optional<std::string> &klid) {
    std::lock_guard lock {mutex};
    if (!klid) {
      restore_unlocked();
      return;
    }
    if (!backend) {
      return;
    }

    const bool saved_now {!saved_handle};
    if (saved_now) {
      // Keep the layout from before the first session; a resume must not save the session's own layout.
      saved_handle = backend->current();
      if (!saved_handle) {
        BOOST_LOG(warning) << "Gabs Remote: no foreground window to read the keyboard layout from; keeping it"sv;
        return;
      }
    }
    if (const auto handle {backend->activate_layout(*klid)}) {
      BOOST_LOG(info) << "Gabs Remote: keyboard layout "sv << *klid << " for the session"sv;
      watch(handle);
    } else {
      BOOST_LOG(warning) << "Gabs Remote: could not switch the keyboard layout to "sv << *klid;
      if (saved_now) {
        saved_handle.reset();
      }
    }
  }

  void switcher_t::restore() {
    std::lock_guard lock {mutex};
    restore_unlocked();
  }

  void switcher_t::restore_unlocked() {
    if (!saved_handle) {
      return;
    }
    const auto handle {*saved_handle};
    saved_handle.reset();
    if (backend && backend->activate_handle(handle)) {
      BOOST_LOG(info) << "Gabs Remote: keyboard layout restored"sv;
      watch(handle);
    } else {
      BOOST_LOG(warning) << "Gabs Remote: could not restore the keyboard layout"sv;
    }
  }

  std::optional<std::uintptr_t> switcher_t::saved() const {
    std::lock_guard lock {mutex};
    return saved_handle;
  }

  void switcher_t::watch(std::optional<std::uintptr_t> handle) {
    wanted_handle = handle;
    checks_left = handle ? watch_checks : 0;
    wake.notify_all();
  }

  bool switcher_t::reassert() {
    std::lock_guard lock {mutex};
    return reassert_unlocked();
  }

  bool switcher_t::reassert_unlocked() {
    if (!backend || !wanted_handle || checks_left <= 0) {
      return false;
    }
    --checks_left;
    const auto current {backend->current()};
    if (current && *current != *wanted_handle) {
      BOOST_LOG(info) << "Gabs Remote: foreground window has keyboard layout 0x"sv << util::hex(*current).to_string_view()
                      << ", asking again for 0x"sv << util::hex(*wanted_handle).to_string_view();
      backend->activate_handle(*wanted_handle);
    }
    return true;
  }

  void switcher_t::watcher_loop() {
    std::unique_lock lock {mutex};
    while (!stopping) {
      wake.wait(lock, [this]() {
        return stopping || checks_left > 0;
      });
      if (stopping) {
        break;
      }
      // Wait one interval before each check; a new switch during the wait just restarts the count.
      if (wake.wait_for(lock, watch_interval, [this]() {
            return stopping;
          })) {
        break;
      }
      reassert_unlocked();
    }
  }

  namespace {
    std::mutex slot_mutex;

    std::unique_ptr<switcher_t> &slot() {
      static std::unique_ptr<switcher_t> current {std::make_unique<switcher_t>(nullptr)};
      return current;
    }
  }  // namespace

  switcher_t &instance() {
    std::lock_guard lock {slot_mutex};
    return *slot();
  }

  void install(std::unique_ptr<switcher_t> switcher) {
    std::lock_guard lock {slot_mutex};
    slot() = switcher ? std::move(switcher) : std::make_unique<switcher_t>(nullptr);
  }

#ifndef _WIN32
  std::unique_ptr<backend_t> make_platform_backend() {
    return nullptr;
  }
#endif

}  // namespace gabs::keyboard_layout
