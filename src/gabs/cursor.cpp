/**
 * @file src/gabs/cursor.cpp
 * @brief Gabs Remote: cursor drawn on the tablet (`grCursor`).
 */
// local includes
#include "cursor.h"

// standard includes
#include <charconv>
#include <optional>

namespace gabs::cursor {

  std::string_view shape_name(shape_e shape) {
    switch (shape) {
      case shape_e::hidden:
        return "hidden";
      case shape_e::arrow:
        return "arrow";
      case shape_e::ibeam:
        return "ibeam";
      case shape_e::hand:
        return "hand";
      case shape_e::wait:
        return "wait";
      case shape_e::app_starting:
        return "app_starting";
      case shape_e::cross:
        return "cross";
      case shape_e::size_ns:
        return "size_ns";
      case shape_e::size_we:
        return "size_we";
      case shape_e::size_nwse:
        return "size_nwse";
      case shape_e::size_nesw:
        return "size_nesw";
      case shape_e::size_all:
        return "size_all";
      case shape_e::no:
        return "no";
      case shape_e::help:
        return "help";
      case shape_e::other:
      default:
        return "other";
    }
  }

  bool parse_cursor_choice(std::string_view arg) {
    return arg == "local";
  }

  std::string format_reply(const state_t &state) {
    std::string text = std::to_string(state.version);
    text += ' ';
    text += shape_name(state.shape);
    return text;
  }

  std::uint64_t parse_known(std::string_view arg) {
    std::uint64_t value {0};
    const auto end = arg.data() + arg.size();
    auto [ptr, ec] = std::from_chars(arg.data(), end, value);
    if (ec != std::errc {} || ptr != end) {
      return 0;
    }
    return value;
  }

  tracker_t::tracker_t(sampler_t sampler, video_cursor_t video_cursor, std::chrono::milliseconds interval, std::chrono::milliseconds poll_timeout):
      sampler {std::move(sampler)},
      video_cursor {std::move(video_cursor)},
      interval {interval},
      poll_timeout {poll_timeout} {
    if (interval.count() > 0) {
      sampler_thread = std::thread {&tracker_t::loop, this};
    }
  }

  tracker_t::~tracker_t() {
    {
      std::lock_guard lock {mutex};
      stopping = true;
    }
    wake.notify_all();
    if (sampler_thread.joinable()) {
      sampler_thread.join();
    }
  }

  void tracker_t::apply(bool local) {
    std::lock_guard lock {mutex};
    if (local == is_local) {
      return;
    }
    is_local = local;
    if (video_cursor) {
      video_cursor(!local);
    }
  }

  void tracker_t::restore() {
    std::vector<waiter_t> answered;
    state_t now_state;
    {
      std::lock_guard lock {mutex};
      if (is_local) {
        is_local = false;
        if (video_cursor) {
          video_cursor(true);
        }
      }
      answered.swap(waiters);
      now_state = current;
    }
    for (auto &w : answered) {
      w.reply(now_state);
    }
  }

  bool tracker_t::local() const {
    std::lock_guard lock {mutex};
    return is_local;
  }

  state_t tracker_t::state() const {
    std::lock_guard lock {mutex};
    return current;
  }

  void tracker_t::poll(std::uint64_t known, reply_t reply) {
    if (!reply) {
      return;
    }
    state_t now_state;
    {
      std::lock_guard lock {mutex};
      if (known == current.version) {
        waiters.push_back({std::chrono::steady_clock::now(), std::move(reply)});
        return;
      }
      now_state = current;
    }
    reply(now_state);
  }

  void tracker_t::tick(std::chrono::steady_clock::time_point now) {
    sampler_t read;
    {
      std::lock_guard lock {mutex};
      if (is_local) {
        read = sampler;
      }
    }
    // Read outside the lock: the platform call may take a moment.
    std::optional<shape_e> sampled;
    if (read) {
      sampled = read();
    } else {
      std::lock_guard lock {mutex};
      if (is_local) {
        sampled = shape_e::arrow;
      }
    }

    std::vector<waiter_t> answered;
    state_t now_state;
    {
      std::lock_guard lock {mutex};
      bool changed = false;
      if (sampled && is_local && *sampled != current.shape) {
        current.shape = *sampled;
        ++current.version;
        changed = true;
      }
      for (auto it = waiters.begin(); it != waiters.end();) {
        if (changed || now - it->since >= poll_timeout) {
          answered.push_back(std::move(*it));
          it = waiters.erase(it);
        } else {
          ++it;
        }
      }
      now_state = current;
    }
    for (auto &w : answered) {
      w.reply(now_state);
    }
  }

  std::size_t tracker_t::waiting() const {
    std::lock_guard lock {mutex};
    return waiters.size();
  }

  void tracker_t::loop() {
    std::unique_lock lock {mutex};
    while (!stopping) {
      wake.wait_for(lock, interval);
      if (stopping) {
        break;
      }
      lock.unlock();
      tick(std::chrono::steady_clock::now());
      lock.lock();
    }
  }

  namespace {
    std::unique_ptr<tracker_t> &installed() {
      static std::unique_ptr<tracker_t> tracker;
      return tracker;
    }
  }  // namespace

  tracker_t &instance() {
    auto &tracker = installed();
    if (!tracker) {
      tracker = std::make_unique<tracker_t>(nullptr, nullptr);
    }
    return *tracker;
  }

  void install(std::unique_ptr<tracker_t> tracker) {
    installed() = tracker ? std::move(tracker) : std::make_unique<tracker_t>(nullptr, nullptr);
  }

#ifndef _WIN32
  sampler_t make_platform_sampler() {
    return nullptr;
  }
#endif

}  // namespace gabs::cursor
