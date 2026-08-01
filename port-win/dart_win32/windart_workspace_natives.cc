// WINDART workspace runtime natives — the live-workspace eval/reload primitives.
// Port of macdart/cocoa/workspace_natives.cc (cocoa->win): the host-callback names
// are windart_* (strong, defined in win_host.cpp and linked into dart_win32
// everywhere), so no weak-fallback trick is needed. See WORKSPACE_PLAN.md §5.
//   Workspace_eval   — Dart_EvaluateExpr: run one expression against live state (Do It).
//   Workspace_reload — Dart_WorkspaceReloadSources: hot-reload a library (S3 API).
//   Workspace_vmStats — Dart_WorkspaceVmStats: live VM counters.
#include <cstdio>
#include <string>

#include "include/dart_api.h"
#include "include/dart_tools_api.h"
#include "win_host.h"     // ui_ready / reload / the menu command queue + main HWND
#include "win_view.h"     // ViewServer / Widget (wsDragWidget splitter drag)
#include "win_canvas.h"   // SnapshotWindowFullToPng

namespace dart {
namespace bin {

// wsEval(String src) -> String. Evaluates `src` as an expression in the scope of
// the current root library, returning the result's toString(). On a compile or
// runtime error, returns an "ERR: <msg>" STRING rather than throwing.
// Dart_EvaluateExpr compiles a single expression; wrap multi-statement do-its as
// an immediately-invoked closure `(){ ... }()` on the Dart side.
void Workspace_eval(Dart_NativeArguments args) {
  Dart_Handle src = Dart_GetNativeArgument(args, 0);
  Dart_Handle lib = Dart_RootLibrary();

  Dart_Handle result = Dart_EvaluateExpr(lib, src);
  if (Dart_IsError(result)) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "ERR: %s", Dart_GetError(result));
    Dart_SetReturnValue(args, Dart_NewStringFromCString(buf));
    return;
  }
  Dart_Handle str = Dart_ToString(result);
  if (Dart_IsError(str)) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "ERR: toString: %s", Dart_GetError(str));
    Dart_SetReturnValue(args, Dart_NewStringFromCString(buf));
    return;
  }
  const char* c = NULL;
  Dart_StringToCString(str, &c);
  Dart_SetReturnValue(args, Dart_NewStringFromCString(c != NULL ? c : "null"));
}

// wsReload() -> "" | "ERR: ...". Hot-reload the isolate's sources (morphs live
// instances). Uses the S3 embedder primitive Dart_WorkspaceReloadSources.
void Workspace_reload(Dart_NativeArguments args) {
  Dart_Handle r = Dart_WorkspaceReloadSources(true /* force_reload */);
  if (Dart_IsError(r)) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "ERR: %s", Dart_GetError(r));
    Dart_SetReturnValue(args, Dart_NewStringFromCString(buf));
    return;
  }
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}

// wsCheckSyntax(String src) -> String : "" if `src` compiles as a library, else the
// compile-error message ("... error: line N pos M: ..."). Validate-before-save: the
// workspace calls this BEFORE writing a user class to the image + reloading, so the
// reload never sees source that won't compile (a bad reload's internal finalize
// PROPAGATES the error and kills the UI isolate). Loads the source into a throwaway
// `usercheck:` URI (created post-boot, so it never appears in the Browser's class
// walk) and finalizes it; Dart_LoadLibrary + Dart_FinalizeLoading are embedder APIs
// that RETURN the error (unlike the reload's internal finalize). Relies on the MSVC
// longjmp fix (runtime/vm/longjump.cc) so a scan/parse error reports, not aborts.
void Workspace_checkSyntax(Dart_NativeArguments args) {
  const char* src = NULL;
  Dart_StringToCString(Dart_GetNativeArgument(args, 0), &src);
  const char* cname = NULL;
  Dart_StringToCString(Dart_GetNativeArgument(args, 1), &cname);
  static int64_t counter = 0;
  char uri[64];
  snprintf(uri, sizeof(uri), "usercheck:%lld", static_cast<long long>(++counter));
  Dart_Handle url = Dart_NewStringFromCString(uri);
  Dart_Handle source = Dart_NewStringFromCString(src != NULL ? src : "");
  Dart_Handle lib = Dart_LoadLibrary(url, url, source, 0, 0);
  if (Dart_IsError(lib)) {
    Dart_SetReturnValue(args, Dart_NewStringFromCString(Dart_GetError(lib)));
    return;
  }
  Dart_Handle fin = Dart_FinalizeLoading(false);
  if (Dart_IsError(fin)) {
    Dart_SetReturnValue(args, Dart_NewStringFromCString(Dart_GetError(fin)));
    return;
  }
  // Force class-BODY finalization (parse fields + method signatures) via a COMPILED
  // expression that instantiates the class. Compiling `new X()` finalizes X and
  // reports the REAL syntax error ("... error: line N pos M: ...") — the same
  // message a normal load gives — whereas the embedder Dart_New masks it as
  // "could not find constructor". A bare Dart_LoadLibrary + Dart_FinalizeLoading
  // only marks the library loaded and leaves its classes to finalize lazily.
  // The compile of `new X()` FAILS before the constructor body runs, so a syntax
  // error is caught without executing user logic. (Method BODIES stay lazy; a
  // body/runtime error is a separate matter, found by running.) NOTE: the MVP
  // path needs a default (no-arg) constructor; a class without one is validated
  // via a throwaway isolate later.
  char expr[160];
  snprintf(expr, sizeof(expr), "new %s()", (cname != NULL) ? cname : "");
  Dart_Handle ev = Dart_EvaluateExpr(lib, Dart_NewStringFromCString(expr));
  if (Dart_IsError(ev)) {
    Dart_SetReturnValue(args, Dart_NewStringFromCString(Dart_GetError(ev)));
    return;
  }
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));   // compiles cleanly
}

// wsVmStats() -> List<int>. This isolate's live VM counters (S3 primitive).
void Workspace_vmStats(Dart_NativeArguments args) {
  int64_t v[kDartWorkspaceVmStatCount];
  Dart_Handle err = Dart_WorkspaceVmStats(v, kDartWorkspaceVmStatCount);
  if (Dart_IsError(err)) { Dart_SetReturnValue(args, err); return; }
  Dart_Handle list = Dart_NewList(kDartWorkspaceVmStatCount);
  if (Dart_IsError(list)) { Dart_SetReturnValue(args, list); return; }
  for (intptr_t i = 0; i < kDartWorkspaceVmStatCount; i++) {
    Dart_ListSetAt(list, i, Dart_NewInteger(v[i]));
  }
  Dart_SetReturnValue(args, list);
}

// wsRequestUiReload()/wsUiReady()/wsUiReloadStatus() -> forward to the host.
void Workspace_requestUiReload(Dart_NativeArguments args) {
  windart_request_ui_reload();
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}
void Workspace_uiReady(Dart_NativeArguments args) {
  windart_ui_ready();
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}
void Workspace_uiReloadStatus(Dart_NativeArguments args) {
  const char* s = windart_take_ui_reload_status();
  Dart_SetReturnValue(args, Dart_NewStringFromCString(s != NULL ? s : ""));
}

// wsMenuPoll() -> int : the next queued menu/toolbar command id, or -1 if none.
// The workspace polls this and maps ids -> actions (tab switch, Do It, image ops).
void Workspace_menuPoll(Dart_NativeArguments args) {
  Dart_SetReturnValue(args, Dart_NewInteger(windart_take_menu_command()));
}

// wsFireCommand(int id) -> "" : synthesize a host command exactly as the icon
// TOOLBAR does — WM_COMMAND to the main window with LOWORD(wParam)==id and
// lParam == the toolbar HWND (non-zero). Drives the real WndProc routing branch
// (win_host.cpp), so the headless self-test proves the toolbar path end-to-end
// (WndProc -> OnMenuCommand -> menu queue), not just the Dart dispatch. For a
// Dart-routed id this only enqueues (no re-entrant Dart), so it is safe to call
// synchronously from inside a native.
void Workspace_fireCommand(Dart_NativeArguments args) {
  int64_t id = 0;
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 0), &id);
  HWND main = WinHostMainHwnd();
  if (main != NULL) {
    SendMessageW(main, WM_COMMAND, MAKEWPARAM((WORD)id, 0),
                 reinterpret_cast<LPARAM>(WinHostToolbarHwnd()));
  }
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}

// wsResizeWindow(int w, int h) -> "" : resize the MAIN window to w x h. Drives the
// real WM_SIZE path (WndProc moves the toolbar + pane container -> the container's
// WM_SIZE -> OnSurfaceResize -> kind-7 dispatch -> the app's onResize), so the
// headless self-test proves resize reflow end-to-end. SetWindowPos sends WM_SIZE
// synchronously; OnSurfaceResize's WinViewInApply guard keeps that safe.
void Workspace_resizeWindow(Dart_NativeArguments args) {
  int64_t w = 0, h = 0;
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 0), &w);
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 1), &h);
  HWND main = WinHostMainHwnd();
  if (main != NULL) {
    SetWindowPos(main, NULL, 0, 0, (int)w, (int)h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}

// wsDragWidget(int ticket, int dx, int dy) -> "" : synthesize a mouse drag on the
// widget with this ticket (down at (3,3), move by (dx,dy), up) — the headless
// proof for the draggable splitter. Uses the SAME WM_LBUTTONDOWN/WM_MOUSEMOVE/
// WM_LBUTTONUP the OS sends, with lParam-carried client coords the SplitterProc
// reads, so this drives the real drag path. Pure C++ (MoveWindow), no Dart.
void Workspace_dragWidget(Dart_NativeArguments args) {
  int64_t ticket = 0, dx = 0, dy = 0;
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 0), &ticket);
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 1), &dx);
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 2), &dy);
  Widget* w = ViewServer::Instance().WidgetByTicket(ticket);
  if (w != NULL && w->hwnd != NULL) {
    const int gx = 3, gy = 3;
    LPARAM downLp = MAKELPARAM(gx, gy);
    LPARAM moveLp = MAKELPARAM(gx + (int)dx, gy + (int)dy);
    SendMessageW(w->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, downLp);
    SendMessageW(w->hwnd, WM_MOUSEMOVE, MK_LBUTTON, moveLp);
    SendMessageW(w->hwnd, WM_LBUTTONUP, 0, moveLp);
  }
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}

// wsSnapshotFull(path) -> "" | error : PNG of the WHOLE main window (frame + menu
// + toolbar + client) — the polish overview capture.
void Workspace_snapshotFull(Dart_NativeArguments args) {
  const char* path = NULL;
  Dart_StringToCString(Dart_GetNativeArgument(args, 0), &path);
  int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
  std::wstring wpath(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, path, -1, &wpath[0], n);
  const char* err = SnapshotWindowFullToPng(WinHostMainHwnd(), wpath.c_str());
  Dart_SetReturnValue(args, Dart_NewStringFromCString(err != NULL ? err : ""));
}

// W1 (snapshot-as-blob): bake the on-disk snapshot .bin into the image `blobs`
// table (windart_snapshot_blob.cc, host layer). Driven from Dart as wsBakeSnapshot()
// — the seed of an in-app "Recreate Snapshot". extern "C": one unmangled symbol.
extern "C" int windart_bake_snapshots_to_image(char* msg, int msgSz);

// wsBakeSnapshot() -> String status. Writes the current on-disk snapshot into the
// SQLite image so the next boot can load the snapshot from the DB blob.
void Workspace_bakeSnapshot(Dart_NativeArguments args) {
  char msg[256];
  windart_bake_snapshots_to_image(msg, static_cast<int>(sizeof(msg)));
  Dart_SetReturnValue(args, Dart_NewStringFromCString(msg));
}

// W2 (export/import-world): the git bridge, in windart_world_io.cc (host layer,
// binary-safe — the Dart sqlite binding is text-only). extern "C": unmangled.
extern "C" int windart_export_world(const char* outDir, char* msg, int msgSz);
extern "C" int windart_import_world(const char* inDir, const char* outDbPath,
                                    char* msg, int msgSz);

// wsExportWorld(String dir) -> String status. Projects the SQLite image to loose,
// diffable files under `dir` (src/*.dart + blobs/*.bin + manifest) for git.
void Workspace_exportWorld(Dart_NativeArguments args) {
  const char* dir = NULL;
  Dart_StringToCString(Dart_GetNativeArgument(args, 0), &dir);
  char msg[512];
  windart_export_world(dir != NULL ? dir : "", msg, static_cast<int>(sizeof(msg)));
  Dart_SetReturnValue(args, Dart_NewStringFromCString(msg));
}

// wsImportWorld(String inDir, String outDb) -> String status. Builds a fresh world
// SQLite at `outDb` from the loose files under `inDir` (do NOT target the live image).
void Workspace_importWorld(Dart_NativeArguments args) {
  const char* inDir = NULL;
  const char* outDb = NULL;
  Dart_StringToCString(Dart_GetNativeArgument(args, 0), &inDir);
  Dart_StringToCString(Dart_GetNativeArgument(args, 1), &outDb);
  char msg[512];
  windart_import_world(inDir != NULL ? inDir : "", outDb != NULL ? outDb : "", msg,
                       static_cast<int>(sizeof(msg)));
  Dart_SetReturnValue(args, Dart_NewStringFromCString(msg));
}

}  // namespace bin
}  // namespace dart
