/**
 * @file src/gabs/virtual_display.h
 * @brief Gabs Remote: per-client virtual display requested by the app with the `grDisplay` launch parameter.
 *
 * The app asks for `grDisplay=virtual` (display at the tablet's resolution) or `grDisplay=mirror`
 * (stream the physical monitor without touching it). Without the parameter the host behaves like upstream.
 * The display itself is created by a driver behind `driver_t` (SudoVDA on Windows, decision 0003).
 */
#pragma once

// standard includes
#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

// local includes
#include "src/config.h"

namespace gabs::virtual_display {

  /**
   * @brief Display mode requested for a virtual display.
   */
  struct mode_t {
    int width;  ///< Width in pixels.
    int height;  ///< Height in pixels.
    int refresh_hz;  ///< Refresh rate in Hz.

    bool operator==(const mode_t &) const = default;
  };

  /**
   * @brief What the client asked for in `grDisplay`.
   */
  enum class display_choice_e {
    upstream,  ///< Parameter absent or unknown: behave like upstream.
    mirror,  ///< Stream the physical monitor and do not configure displays.
    virtual_display  ///< Create a virtual display for this client.
  };

  /**
   * @brief Parse the `grDisplay` launch argument (case-sensitive).
   * @param arg Raw argument value.
   * @return `virtual_display` for "virtual", `mirror` for "mirror", `upstream` otherwise.
   */
  display_choice_e parse_display_choice(std::string_view arg);

  /**
   * @brief Stable identifier of the paired client.
   *
   * Moonlight clients send the same `uniqueid` ("0123456789ABCDEF"), so the paired client certificate is preferred.
   * @param client_cert PEM certificate of the paired client (may be empty).
   * @param unique_id `uniqueid` launch argument.
   * @return The certificate when present, otherwise the unique id.
   */
  std::string client_uid_from(std::string_view client_cert, std::string_view unique_id);

  /**
   * @brief Convert the launch `fps` (Hz, or millihertz from some clients) to whole Hz.
   * @param fps Value from the launch `mode` argument.
   * @return Refresh rate in Hz; 60 when the client sent 0.
   */
  int refresh_hz_from_fps(int fps);

  /**
   * @brief Name-based UUID (version 5, SHA-1) for the monitor of a client, stable across host restarts.
   * @param client_uid Client identifier from `client_uid_from`.
   * @return The 16 UUID bytes in RFC 4122 order.
   */
  std::array<std::uint8_t, 16> monitor_guid_for(std::string_view client_uid);

  /**
   * @brief Driver that creates and removes virtual displays.
   */
  class driver_t {
  public:
    virtual ~driver_t() = default;

    /**
     * @brief Whether the driver is installed and usable.
     * @return True when displays can be created.
     */
    virtual bool available() = 0;

    /**
     * @brief Create the virtual display of a client.
     * @param client_uid Client identifier.
     * @param mode Requested mode.
     * @return GDI display name (`\\.\DISPLAYn`), or empty on failure.
     */
    virtual std::optional<std::string> add(const std::string &client_uid, const mode_t &mode) = 0;

    /**
     * @brief Remove the virtual display of a client.
     * @param client_uid Client identifier.
     * @return True on success.
     */
    virtual bool remove(const std::string &client_uid) = 0;

    /**
     * @brief Remove every virtual display this host knows about.
     */
    virtual void remove_all() = 0;

    /**
     * @brief Tell the driver the host is alive (its watchdog removes displays of a dead host).
     */
    virtual void keep_alive() = 0;
  };

  /**
   * @brief Owns the active virtual display (one client at a time in phase 2).
   */
  class manager_t {
  public:
    /**
     * @brief Resolves a GDI display name to the libdisplaydevice device id.
     */
    using device_id_for_t = std::function<std::optional<std::string>(const std::string &display_name)>;

    /**
     * @brief Construct a manager.
     * @param driver Driver used to create displays (may be null: no virtual displays).
     * @param device_id_for Resolver from display name to device id.
     */
    manager_t(std::unique_ptr<driver_t> driver, device_id_for_t device_id_for);
    ~manager_t();

    manager_t(const manager_t &) = delete;
    manager_t &operator=(const manager_t &) = delete;

    /**
     * @brief Make sure the client has a virtual display with this mode, reusing the existing one when it matches.
     * @param client_uid Client identifier.
     * @param mode Requested mode.
     * @return Device id of the display, or empty when it could not be created.
     */
    std::optional<std::string> ensure(const std::string &client_uid, const mode_t &mode);

    /**
     * @brief Remove the active virtual display, if any.
     */
    void release_all();

    /**
     * @brief Remove leftovers of a previous host run.
     */
    void startup_cleanup();

    /**
     * @brief Device id of the active virtual display.
     * @return The device id, or empty when no virtual display is active.
     */
    std::optional<std::string> active_device_id() const;

    /**
     * @brief GDI name of the active virtual display.
     * @return The name, or empty when no virtual display is active.
     */
    std::optional<std::string> active_display() const;

  private:
    void start_keep_alive();
    void stop_keep_alive();
    void remove_active_unlocked();

    std::unique_ptr<driver_t> driver;
    device_id_for_t device_id_for;

    mutable std::mutex mutex;
    std::optional<std::string> active_client;
    std::optional<mode_t> active_mode;
    std::optional<std::string> active_display_name;
    std::optional<std::string> active_id;

    // The keep-alive thread never takes `mutex`, so it keeps pinging while `ensure` waits on the driver.
    std::mutex keep_alive_mutex;
    std::condition_variable keep_alive_wake;
    bool keep_alive_running {false};
    std::thread keep_alive_thread;
  };

  /**
   * @brief The manager used by the host.
   * @return The installed manager, or a driverless one.
   */
  manager_t &instance();

  /**
   * @brief Replace the manager used by the host.
   * @param manager New manager; null installs a driverless one.
   */
  void install(std::unique_ptr<manager_t> manager);

  /**
   * @brief Output the capture should use.
   * @param configured `output_name` from the configuration.
   * @return Device id of the active virtual display, or `configured`.
   */
  std::string effective_output_name(const std::string &configured);

  /**
   * @brief Video configuration for the display-device step of a launch.
   * @param base Configured video settings.
   * @param choice Client's `grDisplay` choice.
   * @param client_uid Client identifier.
   * @param mode Requested mode.
   * @return `base` for upstream; empty for mirror (do not configure) or when the virtual display fails;
   *         otherwise `base` with the virtual display as the only display at the requested mode.
   */
  std::optional<config::video_t> video_config_for_launch(const config::video_t &base, display_choice_e choice, const std::string &client_uid, const mode_t &mode);

  /**
   * @brief Whether the display configuration must be reverted when the last session ends.
   *
   * Upstream keeps the configuration while an app is still "running" (the Desktop app never stops),
   * but a virtual display must go away on disconnect, or the physical monitors stay off.
   * @param upstream_decision What upstream decided (`config_revert_on_disconnect` or no app running).
   * @return True to revert.
   */
  bool should_revert_on_session_end(bool upstream_decision);

  /**
   * @brief Prepare the displays before a launch/resume configures them.
   *
   * Always cancels a pending (delayed) revert first, so it cannot remove a virtual display that this
   * launch is about to reuse. When the client does not want a virtual display but one is active,
   * reverts right away and removes it, so the capture goes back to the physical monitor.
   * @param choice Client's `grDisplay` choice.
   * @param cancel_pending_revert Stops any scheduled revert (waits for one in progress).
   * @param revert_now Reverts the display configuration synchronously.
   */
  void prepare_launch(display_choice_e choice, const std::function<void()> &cancel_pending_revert, const std::function<void()> &revert_now);

  /**
   * @brief Parse the `grScale` launch argument (Windows display scale in percent).
   * @param arg Raw value.
   * @return One of Windows' scale steps (100, 125, ... 500), or empty for anything else (do not change).
   */
  std::optional<int> parse_scale(std::string_view arg);

  /**
   * @brief Step to pass to Windows to get a scale, relative to the recommended scale of the display.
   * @param percent Target scale in percent.
   * @param min_relative Lowest relative step Windows reports for the display (minus the index of the recommended scale).
   * @return The relative step, or empty if `percent` is not a Windows scale step.
   */
  std::optional<int> dpi_relative_step(int percent, int min_relative);

  /**
   * @brief One log line with what the client asked for in the launch.
   * @param mode `mode` argument (WxHxFPS).
   * @param gr_display `grDisplay` argument.
   * @param gr_scale `grScale` argument.
   * @return Text for the log.
   */
  std::string describe_launch(std::string_view mode, std::string_view gr_display, std::string_view gr_scale);

  /**
   * @brief Set the Windows display scale of a display.
   * @param display_name GDI name (`\\.\DISPLAYn`).
   * @param percent Scale in percent (a Windows step).
   * @return True on success; false on other platforms or failure.
   */
  bool set_display_scale(const std::string &display_name, int percent);

  /**
   * @brief GDI name of the active virtual display.
   * @return The name, or empty when no virtual display is active.
   */
  std::optional<std::string> active_display_name();

  /**
   * @brief Apply the client's Windows scale to the virtual display, after the displays were configured.
   * @param choice Client's `grDisplay` choice (only `virtual_display` is scaled).
   * @param scale Parsed `grScale` (empty = do not change).
   */
  void apply_scale_for_launch(display_choice_e choice, std::optional<int> scale);

  /**
   * @brief Driver for this platform (SudoVDA on Windows, none elsewhere).
   * @return The driver, or null when the platform has none.
   */
  std::unique_ptr<driver_t> make_platform_driver();

}  // namespace gabs::virtual_display
