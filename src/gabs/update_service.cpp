/**
 * @file src/gabs/update_service.cpp
 * @brief Gabs Remote: the host updater (decision 0006).
 */
// header include
#include "update_service.h"

// standard includes
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <regex>
#include <thread>

// lib includes
#include <curl/curl.h>

// local includes
#include "update_key.h"
#include "update_policy.h"
#include "src/logging.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "src/system_tray.h"

using namespace std::literals;

namespace gabs::update {

  namespace {
    constexpr auto manifest_url = "https://github.com/gabavares/gabs-remote/releases/latest/download/manifest.json";
    constexpr auto signature_url = "https://github.com/gabavares/gabs-remote/releases/latest/download/manifest.json.sig";
    constexpr auto release_base = "https://github.com/gabavares/gabs-remote/releases/download/v";
    constexpr auto tick = 60s;
    constexpr long manifest_timeout_s = 30;
    constexpr long installer_timeout_s = 30 * 60;
    constexpr std::size_t max_manifest_bytes = 1024 * 1024;

    struct shared_t {
      std::mutex mutex;
      std::condition_variable_any cv;
      state_e state = state_e::idle;
      std::string error;
      std::optional<manifest_t> manifest;  ///< last verified manifest
      bool installer_ready = false;
      std::filesystem::path installer;
      bool user_check = false;
      bool user_install = false;
      std::optional<version_t> installed;
    };

    shared_t &shared() {
      static shared_t s;
      return s;
    }

    enum class fetch_e {
      ok,
      not_found,
      failed,
      too_big,
    };

    struct sink_t {
      std::string *memory = nullptr;
      std::FILE *file = nullptr;
      std::uint64_t written = 0;
      std::uint64_t limit = 0;
      bool too_big = false;
    };

    std::size_t write_cb(char *data, std::size_t size, std::size_t n, void *user) {
      auto *sink = static_cast<sink_t *>(user);
      const auto bytes = size * n;
      if (sink->written + bytes > sink->limit) {
        sink->too_big = true;
        return 0;  // aborts the transfer
      }
      sink->written += bytes;
      if (sink->memory) {
        sink->memory->append(data, bytes);
        return bytes;
      }
      return std::fwrite(data, 1, bytes, sink->file);
    }

    fetch_e fetch(const std::string &url, sink_t &sink, long timeout_s) {
      CURL *curl = curl_easy_init();
      if (!curl) {
        return fetch_e::failed;
      }
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
      // Only HTTPS, also after redirects (GitHub sends the assets to another HTTPS host).
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "GabsRemote-Updater/" PROJECT_VERSION);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);

      auto result = curl_easy_perform(curl);
      long status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      curl_easy_cleanup(curl);

      if (sink.too_big) {
        return fetch_e::too_big;
      }
      if (result != CURLE_OK) {
        if (status == 404) {
          return fetch_e::not_found;
        }
        BOOST_LOG(warning) << "Gabs Remote: update download failed: "sv << curl_easy_strerror(result);
        return fetch_e::failed;
      }
      if (status == 404) {
        return fetch_e::not_found;
      }
      return status == 200 ? fetch_e::ok : fetch_e::failed;
    }

    std::string public_key_raw() {
      static const std::string key = decode_base64(kPublicKeyBase64).value_or(std::string {});
      return key;
    }

    void set_state(state_e state, std::string error = {}) {
      auto &s = shared();
      std::scoped_lock lock {s.mutex};
      s.state = state;
      s.error = std::move(error);
    }

    /**
     * @return false when the check failed (network, missing Release or invalid manifest).
     */
    bool do_check(const version_t &installed) {
      set_state(state_e::checking);
      std::string json;
      std::string sig;
      sink_t a {&json, nullptr, 0, max_manifest_bytes};
      sink_t b {&sig, nullptr, 0, 1024};
      auto ra = fetch(manifest_url, a, manifest_timeout_s);
      auto rb = ra == fetch_e::ok ? fetch(signature_url, b, manifest_timeout_s) : ra;
      if (ra == fetch_e::not_found || rb == fetch_e::not_found) {
        BOOST_LOG(info) << "Gabs Remote: no Release published yet (HTTP 404)"sv;
        set_state(state_e::error, "Nenhuma versão publicada ainda.");
        return false;
      }
      if (ra != fetch_e::ok || rb != fetch_e::ok) {
        set_state(state_e::error, "Sem conexão com o GitHub. Tento de novo em 1 hora.");
        return false;
      }
      auto manifest = load_verified(json, sig, public_key_raw());
      if (!manifest) {
        BOOST_LOG(error) << "Gabs Remote: update manifest rejected (signature or fields)"sv;
        set_state(state_e::error, "Atualização inválida: o manifesto não confere com a assinatura.");
        return false;
      }

      auto &s = shared();
      std::scoped_lock lock {s.mutex};
      if (s.manifest && s.manifest->version != manifest->version) {
        s.installer_ready = false;
        s.installer.clear();
      }
      s.manifest = manifest;
      if (manifest->version > installed) {
        BOOST_LOG(info) << "Gabs Remote: version "sv << to_string(manifest->version) << " available"sv;
        s.state = s.installer_ready ? state_e::ready : state_e::downloading;
      } else {
        BOOST_LOG(info) << "Gabs Remote: up to date ("sv << to_string(installed) << ')';
        s.state = state_e::up_to_date;
      }
      s.error.clear();
      return true;
    }

    bool do_download(const manifest_t &manifest) {
      auto dir = updates_dir();
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      auto final_path = dir / manifest.host.file;

      // Already downloaded before a restart?
      if (std::filesystem::exists(final_path, ec) && sha256_file(final_path) == manifest.host.sha256) {
        auto &s = shared();
        std::scoped_lock lock {s.mutex};
        s.installer_ready = true;
        s.installer = final_path;
        s.state = state_e::ready;
        return true;
      }
      std::filesystem::remove(final_path, ec);

      set_state(state_e::downloading);
      auto part = dir / (manifest.host.file + ".part");
      auto *file = _wfopen(part.c_str(), L"wb");
      if (!file) {
        set_state(state_e::error, "Não consegui gravar em config\\updates.");
        return false;
      }
      sink_t sink {nullptr, file, 0, manifest.host.size};
      auto url = std::string {release_base} + to_string(manifest.version) + "/" + manifest.host.file;
      BOOST_LOG(info) << "Gabs Remote: downloading "sv << url;
      auto result = fetch(url, sink, installer_timeout_s);
      std::fclose(file);
      if (result != fetch_e::ok || !finish_download(part, manifest.host)) {
        std::filesystem::remove(part, ec);
        BOOST_LOG(error) << "Gabs Remote: installer download failed or did not match the manifest"sv;
        set_state(state_e::error, "Falha ao baixar a atualização (ou o arquivo não confere). Tento de novo em 1 hora.");
        return false;
      }

      auto &s = shared();
      {
        std::scoped_lock lock {s.mutex};
        s.installer_ready = true;
        s.installer = final_path;
        s.state = state_e::ready;
      }
      BOOST_LOG(info) << "Gabs Remote: version "sv << to_string(manifest.version) << " ready; installs when there is no session"sv;
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_update_ready(to_string(manifest.version));
#endif
      return true;
    }

    void do_install(const manifest_t &manifest, const std::filesystem::path &installer) {
      // Checked again right before running: the file could have changed since the download.
      if (sha256_file(installer) != manifest.host.sha256) {
        std::error_code ec;
        std::filesystem::remove(installer, ec);
        auto &s = shared();
        std::scoped_lock lock {s.mutex};
        s.installer_ready = false;
        s.installer.clear();
        s.state = state_e::error;
        s.error = "O instalador baixado mudou depois da conferência; baixo de novo.";
        BOOST_LOG(error) << "Gabs Remote: installer changed after the check; discarded"sv;
        return;
      }
      set_state(state_e::installing);
      BOOST_LOG(info) << "Gabs Remote: installing "sv << to_string(manifest.version) << " (the service restarts)"sv;
      if (!launch_installer(installer)) {
        set_state(state_e::error, "Não consegui iniciar o instalador.");
      }
    }

    void run(std::stop_token stop, version_t installed) {
      platf::set_thread_name("gabs::update");
      clean_updates_dir(updates_dir(), installed);

      const auto started = std::chrono::steady_clock::now();
      std::optional<std::chrono::steady_clock::time_point> last_check;
      bool last_failed = false;
      bool launched = false;

      auto &s = shared();
      while (!stop.stop_requested()) {
        policy_input_t in {};
        std::optional<manifest_t> manifest;
        std::filesystem::path installer;
        {
          std::scoped_lock lock {s.mutex};
          auto now = std::chrono::steady_clock::now();
          in.since_start = std::chrono::duration_cast<std::chrono::seconds>(now - started);
          if (last_check) {
            in.since_last_check = std::chrono::duration_cast<std::chrono::seconds>(now - *last_check);
          }
          in.last_check_failed = last_failed;
          // After a failure the policy waits for the next check (1 hour); a ready installer still installs.
          if (s.manifest) {
            in.available = s.manifest->version;
          }
          in.installer_ready = s.installer_ready && !launched;
          in.installed = installed;
          in.user_asked_check = s.user_check;
          in.user_asked_install = s.user_install;
          s.user_check = false;
          s.user_install = false;
          manifest = s.manifest;
          installer = s.installer;
        }
        in.sessions = rtsp_stream::session_count();
        in.running_apps = proc::proc.running() > 0 ? 1 : 0;

        switch (next_action(in)) {
          case action_e::check:
            last_failed = !do_check(installed);
            last_check = std::chrono::steady_clock::now();
            break;
          case action_e::download:
            if (manifest && !do_download(*manifest)) {
              last_failed = true;
              last_check = std::chrono::steady_clock::now();
            }
            break;
          case action_e::install:
            if (manifest) {
              do_install(*manifest, installer);
              std::scoped_lock lock {s.mutex};
              launched = s.state == state_e::installing;
              if (!launched) {
                // Hash changed (e.g. antivirus) or the installer did not start: no new download for 1 hour.
                last_failed = true;
                last_check = std::chrono::steady_clock::now();
              }
            }
            break;
          case action_e::wait:
            break;
        }

        std::unique_lock lock {s.mutex};
        s.cv.wait_for(lock, stop, tick, [&s] {
          return s.user_check || s.user_install;
        });
      }
    }

    class deinit_t: public platf::deinit_t {
    public:
      explicit deinit_t(version_t installed):
          thread {run, installed} {
      }

      ~deinit_t() override {
        thread.request_stop();
        shared().cv.notify_all();
      }

    private:
      std::jthread thread;
    };
  }  // namespace

  const char *to_string(state_e state) {
    switch (state) {
      case state_e::idle:
        return "idle";
      case state_e::checking:
        return "checking";
      case state_e::up_to_date:
        return "up_to_date";
      case state_e::downloading:
        return "downloading";
      case state_e::ready:
        return "ready";
      case state_e::installing:
        return "installing";
      case state_e::error:
        return "error";
    }
    return "idle";
  }

  status_t status() {
    auto &s = shared();
    std::scoped_lock lock {s.mutex};
    status_t out;
    out.installed = PROJECT_VERSION;
    if (s.manifest) {
      out.latest = to_string(s.manifest->version);
      out.notes = s.manifest->notes;
    }
    out.state = s.state;
    out.error = s.error;
    return out;
  }

  void request_check() {
    auto &s = shared();
    {
      std::scoped_lock lock {s.mutex};
      s.user_check = true;
    }
    s.cv.notify_all();
  }

  bool request_install() {
    auto &s = shared();
    {
      std::scoped_lock lock {s.mutex};
      if (!s.installer_ready || rtsp_stream::session_count() > 0 || proc::proc.running() > 0) {
        return false;
      }
      s.user_install = true;
    }
    s.cv.notify_all();
    return true;
  }

  std::unique_ptr<platf::deinit_t> start() {
    auto installed = parse_version(PROJECT_VERSION);
    if (!installed) {
      BOOST_LOG(warning) << "Gabs Remote: updater off (version "sv << PROJECT_VERSION << " is not x.y.z)"sv;
      return nullptr;
    }
    BOOST_LOG(info) << "Gabs Remote: updater on (installed "sv << PROJECT_VERSION << ')';
    return std::make_unique<deinit_t>(*installed);
  }

  std::filesystem::path updates_dir() {
    return platf::appdata() / "updates";
  }

  bool finish_download(const std::filesystem::path &part, const artifact_t &expected) {
    std::error_code ec;
    auto size = std::filesystem::file_size(part, ec);
    if (ec || size != expected.size || sha256_file(part) != expected.sha256) {
      std::filesystem::remove(part, ec);
      return false;
    }
    auto final_path = part.parent_path() / expected.file;
    std::filesystem::rename(part, final_path, ec);
    if (ec) {
      std::filesystem::remove(part, ec);
      return false;
    }
    return true;
  }

  std::optional<version_t> installer_version(const std::string &file_name) {
    static const std::regex pattern {R"(^GabsRemote-Setup-(\d{1,3}\.\d{1,3}\.\d{1,3})\.exe$)"};
    std::smatch m;
    if (!std::regex_match(file_name, m, pattern)) {
      return std::nullopt;
    }
    return parse_version(m[1].str());
  }

  void clean_updates_dir(const std::filesystem::path &dir, const version_t &installed) {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      auto name = entry.path().filename().string();
      auto version = installer_version(name);
      bool partial = entry.path().extension() == ".part";
      if (partial || (version && *version <= installed)) {
        std::filesystem::remove(entry.path(), ec);
      }
    }
  }

}  // namespace gabs::update
