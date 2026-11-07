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

  switcher_t::switcher_t(std::unique_ptr<backend_t> backend):
      backend {std::move(backend)} {
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
    if (backend->activate_layout(*klid)) {
      BOOST_LOG(info) << "Gabs Remote: keyboard layout "sv << *klid << " for the session"sv;
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
    } else {
      BOOST_LOG(warning) << "Gabs Remote: could not restore the keyboard layout"sv;
    }
  }

  std::optional<std::uintptr_t> switcher_t::saved() const {
    std::lock_guard lock {mutex};
    return saved_handle;
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
