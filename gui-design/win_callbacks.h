// WINDART dart:win — reverse callbacks + key-state. Declares the routing the
// host WNDPROC (win_host.cpp) calls into, plus the menu dispatch. Implemented in
// win_callbacks.cpp. See S4_GUI_HOST_DESIGN.md §3.
#ifndef WINDART_WIN_CALLBACKS_H_
#define WINDART_WIN_CALLBACKS_H_

#include <windows.h>
#include "include/dart_api.h"

namespace dart {
namespace bin {

// Down-call into the host for the main HWND (defined in win_host.cpp).
HWND WinHostMainHwnd();

// ── Up-calls the host WndProc makes (the crate::on_* seam, win.rs:235-273) ──
void    OnMenuCommand(WORD id);                            // WM_COMMAND from a menu
void    OnSurfaceCommand(HWND host, WORD id, WORD code, HWND ctrl);  // control WM_COMMAND
LRESULT OnSurfaceNotify(HWND host, NMHDR* hdr);            // WM_NOTIFY (list view)
void    OnSurfaceResize(HWND host, int w, int h);          // WM_SIZE (kind 7, debounced)
void    OnSurfaceClose(HWND host);                         // WM_CLOSE (kind 8)

}  // namespace bin
}  // namespace dart

#endif  // WINDART_WIN_CALLBACKS_H_
