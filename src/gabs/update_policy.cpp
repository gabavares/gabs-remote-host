/**
 * @file src/gabs/update_policy.cpp
 * @brief Gabs Remote: what the host updater does next (decision 0006).
 */
// header include
#include "update_policy.h"

namespace gabs::update {

  namespace {
    using namespace std::chrono_literals;

    constexpr std::chrono::seconds first_check_delay = 2min;
    constexpr std::chrono::seconds check_interval = 24h;
    constexpr std::chrono::seconds retry_interval = 1h;
  }  // namespace

  action_e next_action(const policy_input_t &in) {
    if (in.user_asked_check) {
      return action_e::check;
    }

    const bool newer = in.available && *in.available > in.installed;
    if (newer && in.installer_ready) {
      // The installer stops the service: only with no session and no app (PC "ready", decision 0004).
      return (in.sessions == 0 && in.running_apps == 0) ? action_e::install : action_e::wait;
    }
    if (newer && !in.last_check_failed) {
      return action_e::download;
    }
    // After a failed check, download, hash check or installer start: nothing new until the next check.

    if (!in.since_last_check) {
      return in.since_start >= first_check_delay ? action_e::check : action_e::wait;
    }
    const auto interval = in.last_check_failed ? retry_interval : check_interval;
    return *in.since_last_check >= interval ? action_e::check : action_e::wait;
  }

}  // namespace gabs::update
