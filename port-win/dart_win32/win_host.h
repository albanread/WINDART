// WINDART GUI host — the shell seam (the C++ counterpart of WINVM's
// gui/src/shell/mod.rs). Everything about the GUI that is NOT portable: a
// window, a message pump, a menu bar, a periodic tick, and the worker->UI-thread
// wakeup. Deliberately narrow — a handful of free functions.
//
// Direction of control (mirrors shell/mod.rs:26-30): the host owns the event
// loop and calls UP into the app (Dart_HandleMessages drains the UI isolate;
// WndProc forwards menu/surface routing); the natives call DOWN for effects
// (the HWND accessor, the reload request).
//
// Replaces macdart/cocoa/cocoa_host.mm. See S4_GUI_HOST_DESIGN.md §1.
#ifndef WINDART_WIN_HOST_H_
#define WINDART_WIN_HOST_H_

#include <windows.h>
#include "include/dart_api.h"

namespace dart {
namespace bin {

// ── Entry point (bin/main.cc, under DART_UI_HOST) ───────────────────────────
// Owns OS thread 0 with a GetMessageW pump after RunMainIsolate has entered the
// UI isolate and queued main(). Counterpart of macdart_run_ui_host
// (cocoa_host.mm:133). Returns 0 on clean exit.
extern "C" int windart_run_ui_host(void);

// ── UI-readiness + host-driven hot reload (cocoa_host.mm:43-61) ─────────────
// Dart calls windart_ui_ready() once the first window is built; before that a
// UI-isolate error is fatal (a running process with no window looks like a hang).
extern "C" void windart_ui_ready(void);
// Dart asks the HOST to reload the UI isolate's own sources (an isolate cannot
// rewrite the frames it stands on). Returns immediately; the reload runs at the
// pump top with no Dart frames live. Poll windart_take_ui_reload_status().
extern "C" void windart_request_ui_reload(void);
// Outcome of the last reload: "ok", "ERR: …", or NULL if none/consumed.
extern "C" const char* windart_take_ui_reload_status(void);

// ── Down-calls the natives/materializer make into the host ──────────────────
// The main workspace-window HWND, read from an atomic (may be 0 before the
// window is up — callers must tolerate that, cf. win.rs:196-199).
HWND WinHostMainHwnd();

// The shared window class name (registered by the host), so the view-server can
// create surface windows/panes whose WM_COMMAND/WM_NOTIFY route to the host WndProc.
const wchar_t* WinHostWindowClassName();

// ── Menu + toolbar command ids (shared by BuildMenuBar, the toolbar, and the
// OnMenuCommand dispatch). Host-handled ids act directly; Dart-routed ids are
// queued for the workspace to poll (Workspace_menuPoll). Kept disjoint from the
// materializer's widget control-ids (which start at 0x1000'0000). ──────────────
enum WinHostCommand {
  // File
  CMD_NEW = 130, CMD_OPEN = 131, CMD_SAVE = 132, CMD_EXIT = 104,
  // Edit
  CMD_DOIT = 140, CMD_PRINTIT = 141, CMD_CUT = 142, CMD_COPY = 143, CMD_PASTE = 144,
  // Navigation / actions (toolbar)
  CMD_HOME = 145, CMD_BACK = 146, CMD_FORWARD = 147, CMD_REFRESH = 148,
  CMD_FIND = 149, CMD_BROWSE = 150,
  // Help / host
  CMD_ABOUT = 160, CMD_RELOAD_UI = 120,
  // View: the 9 tabs (CMD_TAB_BASE + tabIndex)
  CMD_TAB_BASE = 200
};

// Toolbar band height (the top inset a pane container leaves for the toolbar), or
// 0 if no toolbar was created.
int WinHostToolbarHeight();

// The icon toolbar HWND (nullptr if none). Used by Workspace_fireCommand to
// synthesize a faithful toolbar-button WM_COMMAND (lParam == this HWND) for the
// headless self-test of the toolbar wiring.
HWND WinHostToolbarHwnd();

// The Dart-routed menu/toolbar command queue (single UI thread; no lock needed).
void windart_push_menu_command(int id);   // called by OnMenuCommand
int  windart_take_menu_command();          // Workspace_menuPoll drains it; -1 = empty

}  // namespace bin
}  // namespace dart

#endif  // WINDART_WIN_HOST_H_
