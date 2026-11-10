/**
 * @file src/gabs/update_service.h
 * @brief Gabs Remote: the host updater (decision 0006).
 *
 * Checks the Release manifest 2 minutes after start and every 24 hours, downloads a newer installer to
 * `config\updates` (admin and SYSTEM only), checks SHA-256 and the Ed25519 signature of the manifest,
 * shows it in the tray and the web panel, and runs the installer silently only with no session and no
 * app running. The installer is started outside the service job object, so it survives the service stop.
 */
#pragma once

// standard includes
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

// local includes
#include "update_manifest.h"
#include "src/platform/common.h"

namespace gabs::update {

  enum class state_e {
    idle,
    checking,
    up_to_date,
    downloading,
    ready,
    installing,
    error,
  };

  /**
   * @brief Lowercase name of the state, as the web panel uses it.
   */
  const char *to_string(state_e state);

  struct status_t {
    std::string installed;
    std::string latest;  ///< empty before the first good check
    std::string notes;
    state_e state;
    std::string error;  ///< pt-BR, for the panel
  };

  status_t status();

  /**
   * @brief Ask for a check now (panel button).
   */
  void request_check();

  /**
   * @brief Ask to install now. False with a session or app running, or with no installer ready.
   */
  bool request_install();

  /**
   * @brief Starts the updater thread (asks `next_action` every 60 s).
   */
  std::unique_ptr<platf::deinit_t> start();

  /**
   * @brief `<config dir>\updates`.
   */
  std::filesystem::path updates_dir();

  /**
   * @brief Checks size and SHA-256 of a downloaded `.part`; only then renames it to `expected.file`
   * next to it. On any mismatch the `.part` is deleted.
   */
  bool finish_download(const std::filesystem::path &part, const artifact_t &expected);

  /**
   * @brief Deletes `.part` files and installers of versions <= installed. Other files stay.
   */
  void clean_updates_dir(const std::filesystem::path &dir, const version_t &installed);

  /**
   * @brief Version in a `GabsRemote-Setup-x.y.z.exe` name.
   */
  std::optional<version_t> installer_version(const std::string &file_name);

  /**
   * @brief Windows: runs `"<exe>" /S /ATUALIZACAO` detached, outside the service job object.
   */
  bool launch_installer(const std::filesystem::path &exe);

}  // namespace gabs::update
