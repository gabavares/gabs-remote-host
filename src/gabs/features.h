/**
 * @file src/gabs/features.h
 * @brief Gabs Remote: what this host can do, sent as `GabsFeatures` in `/serverinfo`.
 *
 * Space-separated words in any order: `mic`, `clipboard`, `files`, `actions`. The app only shows
 * what the host lists, so a newer app works with an older host.
 */
#pragma once

// standard includes
#include <string>

namespace gabs {
  /**
   * @brief Feature words of this host.
   */
  std::string features();
}  // namespace gabs
