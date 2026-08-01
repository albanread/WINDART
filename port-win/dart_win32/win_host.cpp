// WINDART GUI host — the Win32 UI-thread pump. Runs the UI isolate on the
// process main thread so Win32 window work (which has thread affinity) and the
// single-threaded Direct2D device are both legal. Only linked into `dartui.exe`
// (DART_UI_HOST in bin/main.cc).
//
// This is a transliteration of TWO references, merged:
//   * cocoa_host.mm            — the thread-0 host STRUCTURE (notify->wake->drain,
//                                the re-entrancy guard, host-driven hot reload).
//   * E:\WINVM gui/src/shell/win.rs — the Win32 MACHINERY (window class/create,
//                                the GetMessageW pump, PostMessageW/WM_APP wake,
//                                the "read HWND from an atomic at notify time"
//                                bug-fix, menu bar, DPI + COM init).
//
// The one non-obvious merge: win.rs's wake handler has no re-entrancy guard, but
// a modal Win32 loop (MessageBox / IFileOpenDialog / menu tracking / the
// DefWindowProc move-size loop) re-enters it exactly as AppKit's tracking loop
// re-enters PumpPerform. So we take win.rs's machinery AND cocoa_host.mm's
// g_in_pump/g_pending guard. See S4_GUI_HOST_DESIGN.md §0.
#include "win_host.h"

#include <commctrl.h>                 // InitCommonControlsEx (button/list/tab classes)
#include <objbase.h>                  // CoInitializeEx (WIN32_LEAN_AND_MEAN omits it)
#include <atomic>
#include <cstdio>
#include <cstdlib>                    // getenv (WINDART_SELFTEST)
#include <cstdint>                    // uint8_t (snapshot buffers, Stage 2)
#include <cstring>                    // memcmp (snapshot version guard, W1)
#include <cwchar>                     // wcscpy_s / wcscat_s (exe-dir path, Stage 2)

#include "include/dart_api.h"
#include "include/dart_tools_api.h"   // Dart_WorkspaceReloadSources (embedder patch)
#include "win_callbacks.h"            // OnMenuCommand, OnSurfaceCommand/Notify routing
#include "win_toolbar.h"              // WinToolbarCreate / WinToolbarHeight (polish)

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")

namespace dart {
namespace bin {

// The worker/other-isolate thread posts this to wake the UI thread and drain the
// isolate's message queue — the performSelectorOnMainThread: / CFRunLoopSource
// analogue. WM_APP+1 (win.rs:69). PostMessageW is the one documented-thread-safe
// way into a window's queue, which is the property this whole design rests on.
static const UINT WM_WINDART_WAKE = WM_APP + 1;

// ~60 Hz UI/metrics tick (SetTimer id), cf. win.rs:71-73.
static const UINT_PTR TIMER_TICK = 1;

// Headless self-test timer (only armed when WINDART_SELFTEST is set): after the
// UI isolate's main() has materialized its widgets, synthesize a BM_CLICK on the
// button so the whole round-trip (materialize -> WM_COMMAND -> ticket ->
// _winDispatch -> Dart closure -> print) runs with no human, then exit cleanly.
static const UINT_PTR TIMER_SELFTEST = 2;

// Menu command ids live in win_host.h (WinHostCommand) — shared with the toolbar
// and the OnMenuCommand dispatch.

// ── Dart-routed menu/toolbar command queue (single UI thread; no lock) ────────
static int g_menu_q[32];
static int g_menu_q_head = 0, g_menu_q_tail = 0;
void windart_push_menu_command(int id) {
  int n = (g_menu_q_tail + 1) % 32;
  if (n != g_menu_q_head) { g_menu_q[g_menu_q_tail] = id; g_menu_q_tail = n; }
}
int windart_take_menu_command() {
  if (g_menu_q_head == g_menu_q_tail) return -1;
  int id = g_menu_q[g_menu_q_head];
  g_menu_q_head = (g_menu_q_head + 1) % 32;
  return id;
}

// The icon toolbar + its measured band height (0 until it exists). The pane
// container leaves this much room at the top of the client (win_view OpenPane).
static HWND g_toolbar = nullptr;
static int g_toolbar_h = 0;
int WinHostToolbarHeight() { return g_toolbar_h; }
HWND WinHostToolbarHwnd() { return g_toolbar; }

// The window handle as a raw intptr in an atomic, so NotifyUi (any thread) can
// read it at notify time. THE BUG-FIX from win.rs:159-211: the VM spawns
// isolates BEFORE this host creates the window, so a waker that captured the
// handle at construction would capture 0 and silently drop every wakeup for the
// life of the process. Read it fresh instead.
static std::atomic<intptr_t> g_main_hwnd{0};   // win.rs:95

// Re-entrancy + lost-wake guard (cocoa_host.mm:28-29).
static bool g_in_pump = false;
static bool g_pending = false;

// UI readiness + host-driven reload state (cocoa_host.mm:37-39).
static bool g_ui_ready = false;
static bool g_ui_reload_requested = false;
static char g_ui_reload_status[1024] = {0};

static const wchar_t* kClassName = L"WindartWorkspaceWindow";

HWND WinHostMainHwnd() {
  return reinterpret_cast<HWND>(g_main_hwnd.load(std::memory_order_relaxed));
}

// The shared window class, so the view-server can create surface windows/panes
// that route WM_COMMAND/WM_NOTIFY to the same WndProc (S4_GUI_HOST_DESIGN.md §3.1).
const wchar_t* WinHostWindowClassName() { return kClassName; }

// ── Headless self-test: find the first BUTTON descendant and click it ────────
static HWND g_found_button = nullptr;
static BOOL CALLBACK FindButtonProc(HWND child, LPARAM /*p*/) {
  wchar_t cls[64] = {0};
  GetClassNameW(child, cls, 64);
  if (lstrcmpiW(cls, L"Button") == 0) { g_found_button = child; return FALSE; }
  return TRUE;
}
static void RunSelfTest(HWND main) {
  g_found_button = nullptr;
  EnumChildWindows(main, FindButtonProc, 0);
  if (g_found_button == nullptr) return;   // not materialized yet; retry next tick
  KillTimer(main, TIMER_SELFTEST);
  std::fprintf(stderr, "dartui: [selftest] synthesizing BM_CLICK on the button\n");
  SendMessageW(g_found_button, BM_CLICK, 0, 0);   // -> WM_COMMAND -> Dart handler prints
  std::fflush(stdout);
  std::fflush(stderr);
  PostMessageW(main, WM_CLOSE, 0, 0);      // clean exit -> WM_DESTROY -> WM_QUIT
}

// ── Worker -> UI wakeup (cocoa_host.mm:121-131) ─────────────────────────────
// May be called from ANY thread (the IO event-handler thread, another isolate).
// Only wakes; the actual Dart_HandleMessages happens on the pump thread in
// OnWake. Applies to the CURRENT (UI) isolate only — set in windart_run_ui_host.
static void NotifyUi(Dart_Isolate /*dest_isolate*/) {
  intptr_t h = g_main_hwnd.load(std::memory_order_relaxed);
  if (h == 0) return;   // window not up yet; the next response will wake it (win.rs:198)
  PostMessageW(reinterpret_cast<HWND>(h), WM_WINDART_WAKE, 0, 0);   // win.rs:202
}

// ── Host-driven UI hot reload (cocoa_host.mm:43-80) ─────────────────────────
extern "C" void windart_ui_ready(void) { g_ui_ready = true; }

extern "C" void windart_request_ui_reload(void) {
  g_ui_reload_requested = true;
  HWND h = WinHostMainHwnd();
  if (h) PostMessageW(h, WM_WINDART_WAKE, 0, 0);   // signal + wake (cocoa_host.mm:47-50)
}

extern "C" const char* windart_take_ui_reload_status(void) {
  if (g_ui_reload_status[0] == '\0') return nullptr;
  static char out[1024];
  std::snprintf(out, sizeof(out), "%s", g_ui_reload_status);
  g_ui_reload_status[0] = '\0';
  return out;
}

// ── Stage 2: boot the core snapshot from on-disk .bin (swappable, no relink) ──
// dartui normally boots the snapshot baked into snapshot_gen.cc. When BOTH
// vm_isolate_snapshot.bin and isolate_snapshot.bin sit next to the exe, load them
// and override the snapshot pointers BEFORE Dart_Initialize, so the snapshot can
// be regenerated without recompiling/relinking C++. ALL-OR-NOTHING: override only
// if BOTH read fully; otherwise leave both baked (a half-swap mixes snapshots from
// different builds and Dart_CreateIsolate rejects it). The JIT snapshots are
// data-only, so *_instructions stay baked (NULL). The VM does not copy the
// buffers, so they are process-lifetime (intentionally leaked). Called from
// bin/main.cc just before Dart_Initialize (DART_UI_HOST only).
extern const uint8_t* vm_snapshot_data;             // snapshot_gen.cc (dart::bin)
extern const uint8_t* core_isolate_snapshot_data;   //   ""

// W1 (snapshot-as-blob): the boot snapshot can live IN the SQLite image. The read
// path is in windart_snapshot_blob.cc (returns a malloc'd process-lifetime buffer
// or NULL). Fallback chain below: DB blob -> on-disk .bin -> baked array.
extern "C" unsigned char* windart_read_snapshot_blob(const char* key,
                                                     unsigned int* out_size);

// A candidate snapshot (DB blob) is compatible with THIS build iff its version +
// features header matches the baked array's. Snapshot layout is
// [16-byte Snapshot header][version (no null)][features (null-terminated)][data];
// comparing [16 .. features-null] covers version+features and STOPS before the data,
// so a legitimately re-baked snapshot (same VM build, new classes/data) still
// matches. The baked arrays are always present and are the authoritative "this
// build's version", so no VM-internal symbol is needed. A mismatch -> skip the blob
// and fall back, rather than let Dart_CreateIsolate abort on a stale snapshot.
static bool WindartSnapshotCompatible(const unsigned char* cand, unsigned int candSz,
                                      const uint8_t* baked) {
  const int H = 16, CAP = 1024;   // Snapshot::kHeaderSize == 2*sizeof(int64_t)
  if (static_cast<int>(candSz) <= H) return false;
  int ci = H;
  while (ci < static_cast<int>(candSz) && ci < H + CAP && cand[ci] != 0) ci++;
  int bi = H;
  while (bi < H + CAP && baked[bi] != 0) bi++;
  if (ci >= H + CAP || bi >= H + CAP || ci != bi) return false;
  return memcmp(cand + H, baked + H, static_cast<size_t>(ci - H)) == 0;
}

static uint8_t* WindartReadWholeFile(const wchar_t* path, DWORD* out_size) {
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  LARGE_INTEGER sz;
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 0x7fffffffLL) {
    CloseHandle(h);
    return nullptr;
  }
  DWORD n = static_cast<DWORD>(sz.QuadPart);
  uint8_t* buf = static_cast<uint8_t*>(malloc(n));
  if (buf == nullptr) { CloseHandle(h); return nullptr; }
  DWORD off = 0, got = 0;
  BOOL ok = TRUE;
  while (off < n && (ok = ReadFile(h, buf + off, n - off, &got, nullptr)) != FALSE &&
         got > 0) {
    off += got;
  }
  CloseHandle(h);
  if (ok == FALSE || off != n) { free(buf); return nullptr; }
  *out_size = n;
  return buf;
}

extern "C" void windart_load_snapshots_from_disk(void) {
  // 1) The world: boot the snapshot from the SQLite image `blobs` table, if BOTH
  //    blobs are present AND version-compatible with this build. (Same all-or-
  //    nothing rule as the .bin path — a half/mismatched swap would abort.)
  {
    unsigned int vSz = 0, iSz = 0;
    unsigned char* vBlob = windart_read_snapshot_blob("snapshot:vm", &vSz);
    unsigned char* iBlob = windart_read_snapshot_blob("snapshot:isolate", &iSz);
    if (vBlob != nullptr && iBlob != nullptr &&
        WindartSnapshotCompatible(vBlob, vSz, vm_snapshot_data) &&
        WindartSnapshotCompatible(iBlob, iSz, core_isolate_snapshot_data)) {
      vm_snapshot_data = vBlob;            // process-lifetime (VM does not copy)
      core_isolate_snapshot_data = iBlob;
      std::fprintf(
          stderr,
          "[windart] snapshot: booting from DB image blob (vm=%u iso=%u bytes)\n",
          vSz, iSz);
      return;
    }
    if (vBlob != nullptr) free(vBlob);     // absent/stale -> fall back to the .bin
    if (iBlob != nullptr) free(iBlob);
  }
  // 2) Stage 2: the two on-disk .bin next to the exe.
  wchar_t exe[MAX_PATH];
  DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return;
  while (len > 0 && exe[len - 1] != L'\\' && exe[len - 1] != L'/') len--;
  exe[len] = L'\0';                         // strip the filename -> dir + trailing slash
  wchar_t vmPath[MAX_PATH], isoPath[MAX_PATH];
  wcscpy_s(vmPath, MAX_PATH, exe);   wcscat_s(vmPath, MAX_PATH, L"vm_isolate_snapshot.bin");
  wcscpy_s(isoPath, MAX_PATH, exe);  wcscat_s(isoPath, MAX_PATH, L"isolate_snapshot.bin");
  DWORD vmSz = 0, isoSz = 0;
  uint8_t* vmBuf = WindartReadWholeFile(vmPath, &vmSz);
  uint8_t* isoBuf = WindartReadWholeFile(isoPath, &isoSz);
  if (vmBuf != nullptr && isoBuf != nullptr) {
    vm_snapshot_data = vmBuf;
    core_isolate_snapshot_data = isoBuf;
    std::fprintf(stderr,
                 "[windart] snapshot: booting from on-disk .bin (vm=%lu iso=%lu bytes)\n",
                 static_cast<unsigned long>(vmSz), static_cast<unsigned long>(isoSz));
  } else {
    if (vmBuf != nullptr) free(vmBuf);
    if (isoBuf != nullptr) free(isoBuf);
    std::fprintf(stderr,
                 "[windart] snapshot: using baked-in arrays (on-disk .bin absent/short)\n");
  }
}

// Only ever called from OnWake, after Dart_HandleMessages returns — the isolate
// is entered but quiescent, Dart off the stack. ReloadSources is atomic: a
// syntax error is CANCELLED and running code is untouched. (cocoa_host.mm:67-80.)
static void PerformUiReload(void) {
  g_ui_reload_requested = false;
  Dart_EnterScope();
  Dart_Handle r = Dart_WorkspaceReloadSources(true /* force_reload */);
  if (Dart_IsError(r)) {
    std::snprintf(g_ui_reload_status, sizeof(g_ui_reload_status), "ERR: %s",
                  Dart_GetError(r));
    std::fprintf(stderr, "dartui: UI reload cancelled: %s\n", Dart_GetError(r));
  } else {
    std::snprintf(g_ui_reload_status, sizeof(g_ui_reload_status), "ok");
    std::fprintf(stderr, "dartui: UI reloaded\n");
  }
  Dart_ExitScope();
}

// ── The drain (== cocoa_host.mm:85-119 PumpPerform) ─────────────────────────
// Drains the UI isolate's message queue: the queued main() on the first tick,
// then socket events, timers, cross-isolate replies, and widget callbacks.
static void OnWake(void) {
  if (g_in_pump) {
    // Re-entered by a nested modal/menu loop (S4_GUI_HOST_DESIGN.md §0). Handling
    // messages here would nest Dart_HandleMessages; dropping it would lose the
    // wake. Remember it and re-drain once the outer pump unwinds (cocoa_host.mm:87-94).
    g_pending = true;
    return;
  }
  g_in_pump = true;
  do {
    g_pending = false;
    Dart_EnterScope();
    Dart_Handle r = Dart_HandleMessages();
    if (Dart_IsError(r)) {
      std::fprintf(stderr, "dartui: UI isolate error: %s\n", Dart_GetError(r));
      if (!g_ui_ready) {
        // The script failed to load or build a window. Carrying on leaves a
        // running process with nothing on screen and no control socket — looks
        // like a hang. Say how to recover and stop. (cocoa_host.mm:107-112.)
        std::fprintf(stderr,
            "dartui: the UI never started — restore the last good source with:\n"
            "    start-gui.ps1 -Restore\n");
        Dart_ExitScope();
        std::exit(70);
      }
      // After ready: a callback that throws is survivable — log and keep the
      // window (leak-over-crash).
    }
    Dart_ExitScope();
    // Dart is off the stack here — the only safe moment to swap its code out.
    if (g_ui_reload_requested) PerformUiReload();
  } while (g_pending);
  g_in_pump = false;
}

// ── WNDPROC (== win.rs:231-277) ─────────────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_WINDART_WAKE:            // the UI isolate has work; drain it
      OnWake();
      return 0;

    case WM_TIMER:
      if (w == TIMER_TICK) { /* OnTick(): push a pull-frame invite, sample metrics */ }
      else if (w == TIMER_SELFTEST) { RunSelfTest(hwnd); }
      return 0;

    case WM_COMMAND: {
      // Route by ID SPACE, not by lParam. Host commands (menu items AND icon-
      // toolbar buttons) carry a WinHostCommand id in the fixed low block
      // (< 0x100). A materialized control's notification carries its ticket
      // (>= 0x100, win.dart _nextTicket), disjoint by design. The lParam gate the
      // old code used broke the TOOLBAR: a toolbar button posts WM_COMMAND with
      // lParam == the toolbar HWND (non-zero), so it never matched l==0 and fell
      // through to OnSurfaceCommand, which failed closed (145 is no widget ticket).
      WORD id = LOWORD(w);
      WORD code = HIWORD(w);
      if (id < 0x100) {                           // menu item OR toolbar button
        OnMenuCommand(id);                        // win.rs:243-246 -> dispatch_menu_command
      } else {
        // Control notification: forward to the callbacks dispatcher (win_callbacks.cpp).
        OnSurfaceCommand(hwnd, id, code, reinterpret_cast<HWND>(l));
      }
      return 0;
    }

    case WM_NOTIFY:
      // List-view data source / selection (LVN_GETDISPINFO, LVN_ITEMCHANGED, …).
      return OnSurfaceNotify(hwnd, reinterpret_cast<NMHDR*>(l));

    case WM_CTLCOLORSTATIC:
      // Visual styles otherwise paint STATIC labels on a grey dialog face. Draw
      // them transparently over the window's (white) background so they blend.
      SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));

    case WM_SIZE:
      // On the main window: keep the toolbar spanning the top and the pane
      // container filling the client BELOW it. The container is the sole child of
      // main using our window class (the toolbar is TOOLBARCLASSNAME).
      if (hwnd == WinHostMainHwnd()) {
        int cw = LOWORD(l), ch = HIWORD(l);
        if (g_toolbar) SendMessageW(g_toolbar, TB_AUTOSIZE, 0, 0);
        HWND cont = FindWindowExW(hwnd, nullptr, kClassName, nullptr);
        if (cont) MoveWindow(cont, 0, g_toolbar_h, cw, ch - g_toolbar_h, TRUE);
      }
      OnSurfaceResize(hwnd, LOWORD(l), HIWORD(l));
      return 0;

    case WM_SETFOCUS:
      // Land focus on the active child (editor/field) rather than the bare host.
      // cf. win.rs:261-268 (which moved focus into the WebView2).
      return 0;

    case WM_CLOSE:
      // A window-surface close is an explicit "I'm done" -> event to the app
      // (kind 8), then destroy. The main workspace window falls through to
      // DestroyWindow -> WM_DESTROY -> quit.
      if (hwnd == WinHostMainHwnd()) { DestroyWindow(hwnd); return 0; }
      OnSurfaceClose(hwnd);
      return 0;

    case WM_DESTROY:
      // Only the MAIN window's destruction quits the app — a pane container (same
      // window class) being torn down must not PostQuitMessage.
      if (hwnd == WinHostMainHwnd()) PostQuitMessage(0);   // win.rs:270-273
      return 0;

    default:
      return DefWindowProcW(hwnd, msg, w, l);
  }
}

// ── Window class + main window (== win.rs:382-412) ──────────────────────────
static void RegisterMainWindowClass() {
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClassName;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  // A background brush so the client area ERASES when a child is destroyed
  // (tab-switching removes+re-adds content; without this the revealed area keeps
  // the old widget's stale pixels, which PrintWindow then captures).
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&wc);
}

// Build the menu bar (== win.rs:302-355). Skeleton; fill the full File/Edit/VM/
// Help set. Item actions are named with the same strings the workspace expects,
// so one dispatch (OnMenuCommand) serves them.
static void BuildMenuBar(HWND hwnd) {
  HMENU bar = CreateMenu();

  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, CMD_NEW,  L"&New Class\tCtrl+N");
  AppendMenuW(file, MF_STRING, CMD_OPEN, L"&Open\tCtrl+O");
  AppendMenuW(file, MF_STRING, CMD_SAVE, L"&Save\tCtrl+S");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, CMD_EXIT, L"E&xit");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");

  HMENU edit = CreatePopupMenu();
  AppendMenuW(edit, MF_STRING, CMD_DOIT,    L"&Do It\tCtrl+D");
  AppendMenuW(edit, MF_STRING, CMD_PRINTIT, L"&Print It\tCtrl+P");
  AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(edit, MF_STRING, CMD_CUT,   L"Cu&t\tCtrl+X");
  AppendMenuW(edit, MF_STRING, CMD_COPY,  L"&Copy\tCtrl+C");
  AppendMenuW(edit, MF_STRING, CMD_PASTE, L"&Paste\tCtrl+V");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"&Edit");

  HMENU view = CreatePopupMenu();
  const wchar_t* tabs[] = {L"Workspace", L"Browser", L"Editor", L"Find", L"Docs",
                           L"App", L"Debug", L"VM", L"Help"};
  for (int i = 0; i < 9; i++)
    AppendMenuW(view, MF_STRING, CMD_TAB_BASE + i, tabs[i]);
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, CMD_REFRESH,   L"&Refresh\tF5");
  AppendMenuW(view, MF_STRING, CMD_RELOAD_UI, L"Reload &UI from Source");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");

  HMENU help = CreatePopupMenu();
  AppendMenuW(help, MF_STRING, CMD_ABOUT, L"&About WINDART");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");

  SetMenu(hwnd, bar);
  DrawMenuBar(hwnd);
}

// ── Entry point (== macdart_run_ui_host, cocoa_host.mm:133-172) ─────────────
extern "C" int windart_run_ui_host(void) {
  // 1. Process prerequisites (win.rs:223-229). PER_MONITOR_AWARE_V2 stops Windows
  //    bitmap-stretching Direct2D output on a scaled monitor; APARTMENTTHREADED
  //    COM is required by D2D/DWrite/WIC and IFileOpenDialog. S_FALSE == already
  //    initialised on this thread == success.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  // Register the common-control classes (BUTTON is a user32 class, but the
  // view-server will materialize SysListView32/SysTabControl32/etc. later; do it
  // once up front so the whole widget catalog is available).
  INITCOMMONCONTROLSEX icc = {sizeof(icc),
      ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES |
      ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES};   // + toolbar/tab/listview (themed)
  InitCommonControlsEx(&icc);
  LoadLibraryW(L"Msftedit.dll");   // registers the RICHEDIT50W editor class (S7.2)

  // 2. Create the main workspace window; publish its HWND to the atomic BEFORE
  //    anything can wake us (win.rs:382-412).
  RegisterMainWindowClass();
  HWND main = CreateWindowExW(
      0, kClassName, L"WINDART", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      CW_USEDEFAULT, CW_USEDEFAULT, 1100, 800,
      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  g_main_hwnd.store(reinterpret_cast<intptr_t>(main), std::memory_order_relaxed);
  BuildMenuBar(main);
  // The icon toolbar band (created BEFORE the app's pane so OpenPane can inset the
  // pane container below it). Measure its height for the inset.
  g_toolbar = WinToolbarCreate(main);
  g_toolbar_h = WinToolbarHeight(g_toolbar);
  ShowWindow(main, SW_SHOW);

  // 3. Route THIS (UI) isolate's message wakeups to our pump (cocoa_host.mm:150).
  Dart_SetMessageNotifyCallback(&NotifyUi);

  // 4. Start the periodic tick, then kick the first drain so the queued main()
  //    runs before we block in GetMessageW (cocoa_host.mm:164-167).
  SetTimer(main, TIMER_TICK, 1000 / 60, nullptr);
  if (std::getenv("WINDART_SELFTEST") != nullptr) {
    SetTimer(main, TIMER_SELFTEST, 400, nullptr);   // headless click after main() runs
  }
  PostMessageW(main, WM_WINDART_WAKE, 0, 0);

  // 5. Own the thread until WM_QUIT (win.rs:685-693). GetMessageW returns 0 on
  //    WM_QUIT and -1 on error.
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  Dart_SetMessageNotifyCallback(nullptr);   // cocoa_host.mm:169
  return 0;
}

}  // namespace bin
}  // namespace dart
