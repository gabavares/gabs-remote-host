/**
 * @file src/platform/windows/gabs/update_launch_win.cpp
 * @brief Gabs Remote: runs the downloaded installer outside the service job object (decision 0006).
 *
 * sunshinesvc puts the host in a job object that kills its children when the service stops. The
 * installer stops the service, so it must leave the job (the job allows it: JOB_OBJECT_LIMIT_BREAKAWAY_OK).
 */
// standard includes
#include <string>

// platform includes
#include <windows.h>

// local includes
#include "src/gabs/update_service.h"
#include "src/logging.h"

using namespace std::literals;

namespace gabs::update {

  bool launch_installer(const std::filesystem::path &exe) {
    std::wstring command = L"\"" + exe.wstring() + L"\" /S /ATUALIZACAO";
    STARTUPINFOW si {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi {};
    auto flags = CREATE_BREAKAWAY_FROM_JOB | DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    auto workdir = exe.parent_path().wstring();
    BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr, workdir.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
      // Not in a job that allows breakaway (e.g. host run from a console): start it normally.
      flags &= ~CREATE_BREAKAWAY_FROM_JOB;
      command = L"\"" + exe.wstring() + L"\" /S /ATUALIZACAO";
      ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr, workdir.c_str(), &si, &pi);
    }
    if (!ok) {
      BOOST_LOG(error) << "Gabs Remote: could not start the installer (error "sv << GetLastError() << ')';
      return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
  }

}  // namespace gabs::update
