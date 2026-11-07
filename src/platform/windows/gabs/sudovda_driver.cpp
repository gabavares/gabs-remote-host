/**
 * @file src/platform/windows/gabs/sudovda_driver.cpp
 * @brief Gabs Remote: virtual display driver backed by SudoVDA (decision 0003).
 *
 * Ported in part from Apollo (src/platform/windows/virtual_display.cpp, GPL-3.0): open the device,
 * add, remove and ping. The monitor GUID is a name-based UUID of the client, so a reconnecting client
 * reuses the same monitor identity.
 */
// standard includes
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>  // used by sudovda.h

// platform includes
#include <windows.h>

// third-party includes
#include <sudovda/sudovda.h>

// local includes
#include "src/gabs/virtual_display.h"
#include "src/logging.h"
#include "src/platform/windows/utf_utils.h"

using namespace std::literals;

namespace gabs::virtual_display {
  namespace {
    GUID to_guid(const std::array<std::uint8_t, 16> &uuid) {
      // RFC 4122 byte order (big-endian fields) -> Windows GUID layout.
      GUID guid {};
      guid.Data1 = (DWORD(uuid[0]) << 24) | (DWORD(uuid[1]) << 16) | (DWORD(uuid[2]) << 8) | DWORD(uuid[3]);
      guid.Data2 = static_cast<WORD>((uuid[4] << 8) | uuid[5]);
      guid.Data3 = static_cast<WORD>((uuid[6] << 8) | uuid[7]);
      std::memcpy(guid.Data4, uuid.data() + 8, 8);
      return guid;
    }

    /**
     * @brief Serial shown by Windows for the monitor: "GR" + 11 hex digits of the client GUID.
     */
    std::string serial_for(const std::array<std::uint8_t, 16> &uuid) {
      static constexpr auto hex {"0123456789ABCDEF"};
      std::string serial {"GR"};
      for (std::size_t i = 0; serial.size() < 13; ++i) {
        serial += hex[(uuid[i / 2] >> ((i % 2) ? 0 : 4)) & 0xF];
      }
      return serial;
    }

    class sudovda_driver_t: public driver_t {
    public:
      ~sudovda_driver_t() override {
        std::lock_guard lock {mutex};
        if (handle != INVALID_HANDLE_VALUE) {
          CloseHandle(handle);
        }
      }

      bool available() override {
        std::lock_guard lock {mutex};
        return open_unlocked();
      }

      std::optional<std::string> add(const std::string &client_uid, const mode_t &mode) override {
        std::lock_guard lock {mutex};
        if (!open_unlocked()) {
          return std::nullopt;
        }

        const auto uuid {monitor_guid_for(client_uid)};
        const auto guid {to_guid(uuid)};
        // A host restarted within the watchdog timeout may still own this monitor.
        SUDOVDA::RemoveVirtualDisplay(handle, guid);

        SUDOVDA::VIRTUAL_DISPLAY_ADD_OUT output {};
        const auto serial {serial_for(uuid)};
        if (!SUDOVDA::AddVirtualDisplay(handle, mode.width, mode.height, static_cast<UINT>(mode.refresh_hz) * 1000, guid, "Gabs Remote", serial.c_str(), output)) {
          BOOST_LOG(error) << "Gabs Remote: SudoVDA failed to add a virtual display"sv;
          return std::nullopt;
        }
        known[client_uid] = guid;

        // Windows takes a moment to activate the new path.
        wchar_t device_name[CCHDEVICENAME] {};
        for (auto wait {20ms}; !SUDOVDA::GetAddedDisplayName(output, device_name); wait *= 2) {
          if (wait > 1280ms) {
            BOOST_LOG(error) << "Gabs Remote: SudoVDA added a display but Windows did not activate it in time"sv;
            SUDOVDA::RemoveVirtualDisplay(handle, guid);
            known.erase(client_uid);
            return std::nullopt;
          }
          // Keep the watchdog happy while Windows activates the display (this mutex blocks keep_alive()).
          SUDOVDA::PingDriver(handle);
          std::this_thread::sleep_for(wait);
        }
        return utf_utils::to_utf8(std::wstring {device_name});
      }

      bool remove(const std::string &client_uid) override {
        std::lock_guard lock {mutex};
        if (!open_unlocked()) {
          return false;
        }
        const auto guid {to_guid(monitor_guid_for(client_uid))};
        known.erase(client_uid);
        return SUDOVDA::RemoveVirtualDisplay(handle, guid);
      }

      void remove_all() override {
        std::lock_guard lock {mutex};
        if (!open_unlocked()) {
          return;
        }
        // Monitors of a host that died are removed by the driver watchdog; these are the ones this process made.
        for (const auto &[uid, guid] : known) {
          SUDOVDA::RemoveVirtualDisplay(handle, guid);
        }
        known.clear();
      }

      void keep_alive() override {
        std::lock_guard lock {mutex};
        if (handle != INVALID_HANDLE_VALUE) {
          SUDOVDA::PingDriver(handle);
        }
      }

    private:
      bool open_unlocked() {
        if (handle != INVALID_HANDLE_VALUE) {
          return true;
        }
        handle = SUDOVDA::OpenDevice(&SUDOVDA::SUVDA_INTERFACE_GUID);
        if (handle == INVALID_HANDLE_VALUE) {
          if (!warned) {
            BOOST_LOG(warning) << "Gabs Remote: SudoVDA virtual display driver not found (install drivers\\sudovda)"sv;
            warned = true;
          }
          return false;
        }
        if (!SUDOVDA::CheckProtocolCompatible(handle)) {
          BOOST_LOG(error) << "Gabs Remote: SudoVDA driver protocol is not compatible with this host"sv;
          CloseHandle(handle);
          handle = INVALID_HANDLE_VALUE;
          return false;
        }
        SUDOVDA::VIRTUAL_DISPLAY_GET_WATCHDOG_OUT watchdog {};
        if (SUDOVDA::GetWatchdogTimeout(handle, watchdog)) {
          BOOST_LOG(info) << "Gabs Remote: SudoVDA ready (watchdog "sv << watchdog.Timeout << " s)"sv;
        }
        return true;
      }

      std::mutex mutex;
      HANDLE handle {INVALID_HANDLE_VALUE};
      bool warned {false};
      std::map<std::string, GUID> known;
    };
  }  // namespace

  std::unique_ptr<driver_t> make_platform_driver() {
    return std::make_unique<sudovda_driver_t>();
  }
}  // namespace gabs::virtual_display
