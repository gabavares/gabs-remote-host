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
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

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
     * @brief Activate a layout by its KLID.
     * @param klid Keyboard layout id (8 hex digits).
     * @return True on success.
     */
    virtual bool activate_layout(const std::string &klid) = 0;

    /**
     * @brief Activate a layout by a handle returned from `current()`.
     * @param handle Handle to activate.
     * @return True on success.
     */
    virtual bool activate_handle(std::uintptr_t handle) = 0;
  };

  /**
   * @brief Switches the layout for a session and remembers the one to go back to.
   */
  class switcher_t {
  public:
    /**
     * @brief Construct the switcher.
     * @param backend Platform backend (null = do nothing).
     */
    explicit switcher_t(std::unique_ptr<backend_t> backend);

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

  private:
    void restore_unlocked();

    std::unique_ptr<backend_t> backend;
    mutable std::mutex mutex;
    std::optional<std::uintptr_t> saved_handle;
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
