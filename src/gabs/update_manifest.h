/**
 * @file src/gabs/update_manifest.h
 * @brief Gabs Remote: the Release manifest (manifest.json + manifest.json.sig) and its Ed25519 check.
 *
 * Decision 0006: the manifest is signed on Gab's PC with an Ed25519 key that never leaves it. The host
 * only trusts a manifest whose signature matches the public key built in (`update_key.h`), and only
 * runs an installer whose SHA-256 matches the manifest. Format and validation rules: plan of phase 4.
 */
#pragma once

// standard includes
#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace gabs::update {

  struct version_t {
    int major;
    int minor;
    int patch;
    auto operator<=>(const version_t &) const = default;
  };

  /**
   * @brief Only `x.y.z`, each part 0-999 (1 to 3 digits).
   */
  std::optional<version_t> parse_version(std::string_view text);

  std::string to_string(const version_t &version);

  struct artifact_t {
    std::string file;
    std::string sha256;  ///< 64 lowercase hex digits
    std::uint64_t size;
  };

  struct manifest_t {
    version_t version;
    std::string date;
    std::string notes;
    artifact_t host;
    version_t min_app;
    artifact_t app;
    int version_code;
    version_t min_host;
  };

  /**
   * @brief Parses and validates a manifest. Empty if any rule fails (nothing gets downloaded).
   */
  std::optional<manifest_t> parse_manifest(std::string_view json);

  /**
   * @brief Ed25519 check of `data`. `signature` must have 64 bytes and `public_key_raw` 32.
   */
  bool verify_signature(std::string_view data, std::string_view signature, std::string_view public_key_raw);

  /**
   * @brief Signature first, then parse. Empty if either fails.
   */
  std::optional<manifest_t> load_verified(std::string_view json, std::string_view signature, std::string_view public_key_raw);

  /**
   * @brief SHA-256 of a file, as 64 lowercase hex digits. Empty on any error.
   */
  std::string sha256_file(const std::filesystem::path &file);

  /**
   * @brief Strict standard base64 decoding (with padding). Empty on invalid input.
   */
  std::optional<std::string> decode_base64(std::string_view text);

}  // namespace gabs::update
