/**
 * @file src/gabs/update_policy.h
 * @brief Gabs Remote: what the host updater does next (check, download, install or wait). Decision 0006.
 *
 * First check 2 minutes after start, then every 24 hours, and 1 hour after a failed check. A newer
 * version gets downloaded; a ready installer runs only with no session and no app running (the PC
 * "ready" state of decision 0004 counts as an app running).
 */
#pragma once

// standard includes
#include <chrono>
#include <optional>

// local includes
#include "update_manifest.h"

namespace gabs::update {

  enum class action_e {
    wait,
    check,
    download,
    install,
  };

  struct policy_input_t {
    std::chrono::seconds since_start;
    std::optional<std::chrono::seconds> since_last_check;  ///< empty before the first check
    bool last_check_failed;
    std::optional<version_t> available;  ///< version in the last verified manifest
    bool installer_ready;  ///< installer for `available` downloaded and verified
    version_t installed;
    int sessions;
    int running_apps;
    bool user_asked_check;
    bool user_asked_install;
  };

  action_e next_action(const policy_input_t &in);

}  // namespace gabs::update
