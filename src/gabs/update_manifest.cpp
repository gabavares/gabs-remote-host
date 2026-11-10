/**
 * @file src/gabs/update_manifest.cpp
 * @brief Gabs Remote: the Release manifest and its Ed25519 check (decision 0006).
 */
// header include
#include "update_manifest.h"

// standard includes
#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <memory>
#include <regex>

// lib includes
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

namespace gabs::update {

  namespace {
    bool all_digits(std::string_view s) {
      if (s.empty() || s.size() > 3) {
        return false;
      }
      for (char c : s) {
        if (c < '0' || c > '9') {
          return false;
        }
      }
      return true;
    }

    bool valid_file_name(const std::string &name) {
      static const std::regex pattern {"^[A-Za-z0-9._-]+$"};
      return std::regex_match(name, pattern) && name.find("..") == std::string::npos;
    }

    bool valid_sha256(const std::string &hex) {
      if (hex.size() != 64) {
        return false;
      }
      for (char c : hex) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
          return false;
        }
      }
      return true;
    }

    std::optional<version_t> version_field(const nlohmann::json &obj, const char *key) {
      auto it = obj.find(key);
      if (it == obj.end() || !it->is_string()) {
        return std::nullopt;
      }
      return parse_version(it->get<std::string>());
    }

    std::optional<artifact_t> artifact_field(const nlohmann::json &obj, const char *key) {
      auto it = obj.find(key);
      if (it == obj.end() || !it->is_object()) {
        return std::nullopt;
      }
      const auto &a = *it;
      auto file = a.find("file");
      auto sha = a.find("sha256");
      auto size = a.find("size");
      if (file == a.end() || !file->is_string() || sha == a.end() || !sha->is_string() ||
          size == a.end() || !size->is_number_integer()) {
        return std::nullopt;
      }
      artifact_t out;
      out.file = file->get<std::string>();
      out.sha256 = sha->get<std::string>();
      if (size->is_number_unsigned()) {
        out.size = size->get<std::uint64_t>();
      } else {
        auto s = size->get<std::int64_t>();
        if (s <= 0) {
          return std::nullopt;
        }
        out.size = static_cast<std::uint64_t>(s);
      }
      if (!valid_file_name(out.file) || !valid_sha256(out.sha256) || out.size == 0) {
        return std::nullopt;
      }
      return out;
    }

    int expected_version_code(const version_t &v) {
      return v.major * 1000000 + v.minor * 1000 + v.patch;
    }
  }  // namespace

  std::optional<version_t> parse_version(std::string_view text) {
    std::array<int, 3> parts {};
    std::size_t start = 0;
    for (int i = 0; i < 3; ++i) {
      auto end = i < 2 ? text.find('.', start) : text.size();
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      auto piece = text.substr(start, end - start);
      if (!all_digits(piece)) {
        return std::nullopt;
      }
      std::from_chars(piece.data(), piece.data() + piece.size(), parts[i]);
      start = end + 1;
    }
    return version_t {parts[0], parts[1], parts[2]};
  }

  std::string to_string(const version_t &version) {
    return std::to_string(version.major) + "." + std::to_string(version.minor) + "." + std::to_string(version.patch);
  }

  std::optional<manifest_t> parse_manifest(std::string_view json) {
    auto doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
      return std::nullopt;
    }
    auto schema = doc.find("schema");
    if (schema == doc.end() || !schema->is_number_integer() || schema->get<std::int64_t>() != 1) {
      return std::nullopt;
    }
    auto version = version_field(doc, "version");
    auto date = doc.find("date");
    auto notes = doc.find("notes");
    if (!version || date == doc.end() || !date->is_string() || notes == doc.end() || !notes->is_string()) {
      return std::nullopt;
    }
    auto host = artifact_field(doc, "host");
    auto app = artifact_field(doc, "app");
    if (!host || !app) {
      return std::nullopt;
    }
    auto min_app = version_field(doc["host"], "minApp");
    auto min_host = version_field(doc["app"], "minHost");
    auto code = doc["app"].find("versionCode");
    if (!min_app || !min_host || code == doc["app"].end() || !code->is_number_integer() ||
        code->get<std::int64_t>() != expected_version_code(*version)) {
      return std::nullopt;
    }

    manifest_t m;
    m.version = *version;
    m.date = date->get<std::string>();
    m.notes = notes->get<std::string>();
    m.host = *host;
    m.min_app = *min_app;
    m.app = *app;
    m.version_code = static_cast<int>(code->get<std::int64_t>());
    m.min_host = *min_host;
    return m;
  }

  bool verify_signature(std::string_view data, std::string_view signature, std::string_view public_key_raw) {
    if (signature.size() != 64 || public_key_raw.size() != 32) {
      return false;
    }
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key {
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, reinterpret_cast<const unsigned char *>(public_key_raw.data()), public_key_raw.size()),
      &EVP_PKEY_free
    };
    if (!key) {
      return false;
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx {EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (!ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
      return false;
    }
    return EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char *>(signature.data()), signature.size(), reinterpret_cast<const unsigned char *>(data.data()), data.size()) == 1;
  }

  std::optional<manifest_t> load_verified(std::string_view json, std::string_view signature, std::string_view public_key_raw) {
    if (!verify_signature(json, signature, public_key_raw)) {
      return std::nullopt;
    }
    return parse_manifest(json);
  }

  std::string sha256_file(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      return {};
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx {EVP_MD_CTX_new(), &EVP_MD_CTX_free};
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
      return {};
    }
    std::array<char, 64 * 1024> buf {};
    while (in) {
      in.read(buf.data(), buf.size());
      auto n = in.gcount();
      if (n > 0 && EVP_DigestUpdate(ctx.get(), buf.data(), static_cast<std::size_t>(n)) != 1) {
        return {};
      }
    }
    if (in.bad()) {
      return {};
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> md {};
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), md.data(), &len) != 1) {
      return {};
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
      out.push_back(hex[md[i] >> 4]);
      out.push_back(hex[md[i] & 0xf]);
    }
    return out;
  }

  std::optional<std::string> decode_base64(std::string_view text) {
    if (text.empty() || text.size() % 4 != 0) {
      return std::nullopt;
    }
    auto value = [](char c) -> int {
      if (c >= 'A' && c <= 'Z') {
        return c - 'A';
      }
      if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
      }
      if (c >= '0' && c <= '9') {
        return c - '0' + 52;
      }
      if (c == '+') {
        return 62;
      }
      if (c == '/') {
        return 63;
      }
      return -1;
    };
    std::string out;
    for (std::size_t i = 0; i < text.size(); i += 4) {
      int v[4];
      int pad = 0;
      for (int j = 0; j < 4; ++j) {
        char c = text[i + j];
        if (c == '=') {
          if (i + 4 != text.size() || j < 2) {
            return std::nullopt;
          }
          v[j] = 0;
          ++pad;
        } else {
          if (pad > 0) {
            return std::nullopt;
          }
          v[j] = value(c);
          if (v[j] < 0) {
            return std::nullopt;
          }
        }
      }
      std::uint32_t triple = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
      out.push_back(static_cast<char>((triple >> 16) & 0xff));
      if (pad < 2) {
        out.push_back(static_cast<char>((triple >> 8) & 0xff));
      }
      if (pad < 1) {
        out.push_back(static_cast<char>(triple & 0xff));
      }
    }
    return out;
  }

}  // namespace gabs::update
