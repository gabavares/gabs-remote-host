/**
 * @file tests/unit/gabs/test_update.cpp
 * @brief Gabs Remote: tests for the updater (signed manifest, policy, downloaded files). Decision 0006.
 */

// test includes
#include "../../tests_common.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

// local includes
#include <src/gabs/update_manifest.h>
#include <src/gabs/update_policy.h>

namespace up = gabs::update;
using namespace std::chrono_literals;

namespace {
  std::filesystem::path testdata(const std::string &name) {
    return std::filesystem::path(SUNSHINE_SOURCE_DIR) / "tests" / "unit" / "gabs" / "testdata" / "update" / name;
  }

  std::string read_file(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  std::string test_key_raw() {
    auto b64 = read_file(testdata("chave-de-teste.pub.b64"));
    while (!b64.empty() && (b64.back() == '\n' || b64.back() == '\r')) {
      b64.pop_back();
    }
    auto raw = up::decode_base64(b64);
    return raw.value_or(std::string {});
  }

  up::version_t v(const char *text) {
    return up::parse_version(text).value();
  }

  std::string valid_manifest_json() {
    return R"({"schema": 1, "version": "0.4.1", "date": "2026-11-20", "notes": "x",)"
           R"( "host": {"file": "GabsRemote-Setup-0.4.1.exe", "sha256": ")" +
           std::string(64, 'a') + R"(", "size": 10, "minApp": "0.4.0"},)"
                                  R"( "app": {"file": "GabsRemote-0.4.1.apk", "sha256": ")" +
           std::string(64, 'b') + R"(", "size": 20, "versionCode": 4001, "minHost": "0.4.0"}})";
  }

  up::policy_input_t idle_input() {
    up::policy_input_t in {};
    in.since_start = 10min;
    in.since_last_check = 1h;
    in.last_check_failed = false;
    in.available = std::nullopt;
    in.installer_ready = false;
    in.installed = v("0.4.0");
    in.sessions = 0;
    in.running_apps = 0;
    in.user_asked_check = false;
    in.user_asked_install = false;
    return in;
  }
}  // namespace

TEST(UpdateVersion, ParsesAndCompares) {
  EXPECT_GT(v("0.4.1"), v("0.4.0"));
  EXPECT_GT(v("0.10.0"), v("0.9.9"));
  EXPECT_EQ(v("1.2.3"), v("1.2.3"));
  EXPECT_EQ(up::to_string(v("0.4.1")), "0.4.1");
  EXPECT_FALSE(up::parse_version("v0.4.0"));
  EXPECT_FALSE(up::parse_version("0.4"));
  EXPECT_FALSE(up::parse_version("1.2.3-beta"));
  EXPECT_FALSE(up::parse_version("1000.0.0"));
  EXPECT_FALSE(up::parse_version(""));
  EXPECT_FALSE(up::parse_version("1.2.3 "));
}

TEST(UpdateManifest, AcceptsSignedFixture) {
  auto key = test_key_raw();
  ASSERT_EQ(key.size(), 32u);
  auto m = up::load_verified(read_file(testdata("manifest.json")), read_file(testdata("manifest.json.sig")), key);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->version, v("0.4.1"));
  EXPECT_EQ(m->version_code, 4001);
  EXPECT_EQ(m->host.file, "GabsRemote-Setup-0.4.1.exe");
  EXPECT_EQ(m->host.sha256.size(), 64u);
  EXPECT_GT(m->host.size, 0u);
  EXPECT_EQ(m->app.file, "GabsRemote-0.4.1.apk");
  EXPECT_EQ(m->min_app, v("0.4.0"));
  EXPECT_EQ(m->min_host, v("0.4.0"));
  EXPECT_EQ(m->date, "2026-11-20");
  EXPECT_NE(m->notes.find("atualiza\xC3\xA7\xC3\xA3o"), std::string::npos);
}

TEST(UpdateManifest, RejectsTamperedManifest) {
  auto m = up::load_verified(read_file(testdata("manifest-adulterado.json")), read_file(testdata("manifest.json.sig")), test_key_raw());
  EXPECT_FALSE(m);
}

TEST(UpdateManifest, RejectsOtherKey) {
  auto m = up::load_verified(read_file(testdata("manifest.json")), read_file(testdata("manifest-outra-chave.json.sig")), test_key_raw());
  EXPECT_FALSE(m);
}

TEST(UpdateManifest, RejectsBadSignatureSizes) {
  auto json = read_file(testdata("manifest.json"));
  auto sig = read_file(testdata("manifest.json.sig"));
  EXPECT_FALSE(up::verify_signature(json, sig.substr(0, 63), test_key_raw()));
  EXPECT_FALSE(up::verify_signature(json, sig, test_key_raw().substr(0, 31)));
  EXPECT_TRUE(up::verify_signature(json, sig, test_key_raw()));
}

TEST(UpdateManifest, RejectsBadFields) {
  auto base = valid_manifest_json();
  ASSERT_TRUE(up::parse_manifest(base));

  auto with = [&](const std::string &from, const std::string &to) {
    auto s = base;
    auto pos = s.find(from);
    EXPECT_NE(pos, std::string::npos) << from;
    s.replace(pos, from.size(), to);
    return s;
  };
  EXPECT_FALSE(up::parse_manifest(with("\"GabsRemote-Setup-0.4.1.exe\"", "\"..\\\\x.exe\"")));
  EXPECT_FALSE(up::parse_manifest(with("\"GabsRemote-0.4.1.apk\"", "\"a..apk\"")));
  EXPECT_FALSE(up::parse_manifest(with(std::string(64, 'a'), std::string(63, 'a'))));
  EXPECT_FALSE(up::parse_manifest(with(std::string(64, 'b'), std::string(64, 'B'))));
  EXPECT_FALSE(up::parse_manifest(with("\"schema\": 1", "\"schema\": 2")));
  EXPECT_FALSE(up::parse_manifest(with("\"versionCode\": 4001", "\"versionCode\": 4000")));
  EXPECT_FALSE(up::parse_manifest(with("\"size\": 10", "\"size\": 0")));
  EXPECT_FALSE(up::parse_manifest(with("\"version\": \"0.4.1\"", "\"version\": \"v0.4.1\"")));
  EXPECT_FALSE(up::parse_manifest(with("\"minApp\": \"0.4.0\"", "\"minApp\": \"0.4\"")));
  EXPECT_FALSE(up::parse_manifest("{\"schema\": 1"));
  EXPECT_FALSE(up::parse_manifest("[]"));
}

TEST(UpdateManifest, Sha256File) {
  auto dir = std::filesystem::temp_directory_path() / "gabs-update-sha";
  std::filesystem::create_directories(dir);
  auto f = dir / "abc.bin";
  {
    std::ofstream out(f, std::ios::binary);
    out << "abc";
  }
  EXPECT_EQ(up::sha256_file(f), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(up::sha256_file(dir / "nao-existe.bin"), "");
  std::filesystem::remove_all(dir);
}

TEST(UpdatePolicy, FirstCheckAfterTwoMinutes) {
  auto in = idle_input();
  in.since_last_check = std::nullopt;
  in.since_start = 119s;
  EXPECT_EQ(up::next_action(in), up::action_e::wait);
  in.since_start = 120s;
  EXPECT_EQ(up::next_action(in), up::action_e::check);
}

TEST(UpdatePolicy, ChecksEvery24Hours) {
  auto in = idle_input();
  in.since_last_check = 23h + 59min;
  EXPECT_EQ(up::next_action(in), up::action_e::wait);
  in.since_last_check = 24h;
  EXPECT_EQ(up::next_action(in), up::action_e::check);
}

TEST(UpdatePolicy, RetriesAfterOneHourOnFailure) {
  auto in = idle_input();
  in.last_check_failed = true;
  in.since_last_check = 59min;
  EXPECT_EQ(up::next_action(in), up::action_e::wait);
  in.since_last_check = 1h;
  EXPECT_EQ(up::next_action(in), up::action_e::check);
}

TEST(UpdatePolicy, IgnoresOlderOrEqualVersion) {
  auto in = idle_input();
  in.available = v("0.4.0");
  EXPECT_EQ(up::next_action(in), up::action_e::wait);
  in.available = v("0.3.9");
  EXPECT_EQ(up::next_action(in), up::action_e::wait);
  in.available = v("0.4.1");
  EXPECT_EQ(up::next_action(in), up::action_e::download);
}

TEST(UpdatePolicy, WaitsWhileSessionOrAppRunning) {
  auto in = idle_input();
  in.available = v("0.4.1");
  in.installer_ready = true;
  in.sessions = 1;
  EXPECT_EQ(up::next_action(in), up::action_e::wait);
  in.user_asked_install = true;
  EXPECT_EQ(up::next_action(in), up::action_e::wait);
  in.sessions = 0;
  in.running_apps = 1;
  EXPECT_EQ(up::next_action(in), up::action_e::wait);
  in.running_apps = 0;
  EXPECT_EQ(up::next_action(in), up::action_e::install);
  in.user_asked_install = false;
  EXPECT_EQ(up::next_action(in), up::action_e::install);
}

TEST(UpdatePolicy, UserCheckIsImmediate) {
  auto in = idle_input();
  in.since_start = 5s;
  in.since_last_check = std::nullopt;
  in.user_asked_check = true;
  EXPECT_EQ(up::next_action(in), up::action_e::check);
  in.since_last_check = 1s;
  EXPECT_EQ(up::next_action(in), up::action_e::check);
}
