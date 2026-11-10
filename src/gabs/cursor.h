/**
 * @file src/gabs/cursor.h
 * @brief Gabs Remote: cursor drawn on the tablet (`grCursor` launch parameter).
 *
 * The cursor drawn inside the video arrives with all the latency of the picture (about 40 ms away from home).
 * With `grCursor=local` the app draws its own cursor at once and sends absolute positions; the host stops
 * drawing the cursor into the video for the session and tells the app which Windows cursor is showing
 * (arrow, text, hand...) through the `/gabs/cursor` long poll, so the app can draw the same shape.
 */
#pragma once

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace gabs::cursor {

  /**
   * @brief Shape of the Windows cursor, as the app knows how to draw it.
   */
  enum class shape_e {
    hidden,  ///< No cursor on screen (typing, games, touch or pen in use).
    arrow,
    ibeam,  ///< Text.
    hand,  ///< Link.
    wait,
    app_starting,  ///< Arrow with the busy sign.
    cross,
    size_ns,
    size_we,
    size_nwse,
    size_nesw,
    size_all,
    no,
    help,
    other,  ///< A cursor of the application itself (the app draws the arrow).
  };

  /**
   * @brief Name of a shape in the `/gabs/cursor` reply.
   * @param shape Shape.
   * @return Lower-case name (for example `ibeam`).
   */
  std::string_view shape_name(shape_e shape);

  /**
   * @brief Parse the `grCursor` launch argument.
   * @param arg Raw value.
   * @return True for `local` (the app draws the cursor); false for anything else.
   */
  bool parse_cursor_choice(std::string_view arg);

  /**
   * @brief Version and shape the app gets from `/gabs/cursor`.
   */
  struct state_t {
    std::uint64_t version;  ///< Grows by one at each shape change (starts at 1).
    shape_e shape;
  };

  /**
   * @brief Body of the `/gabs/cursor` reply.
   * @param state State to send.
   * @return `<version> <shape>`, for example `7 ibeam`.
   */
  std::string format_reply(const state_t &state);

  /**
   * @brief Parse the `v` query argument of `/gabs/cursor` (the version the app already has).
   * @param arg Raw value.
   * @return The version, or 0 (= reply at once) for anything that is not a number.
   */
  std::uint64_t parse_known(std::string_view arg);

  /**
   * @brief Reads the shape of the cursor on screen.
   */
  using sampler_t = std::function<shape_e()>;

  /**
   * @brief Turns the cursor drawn into the video on or off.
   */
  using video_cursor_t = std::function<void(bool)>;

  /**
   * @brief Reply to a waiting `/gabs/cursor` request.
   */
  using reply_t = std::function<void(const state_t &)>;

  /**
   * @brief Follows the cursor of the PC while a session asked for `grCursor=local`.
   */
  class tracker_t {
  public:
    /**
     * @brief Construct the tracker.
     * @param sampler Reads the cursor shape (null = always `arrow`).
     * @param video_cursor Turns the cursor in the video on or off (null = do nothing).
     * @param interval Time between samples; zero = no sampling thread (call `tick`).
     * @param poll_timeout How long a `/gabs/cursor` request waits for a change before getting the current state.
     */
    tracker_t(sampler_t sampler, video_cursor_t video_cursor, std::chrono::milliseconds interval = std::chrono::milliseconds {0}, std::chrono::milliseconds poll_timeout = std::chrono::seconds {10});

    ~tracker_t();
    tracker_t(const tracker_t &) = delete;
    tracker_t &operator=(const tracker_t &) = delete;

    /**
     * @brief Apply the choice of a launch or resume.
     * @param local True when the app draws the cursor (`grCursor=local`).
     */
    void apply(bool local);

    /**
     * @brief The session ended: the cursor goes back into the video.
     */
    void restore();

    /**
     * @brief Whether the app is drawing the cursor.
     * @return True between `apply(true)` and `restore()`.
     */
    bool local() const;

    /**
     * @brief Current version and shape.
     * @return The state.
     */
    state_t state() const;

    /**
     * @brief A `/gabs/cursor` request: reply at once if the app's version is old, or when the shape changes,
     * or after the poll timeout.
     * @param known Version the app already has.
     * @param reply Called once, possibly from another thread.
     */
    void poll(std::uint64_t known, reply_t reply);

    /**
     * @brief One sample: read the shape (when local) and answer the waiting requests that changed or timed out.
     * @param now Current time.
     */
    void tick(std::chrono::steady_clock::time_point now);

    /**
     * @brief Requests still waiting.
     * @return How many.
     */
    std::size_t waiting() const;

  private:
    struct waiter_t {
      std::chrono::steady_clock::time_point since;
      reply_t reply;
    };

    void loop();

    sampler_t sampler;
    video_cursor_t video_cursor;
    const std::chrono::milliseconds interval;
    const std::chrono::milliseconds poll_timeout;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool is_local {false};
    state_t current {1, shape_e::arrow};
    std::vector<waiter_t> waiters;
    bool stopping {false};
    std::thread sampler_thread;
  };

  /**
   * @brief Process-wide tracker.
   * @return The installed tracker (one without sampler until `install` is called).
   */
  tracker_t &instance();

  /**
   * @brief Install the process-wide tracker.
   * @param tracker Tracker to use (null = one without sampler).
   */
  void install(std::unique_ptr<tracker_t> tracker);

  /**
   * @brief Sampler for this platform.
   * @return The sampler, or null when the platform has none.
   */
  sampler_t make_platform_sampler();

}  // namespace gabs::cursor
