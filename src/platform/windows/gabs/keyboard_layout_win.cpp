/**
 * @file src/platform/windows/gabs/keyboard_layout_win.cpp
 * @brief Gabs Remote: Windows backend for the session keyboard layout (`grKeyboard`).
 *
 * The host runs as a service, on another thread desktop than the user's windows. Each call runs on a
 * short-lived thread attached to the input desktop, reads the layout of the foreground window and asks
 * the windows to change it with WM_INPUTLANGCHANGEREQUEST (what the language bar does). With the Windows
 * default ("one input method for all apps") this changes the layout for every app.
 */
// platform includes
#include <Windows.h>

// standard includes
#include <functional>
#include <string>
#include <thread>

// local includes
#include "src/gabs/keyboard_layout.h"
#include "src/logging.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

using namespace std::literals;

namespace gabs::keyboard_layout {
  namespace {
    /**
     * @brief Run a function on a thread attached to the input desktop and wait for it.
     */
    template<class T>
    T on_input_desktop(const std::function<T()> &work) {
      T result {};
      std::thread worker {[&]() {
        platf::syncThreadDesktop();
        result = work();
      }};
      worker.join();
      return result;
    }

    bool request_layout(HKL layout) {
      const auto foreground {GetForegroundWindow()};
      if (!foreground) {
        return false;
      }
      const auto lparam {reinterpret_cast<LPARAM>(layout)};
      const bool posted {PostMessageW(foreground, WM_INPUTLANGCHANGEREQUEST, 0, lparam) != 0};
      // Other top-level windows too, for the "different input method for each app" setting.
      PostMessageW(HWND_BROADCAST, WM_INPUTLANGCHANGEREQUEST, 0, lparam);
      return posted;
    }

    class windows_backend_t: public backend_t {
    public:
      std::optional<std::uintptr_t> current() override {
        return on_input_desktop<std::optional<std::uintptr_t>>([]() -> std::optional<std::uintptr_t> {
          const auto foreground {GetForegroundWindow()};
          if (!foreground) {
            return std::nullopt;
          }
          const auto thread_id {GetWindowThreadProcessId(foreground, nullptr)};
          const auto layout {GetKeyboardLayout(thread_id)};
          if (!layout) {
            return std::nullopt;
          }
          return reinterpret_cast<std::uintptr_t>(layout);
        });
      }

      std::optional<std::uintptr_t> activate_layout(const std::string &klid) override {
        return on_input_desktop<std::optional<std::uintptr_t>>([&klid]() -> std::optional<std::uintptr_t> {
          const std::wstring wide {klid.begin(), klid.end()};
          const auto layout {LoadKeyboardLayoutW(wide.c_str(), KLF_NOTELLSHELL)};
          if (!layout) {
            BOOST_LOG(warning) << "Gabs Remote: could not load keyboard layout "sv << klid << " [0x"sv << util::hex(GetLastError()).to_string_view() << ']';
            return std::nullopt;
          }
          if (!request_layout(layout)) {
            return std::nullopt;
          }
          return reinterpret_cast<std::uintptr_t>(layout);
        });
      }

      bool activate_handle(std::uintptr_t handle) override {
        return on_input_desktop<bool>([handle]() {
          return request_layout(reinterpret_cast<HKL>(handle));
        });
      }
    };
  }  // namespace

  std::unique_ptr<backend_t> make_platform_backend() {
    return std::make_unique<windows_backend_t>();
  }
}  // namespace gabs::keyboard_layout
