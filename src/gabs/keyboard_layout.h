/**
 * @file src/gabs/keyboard_layout.h
 * @brief Gabs Remote: Windows keyboard layout switched during a session (`grKeyboard` launch parameter).
 *
 * The PC uses the layout of its physical keyboard (English); the tablet's keyboard is Portuguese (ABNT2).
 * The app sends `grKeyboard=<KLID>` (for example `00010416`) and the host switches the active layout for
 * the session, going back to the previous one when the last session ends.
 */
#pragma once

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace gabs::keyboard_layout {

  /**
   * @brief Parse the `grKeyboard` launch argument.
   * @param arg Raw value.
   * @return The keyboard layout id (KLID, 8 hex digits, upper case), or empty for anything else (do not change).
   */
  std::optional<std::string> parse_layout(std::string_view arg);

  /**
   * @brief Platform access to the active keyboard layout.
   */
  class backend_t {
  public:
    virtual ~backend_t() = default;

    /**
     * @brief Layout active in the foreground window.
     * @return Opaque handle (HKL on Windows), or empty when it cannot be read.
     */
    virtual std::optional<std::uintptr_t> current() = 0;

    /**
     * @brief Name of the desktop that receives the input ("Default", or "Winlogon" on the login, lock and UAC screens).
     * @return The name, or empty when it cannot be read.
     */
    virtual std::optional<std::string> input_desktop() = 0;

    /**
     * @brief Activate a layout by its KLID.
     * @param klid Keyboard layout id (8 hex digits).
     * @return Handle of the activated layout (same kind as `current()`), or empty on failure.
     */
    virtual std::optional<std::uintptr_t> activate_layout(const std::string &klid) = 0;

    /**
     * @brief Activate a layout by a handle returned from `current()`.
     * @param handle Handle to activate.
     * @return True on success.
     */
    virtual bool activate_handle(std::uintptr_t handle) = 0;
  };

  /**
   * @brief Switches the layout for a session and remembers the one to go back to.
   *
   * Right after a switch the host also turns the virtual display on or off, and Windows moves the focus
   * while it rearranges the windows; a window still in the old layout can end up in the foreground. So for
   * a while after each switch (and each restore) the switcher checks the foreground window and asks again.
   *
   * The login, lock and UAC screens are another desktop, with their own windows and layouts. During a session
   * the switcher follows the input desktop: on a change it asks again for the session layout, and the first
   * time it sees a desktop it saves that desktop's layout, so the restore uses the one of the desktop in use.
   */
  class switcher_t {
  public:
    /**
     * @brief Construct the switcher.
     * @param backend Platform backend (null = do nothing).
     * @param watch_interval Time between checks after a switch; zero = no watcher thread (call `reassert`).
     * @param watch_checks How many checks follow each switch or restore.
     */
    explicit switcher_t(std::unique_ptr<backend_t> backend, std::chrono::milliseconds watch_interval = std::chrono::milliseconds {0}, int watch_checks = 20);

    ~switcher_t();
    switcher_t(const switcher_t &) = delete;
    switcher_t &operator=(const switcher_t &) = delete;

    /**
     * @brief Apply the layout asked in a launch or resume.
     * @param klid Layout to activate; empty = go back to the saved layout, if any.
     */
    void apply(const std::optional<std::string> &klid);

    /**
     * @brief Go back to the layout saved before the session (no-op when nothing is saved).
     */
    void restore();

    /**
     * @brief Layout saved before the session.
     * @return The handle, or empty when no switch is in effect.
     */
    std::optional<std::uintptr_t> saved() const;

    /**
     * @brief One check after a switch or restore: ask again when the foreground window lost the layout.
     * @return True while the watch after the last switch or restore is still going.
     */
    bool reassert();

  private:
    void restore_unlocked();
    void follow_desktop();
    void watch(std::optional<std::uintptr_t> handle);
    bool reassert_unlocked();
    void watcher_loop();

    std::unique_ptr<backend_t> backend;
    const std::chrono::milliseconds watch_interval;
    const int watch_checks;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::optional<std::uintptr_t> saved_handle;  ///< Layout to go back to (from the desktop of the first switch).
    std::optional<std::uintptr_t> session_handle;  ///< Layout of the session while it is in effect.
    std::optional<std::string> current_desktop;
    std::map<std::string, std::uintptr_t> saved_by_desktop;
    std::optional<std::uintptr_t> wanted_handle;  ///< Layout the watch keeps in the foreground.
    int checks_left {0};
    bool stopping {false};
    std::thread watcher;
  };

  /**
   * @brief Process-wide switcher.
   * @return The installed switcher (one without backend until `install` is called).
   */
  switcher_t &instance();

  /**
   * @brief Install the process-wide switcher.
   * @param switcher Switcher to use (null = one without backend).
   */
  void install(std::unique_ptr<switcher_t> switcher);

  /**
   * @brief Backend for this platform.
   * @return The backend, or null when the platform has none.
   */
  std::unique_ptr<backend_t> make_platform_backend();

}  // namespace gabs::keyboard_layout
