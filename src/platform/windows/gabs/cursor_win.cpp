/**
 * @file src/platform/windows/gabs/cursor_win.cpp
 * @brief Gabs Remote: Windows sampler of the cursor shape (`grCursor=local`).
 *
 * The standard cursors are shared handles: the one on screen is compared with `LoadCursor(nullptr, IDC_*)`.
 * A cursor that belongs to an application comes out as `other` (the app draws the arrow).
 */
// platform includes
#include <Windows.h>

// standard includes
#include <array>
#include <utility>

// local includes
#include "src/gabs/cursor.h"
#include "src/platform/windows/misc.h"

namespace gabs::cursor {

  sampler_t make_platform_sampler() {
    return []() {
      // The sampler runs on the tracker's thread; follow the input desktop (login, lock and UAC screens).
      platf::syncThreadDesktop();

      CURSORINFO info {};
      info.cbSize = sizeof(info);
      if (!GetCursorInfo(&info)) {
        return shape_e::arrow;
      }
      if (!(info.flags & CURSOR_SHOWING) || info.hCursor == nullptr) {
        return shape_e::hidden;
      }

      static const std::array<std::pair<LPCSTR, shape_e>, 13> standard {{
        {IDC_ARROW, shape_e::arrow},
        {IDC_IBEAM, shape_e::ibeam},
        {IDC_HAND, shape_e::hand},
        {IDC_WAIT, shape_e::wait},
        {IDC_APPSTARTING, shape_e::app_starting},
        {IDC_CROSS, shape_e::cross},
        {IDC_SIZENS, shape_e::size_ns},
        {IDC_SIZEWE, shape_e::size_we},
        {IDC_SIZENWSE, shape_e::size_nwse},
        {IDC_SIZENESW, shape_e::size_nesw},
        {IDC_SIZEALL, shape_e::size_all},
        {IDC_NO, shape_e::no},
        {IDC_HELP, shape_e::help},
      }};
      for (const auto &[id, shape] : standard) {
        if (info.hCursor == LoadCursorA(nullptr, id)) {
          return shape;
        }
      }
      return shape_e::other;
    };
  }

}  // namespace gabs::cursor
