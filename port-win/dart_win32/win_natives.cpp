// WINDART dart:win natives — the resolver table, the wire contract, and the
// view-server op catalog. Replaces macdart/cocoa/cocoa_natives.mm.
//
// KEEP the shape (WINDOWS_PORTING_PLAN.md §5): the X-macro table
// (WIN_NATIVE_LIST, cf. COCOA_NATIVE_LIST cocoa_natives.mm:516) and the
// name+argc -> Dart_NativeFunction resolver (cf. CocoaNativeLookup :568). Only
// the entries and bodies change.
//
// DELETE the dynamic send (Cocoa_send, cocoa_natives.mm:195-306) and its
// @encode/AAPCS64 classifier + objc_msgSend shim: there is no "call any selector"
// bridge on Windows. In its place is a FIXED catalog of concrete ops — and
// because the IDE chrome is a view-server too, that catalog is the widget/canvas
// protocol (S4_GUI_HOST_DESIGN.md §2.2). Bodies are skeletons; S4/S5/S6 fill them.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <richedit.h>    // CHARFORMAT2 / EM_SETCHARFORMAT (editor spans, S7.2)
#include <cstdint>
#include <string>
#include <vector>

#include "include/dart_api.h"
#include "win_natives.h"
#include "win_view.h"
#include "win_callbacks.h"
#include "win_canvas.h"
#include "win_host.h"    // WinHostMainHwnd (host quit)

namespace dart {
namespace bin {

// ── Wire contract helpers (cf. cocoa_natives.mm:35-79) ──────────────────────
static int64_t IntArg(Dart_NativeArguments args, int i) {   // cocoa_natives.mm:35
  int64_t v = 0;
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, i), &v);
  return v;
}

static double DoubleFromDart(Dart_Handle h) {               // cocoa_natives.mm:75
  if (Dart_IsDouble(h)) { double d = 0; Dart_DoubleValue(h, &d); return d; }
  if (Dart_IsInteger(h)) { int64_t v = 0; Dart_IntegerToInt64(h, &v); return (double)v; }
  return 0.0;
}

// THE one systematic add over Cocoa (which used UTF-8 NSStrings directly):
// widen at the boundary for the …W Win32 APIs. UTF-8 (Dart_StringToCString) ->
// UTF-16.
static std::wstring Utf16FromDart(Dart_Handle h) {
  const char* c = nullptr;
  if (Dart_IsError(Dart_StringToCString(h, &c)) || c == nullptr) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, c, -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, c, -1, &w[0], n);
  return w;
}

// Wrap a UTF-16 string back to a Dart String.
static Dart_Handle DartStringFromUtf16(const std::wstring& w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  return Dart_NewStringFromCString(s.c_str());
}

// Decode a List<num> frame [x,y,w,h] — TOP-LEFT, NO FLIP (Win32 child coords are
// already top-left; Cocoa needed a height-y flip, APP_PANE_PLAN.md §6). A genuine
// simplification.
static RECT RectArg(Dart_Handle list) {
  RECT r = {0, 0, 0, 0};
  intptr_t n = 0; Dart_ListLength(list, &n);
  auto get = [&](int i) -> LONG {
    if (i >= n) return 0;
    return (LONG)DoubleFromDart(Dart_ListGetAt(list, i));
  };
  LONG x = get(0), y = get(1), w = get(2), h = get(3);
  r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
  return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// The op catalog. Each is a concrete native (name+argc). Skeletons only.
// ─────────────────────────────────────────────────────────────────────────────

// ── Surface + materializer (the view-server core) ───────────────────────────

// _surfaceOpenPane(int w, int h) -> int surfaceHandle
static void Win_surfaceOpenPane(Dart_NativeArguments args) {
  int w = (int)IntArg(args, 0), h = (int)IntArg(args, 1);
  Surface* s = ViewServer::Instance().OpenPane(w, h);
  Dart_SetReturnValue(args, Dart_NewInteger(s ? s->ticket : 0));
}

// _surfaceOpenWindow(String title, int w, int h) -> int surfaceHandle
static void Win_surfaceOpenWindow(Dart_NativeArguments args) {
  std::wstring title = Utf16FromDart(Dart_GetNativeArgument(args, 0));
  int w = (int)IntArg(args, 1), h = (int)IntArg(args, 2);
  Surface* s = ViewServer::Instance().OpenWindow(title, w, h);
  Dart_SetReturnValue(args, Dart_NewInteger(s ? s->ticket : 0));
}

// _surfaceClose(int surface)
static void Win_surfaceClose(Dart_NativeArguments args) {
  ViewServer::Instance().Close(IntArg(args, 0));
}

// _surfaceApply(int surface, int gen, List cmds) -> String ("" or first error).
// THE batched materialize — the Windows analog of gpApply: a whole build's
// command list in one call, applied atomically. cmds is the
// ['clear'|'add'|'set'|'remove'|'place'|'title'|'focus'] vocabulary
// (S4_GUI_HOST_DESIGN.md §4.2). Most S4 work lives in ViewServer::Apply.
static void Win_surfaceApply(Dart_NativeArguments args) {
  int64_t surface = IntArg(args, 0);
  int64_t gen = IntArg(args, 1);
  Dart_Handle cmds = Dart_GetNativeArgument(args, 2);
  std::string err = ViewServer::Instance().Apply(surface, gen, &cmds);
  Dart_SetReturnValue(args, Dart_NewStringFromCString(err.c_str()));
}

// _surfaceSize(int surface) -> [w, h]
static void Win_surfaceSize(Dart_NativeArguments args) {
  Surface* s = /* ViewServer lookup */ nullptr;
  (void)s;
  RECT rc = {0}; /* GetClientRect(s->host, &rc); */
  Dart_Handle l = Dart_NewList(2);
  Dart_ListSetAt(l, 0, Dart_NewInteger(rc.right - rc.left));
  Dart_ListSetAt(l, 1, Dart_NewInteger(rc.bottom - rc.top));
  Dart_SetReturnValue(args, l);
}

// ── Read-back / measurement (need a synchronous return -> not a push cmd) ────

// _widgetText(int ticket) -> String. The field/editor's current text; the Dart
// 'text'/'enter' handler pulls it (mirrors Cocoa's onTextChange handler reading
// sender.stringValue).
static void Win_widgetText(Dart_NativeArguments args) {
  Widget* wdg = ViewServer::Instance().WidgetByTicket(IntArg(args, 0));
  if (!wdg || !wdg->hwnd) { Dart_SetReturnValue(args, Dart_NewStringFromCString("")); return; }
  int len = GetWindowTextLengthW(wdg->hwnd);
  std::wstring buf(len + 1, L'\0');
  GetWindowTextW(wdg->hwnd, &buf[0], len + 1);
  buf.resize(len);
  Dart_SetReturnValue(args, DartStringFromUtf16(buf));
}

// _editorSelection(int ticket) -> [start, len, text] — the Do-It-on-selection
// pull op (S7 catalog gap 1). Works on EDIT and RichEdit via EM_GETSEL.
static void Win_editorSelection(Dart_NativeArguments args) {
  Dart_Handle out = Dart_NewList(3);
  Widget* wdg = ViewServer::Instance().WidgetByTicket(IntArg(args, 0));
  DWORD start = 0, end = 0;
  std::wstring sel;
  if (wdg && wdg->hwnd) {
    SendMessageW(wdg->hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                 reinterpret_cast<LPARAM>(&end));
    int len = GetWindowTextLengthW(wdg->hwnd);
    std::wstring buf(len + 1, L'\0');
    GetWindowTextW(wdg->hwnd, &buf[0], len + 1);
    buf.resize(len);
    if (end > start && (int)end <= len) {
      sel = buf.substr(start, end - start);           // an explicit selection
    } else {
      start = 0; end = (DWORD)len; sel = buf;          // none -> the whole buffer (Do-It-all)
    }
  }
  Dart_ListSetAt(out, 0, Dart_NewInteger(start));
  Dart_ListSetAt(out, 1, Dart_NewInteger((int)end - (int)start));
  Dart_ListSetAt(out, 2, DartStringFromUtf16(sel));
  Dart_SetReturnValue(args, out);
}

// _measureText(String s, String fontSpec) -> [w, h]  (DirectWrite metrics; S5).
static void Win_measureText(Dart_NativeArguments args) {
  // DWrite: CreateTextLayout -> GetMetrics. Skeleton.
  Dart_Handle l = Dart_NewList(2);
  Dart_ListSetAt(l, 0, Dart_NewInteger(0));
  Dart_ListSetAt(l, 1, Dart_NewInteger(0));
  Dart_SetReturnValue(args, l);
}

// ── Dialogs (Win32 modal — each spins a nested loop; the OnWake re-entrancy
// guard in win_host.cpp is what keeps that safe, S4_GUI_HOST_DESIGN.md §0) ────

// _openFileDialog(String optsJson) -> String path ("" if cancelled). IFileOpenDialog.
static void Win_openFileDialog(Dart_NativeArguments args) {
  // CoCreateInstance(CLSID_FileOpenDialog) -> SetFileTypes -> Show(hwnd) ->
  // GetResult -> GetDisplayName(SIGDN_FILESYSPATH). Skeleton.
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}
// _saveFileDialog(String optsJson) -> String path. IFileSaveDialog.
static void Win_saveFileDialog(Dart_NativeArguments args) {
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}
// _messageBox(String title, String body, int kind) -> int (button pressed).
static void Win_messageBox(Dart_NativeArguments args) {
  std::wstring title = Utf16FromDart(Dart_GetNativeArgument(args, 0));
  std::wstring body = Utf16FromDart(Dart_GetNativeArgument(args, 1));
  int rc = MessageBoxW(WinHostMainHwnd(), body.c_str(), title.c_str(),
                       (UINT)IntArg(args, 2));
  Dart_SetReturnValue(args, Dart_NewInteger(rc));
}

// ── Editor (the hard widget, S4_GUI_HOST_DESIGN.md §4.7) ─────────────────────
// _editorApplySpans(int ticket, List<int> runs) — flat [start,len,kind,…]. The
// direct port of Cocoa_applySpans (cocoa_callbacks.mm:265-286). RichEdit
// bring-up: EM_EXSETSEL + EM_SETCHARFORMAT per run. Direct2D form: a DWrite color
// span table. Offsets are UTF-16 units (Dart string indices), which is what both
// EM_EXSETSEL and DWrite ranges want (as NSRange did for Cocoa).
// kind -> syntax colour (cocoa_callbacks.mm:253-262 equivalent):
// 1 keyword, 2 string, 3 comment, 4 number, 5 type, else default.
static COLORREF ColorForSpanKind(int64_t k) {
  switch (k) {
    case 1: return RGB(0, 0, 205);      // keyword  - blue
    case 2: return RGB(163, 21, 21);    // string   - dark red
    case 3: return RGB(0, 128, 0);      // comment  - green
    case 4: return RGB(128, 0, 128);    // number   - purple
    case 5: return RGB(38, 127, 153);   // type     - teal
    default: return RGB(30, 30, 30);    // default  - near-black
  }
}

static void Win_editorApplySpans(Dart_NativeArguments args) {
  Widget* wdg = ViewServer::Instance().WidgetByTicket(IntArg(args, 0));
  if (!wdg || !wdg->hwnd) return;
  HWND ed = wdg->hwnd;
  Dart_Handle runs = Dart_GetNativeArgument(args, 1);
  intptr_t n = 0;
  if (Dart_IsError(Dart_ListLength(runs, &n))) return;

  // Preserve the caret/selection; suppress repaint while we recolour.
  CHARRANGE saved;
  SendMessageW(ed, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&saved));
  SendMessageW(ed, WM_SETREDRAW, FALSE, 0);

  for (intptr_t i = 0; i + 2 < n; i += 3) {
    int64_t start = 0, len = 0, kind = 0;
    Dart_IntegerToInt64(Dart_ListGetAt(runs, i), &start);
    Dart_IntegerToInt64(Dart_ListGetAt(runs, i + 1), &len);
    Dart_IntegerToInt64(Dart_ListGetAt(runs, i + 2), &kind);
    CHARRANGE cr;
    cr.cpMin = (LONG)start;
    cr.cpMax = (LONG)(start + len);           // UTF-16 offsets == Dart string indices
    SendMessageW(ed, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    CHARFORMAT2W cf;
    ZeroMemory(&cf, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = ColorForSpanKind(kind);
    SendMessageW(ed, EM_SETCHARFORMAT, SCF_SELECTION,
                 reinterpret_cast<LPARAM>(&cf));
  }

  SendMessageW(ed, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&saved));
  SendMessageW(ed, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(ed, nullptr, TRUE);
}

// ── Canvas (S5) — the Direct2D demo renderer ─────────────────────────────────
// _canvasDraw(int ticket, List cmds) — replay the demo draw-command protocol
// (clear/rect/oval/line/text/blit) against the canvas' D2D target, atomically.
static void Win_canvasDraw(Dart_NativeArguments args) {
  CanvasDraw(IntArg(args, 0), Dart_GetNativeArgument(args, 1));
}
// _canvasSnap(int ticket, String path) -> String ("" or error) — WIC PNG readback.
static void Win_canvasSnap(Dart_NativeArguments args) {
  std::wstring path = Utf16FromDart(Dart_GetNativeArgument(args, 1));
  const char* err = CanvasSnapPng(IntArg(args, 0), path.c_str());
  Dart_SetReturnValue(args, Dart_NewStringFromCString(err ? err : ""));
}
// _surfaceSnapshot(int surface, String path) -> String — the UNIFIED PNG readback
// (S6 §4): snapshot a surface's canvas (and, later, its game pane) via one native.
// The S7 rehost regression loop calls this per surface.
static void Win_surfaceSnapshot(Dart_NativeArguments args) {
  int64_t surface = IntArg(args, 0);
  std::wstring path = Utf16FromDart(Dart_GetNativeArgument(args, 1));
  int64_t canvas = ViewServer::Instance().CanvasTicketInSurface(surface);
  const char* err;
  if (canvas != 0) {
    err = CanvasSnapPng(canvas, path.c_str());          // Direct2D canvas (demos/games)
  } else {
    HWND host = ViewServer::Instance().SurfaceHost(surface);
    if (host == nullptr) { err = "surface not found"; }
    else { err = SnapshotHwndToPng(host, path.c_str()); }   // native-control window (workspace)
  }
  Dart_SetReturnValue(args, Dart_NewStringFromCString(err ? err : ""));
}
// _hostQuit() — post WM_CLOSE to the main window (clean exit for headless runs).
static void Win_hostQuit(Dart_NativeArguments /*args*/) {
  HWND h = WinHostMainHwnd();
  if (h) PostMessageW(h, WM_CLOSE, 0, 0);
}

// _canvasBlit(int ticket, TypedData px, int w, int h) — zero-copy ExternalTypedData
// path (the demos use the base64 'blit' draw-command instead; kept for S5+).
static void Win_canvasBlit(Dart_NativeArguments args) { /* future: zero-copy */ }
// Game pane — SAME 7-native shape as cocoa_natives.mm:502-508. The D3D11 engine
// + wire live in gp_engine_d3d.cpp / gp_natives_win.cpp (S6); forward-declared
// here so the resolver table below binds them.
void Win_gpOpen(Dart_NativeArguments args);
void Win_gpClose(Dart_NativeArguments args);
void Win_gpApply(Dart_NativeArguments args);
void Win_gpSnap(Dart_NativeArguments args);
void Win_gpSnapPresent(Dart_NativeArguments args);   // T3: on-screen backbuffer -> PNG
void Win_gpStat(Dart_NativeArguments args);
void Win_gpFullscreen(Dart_NativeArguments args);
void Win_gpBackbuffer(Dart_NativeArguments args);

// ── Reverse-callback + key-state (defined in win_callbacks.cpp) ─────────────
void Win_registerCallbackDispatch(Dart_NativeArguments args);  // cf. :492
void Win_keyWatch(Dart_NativeArguments args);                  // cf. :496
void Win_keyCapture(Dart_NativeArguments args);                // cf. :497
void Win_keyState(Dart_NativeArguments args);                  // cf. :498

// ── Workspace primitives — REUSED AS-IS (workspace_natives.cc, OS-neutral) ──
void Workspace_eval(Dart_NativeArguments args);                // cocoa_natives.mm:481
void Workspace_reload(Dart_NativeArguments args);
void Workspace_vmStats(Dart_NativeArguments args);
void Workspace_requestUiReload(Dart_NativeArguments args);
void Workspace_uiReloadStatus(Dart_NativeArguments args);
void Workspace_uiReady(Dart_NativeArguments args);

// ── Debugger primitives (windart_debug_natives.cc) — T4 ─────────────────────
void Workspace_debugArm(Dart_NativeArguments args);
void Workspace_debugPoll(Dart_NativeArguments args);
void Workspace_debugDone(Dart_NativeArguments args);

// ── SQLite image store — REUSED AS-IS (sqlite_natives.cc) ───────────────────
void Sqlite_open(Dart_NativeArguments args);                   // cocoa_natives.mm:511
void Sqlite_close(Dart_NativeArguments args);
void Sqlite_exec(Dart_NativeArguments args);
void Sqlite_query(Dart_NativeArguments args);

// ── The resolver table (== cocoa_natives.mm:516-593) ────────────────────────
#define WIN_NATIVE_LIST(V)                                                     \
  V(Win_surfaceOpenPane, 2)                                                    \
  V(Win_surfaceOpenWindow, 3)                                                  \
  V(Win_surfaceClose, 1)                                                       \
  V(Win_surfaceApply, 3)                                                       \
  V(Win_surfaceSize, 1)                                                        \
  V(Win_widgetText, 1)                                                         \
  V(Win_editorSelection, 1)                                                    \
  V(Win_measureText, 2)                                                        \
  V(Win_openFileDialog, 1)                                                     \
  V(Win_saveFileDialog, 1)                                                     \
  V(Win_messageBox, 3)                                                         \
  V(Win_editorApplySpans, 2)                                                   \
  V(Win_canvasDraw, 2)                                                         \
  V(Win_canvasSnap, 2)                                                         \
  V(Win_surfaceSnapshot, 2)                                                    \
  V(Win_hostQuit, 0)                                                           \
  V(Win_canvasBlit, 4)                                                         \
  V(Win_gpOpen, 5)                                                             \
  V(Win_gpClose, 0)                                                            \
  V(Win_gpApply, 1)                                                            \
  V(Win_gpSnap, 1)                                                             \
  V(Win_gpSnapPresent, 1)                                                      \
  V(Win_gpStat, 0)                                                             \
  V(Win_gpFullscreen, 1)                                                       \
  V(Win_gpBackbuffer, 0)                                                       \
  V(Win_registerCallbackDispatch, 1)                                           \
  V(Win_keyWatch, 0)                                                           \
  V(Win_keyCapture, 1)                                                         \
  V(Win_keyState, 0)                                                           \
  V(Workspace_eval, 1)                                                         \
  V(Workspace_reload, 0)                                                       \
  V(Workspace_vmStats, 0)                                                      \
  V(Workspace_requestUiReload, 0)                                              \
  V(Workspace_uiReloadStatus, 0)                                               \
  V(Workspace_uiReady, 0)                                                      \
  V(Workspace_debugArm, 3)                                                     \
  V(Workspace_debugPoll, 0)                                                    \
  V(Workspace_debugDone, 0)                                                    \
  V(Sqlite_open, 1)                                                            \
  V(Sqlite_close, 1)                                                           \
  V(Sqlite_exec, 3)                                                            \
  V(Sqlite_query, 3)

static struct WinEntry {
  const char* name_;
  Dart_NativeFunction function_;
  int argument_count_;
} WinEntries[] = {
#define WIN_ENTRY(name, argc) {#name, name, argc},
    WIN_NATIVE_LIST(WIN_ENTRY)
#undef WIN_ENTRY
};

Dart_NativeFunction WinNativeLookup(Dart_Handle name,
                                    int argument_count,
                                    bool* auto_setup_scope) {
  const char* function_name = nullptr;
  if (Dart_IsError(Dart_StringToCString(name, &function_name))) return nullptr;
  if (auto_setup_scope != nullptr) *auto_setup_scope = true;
  int num_entries = sizeof(WinEntries) / sizeof(struct WinEntry);
  for (int i = 0; i < num_entries; i++) {
    struct WinEntry* e = &WinEntries[i];
    if (strcmp(function_name, e->name_) == 0 && e->argument_count_ == argument_count) {
      return e->function_;
    }
  }
  return nullptr;
}

const uint8_t* WinNativeSymbol(Dart_NativeFunction nf) {
  int num_entries = sizeof(WinEntries) / sizeof(struct WinEntry);
  for (int i = 0; i < num_entries; i++) {
    if (WinEntries[i].function_ == nf) {
      return reinterpret_cast<const uint8_t*>(WinEntries[i].name_);
    }
  }
  return nullptr;
}

}  // namespace bin
}  // namespace dart
