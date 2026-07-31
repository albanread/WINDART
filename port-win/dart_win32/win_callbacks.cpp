// WINDART dart:win — reverse callbacks (WNDPROC/subclass -> ticket -> Dart) and
// the per-frame key-state poll. Replaces macdart/cocoa/cocoa_callbacks.mm.
//
// Ports the ticket design DIRECTLY (WINDOWS_PORTING_PLAN.md §5). Invariants kept:
//   * the native side stores only an integer TICKET, never a Dart handle
//     (cocoa_callbacks.mm:6-8, :30);
//   * one Dart closure _winDispatch(ticket, kind, arg) is the single funnel
//     (cocoa_callbacks.mm:23-50, Dart_InvokeClosure);
//   * a dead ticket FAILS CLOSED (cocoa_callbacks.mm:40).
//
// The Windows simplification (S4_GUI_HOST_DESIGN.md §3.1): where Cocoa synthesized
// a MacdartActionTarget class whose IMPs are the target/delegate/data-source
// (cocoa_callbacks.mm:158-175), Win32 controls notify their PARENT (the surface
// window) via WM_COMMAND/WM_NOTIFY, and we choose the child control-id. So we set
// control-id == ticket and the surface WndProc is the single dispatch hub — no
// synthesized class, no per-control target object. SetWindowSubclass is used only
// for field-Enter and custom Direct2D widgets.
//
// Runs on the UI thread (single-threaded): no g_mu is needed (cocoa needed one at
// :30 because AppKit finalizers can run off-thread; our dispatch never does).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>   // WM_NOTIFY, LVN_*, NMLVDISPINFO, SetWindowSubclass

#include <cstdio>
#include <cstring>

#include "include/dart_api.h"
#include "win_callbacks.h"
#include "win_view.h"

#pragma comment(lib, "comctl32.lib")

namespace dart {
namespace bin {

// The single Dart dispatcher `dynamic _winDispatch(int ticket, int kind, int arg)`
// (win.dart). Kinds (S4_GUI_HOST_DESIGN.md §4.3):
//   0 click · 1 text · 2 rowCount · 3 cell(arg=row) · 4 select(arg=row)
//   5 enter · 6 toggle(arg=checked) · 7 resize(arg=packed w,h) · 8 close
// Returns void for 0/1/4/5/6/7/8, an int for 2, a String for 3 — marshaled per
// kind, exactly as cocoa (cocoa_callbacks.mm NumRowsIMP/ObjectValueIMP).
static Dart_PersistentHandle g_dispatch = nullptr;   // cf. :27

// Invoke the Dart dispatcher; NULL on failure (fail closed). Caller must be in a
// Dart scope and marshal the result per kind. (cf. Dispatch, cocoa_callbacks.mm:35-50.)
static Dart_Handle Dispatch(int64_t ticket, int kind, int64_t arg) {
  if (g_dispatch == nullptr) return nullptr;
  Dart_Handle fn = Dart_HandleFromPersistent(g_dispatch);
  Dart_Handle a[3] = { Dart_NewInteger(ticket), Dart_NewInteger(kind),
                       Dart_NewInteger(arg) };
  return Dart_InvokeClosure(fn, 3, a);
}

// _registerWinDispatch(Function f) — store the Dart dispatcher (called once).
// (== Cocoa_registerCallbackDispatch, cocoa_callbacks.mm:178-182.)
void Win_registerCallbackDispatch(Dart_NativeArguments args) {
  Dart_Handle closure = Dart_GetNativeArgument(args, 0);
  if (g_dispatch != nullptr) Dart_DeletePersistentHandle(g_dispatch);
  g_dispatch = Dart_NewPersistentHandle(closure);
}

// ── Menu (== win.rs:279-297 dispatch_menu_command) ──────────────────────────
void OnMenuCommand(WORD id) {
  switch (id) {
    // Standard workspace actions named by the same strings the mac side used, so
    // one dispatch serves them. Cut/Copy/Paste/Select-All target the focused
    // editor directly (no responder-chain trick — Win32 has none to need;
    // cf. cocoa_callbacks.mm:218-229).
    // case MENU_CLOSE: PostMessageW(WinHostMainHwnd(), WM_CLOSE, 0, 0); break;
    // case MENU_RELOAD_UI: windart_request_ui_reload(); break;
    default: break;
  }
}

// ── Control notifications (WM_COMMAND from a button/checkbox/edit) ───────────
// id == ticket (the control-id we assigned at CreateWindowExW). code is the
// notification (BN_CLICKED, EN_CHANGE, …). (S4_GUI_HOST_DESIGN.md §3.1 table.)
void OnSurfaceCommand(HWND /*host*/, WORD id, WORD code, HWND /*ctrl*/) {
  if (WinViewInApply()) return;   // skip re-entrant dispatch during materialize
  Widget* w = ViewServer::Instance().WidgetByTicket(id);
  if (!w) return;   // fail closed (stale ticket, e.g. post-rebuild — §3.2)

  Dart_EnterScope();
  switch (w->kind) {
    case WidgetKind::kButton:
      if (code == BN_CLICKED) Dispatch(id, /*click*/ 0, 0);
      break;
    case WidgetKind::kCheckbox:
    case WidgetKind::kRadio:
      if (code == BN_CLICKED) {
        LRESULT st = SendMessageW(w->hwnd, BM_GETCHECK, 0, 0);
        Dispatch(id, /*toggle*/ 6, st == BST_CHECKED ? 1 : 0);
      }
      break;
    case WidgetKind::kField:
      if (code == EN_CHANGE) Dispatch(id, /*text*/ 1, 0);   // handler pulls via Win_widgetText
      break;
    case WidgetKind::kPopup:
      if (code == CBN_SELCHANGE) {
        LRESULT idx = SendMessageW(w->hwnd, CB_GETCURSEL, 0, 0);
        Dispatch(id, /*select*/ 4, (int64_t)idx);
      }
      break;
    default: break;
  }
  Dart_ExitScope();
}

// ── List-view data source + selection (WM_NOTIFY) ───────────────────────────
// LVS_OWNERDATA (virtual) list: the data source is PULL-based, matching
// NSTableView. LVN_GETDISPINFO -> cell text (kind 3); LVN_ODCACHEHINT / the count
// we push -> rowCount (kind 2, via LVM_SETITEMCOUNT at apply time);
// LVN_ITEMCHANGED -> selection (kind 4). (cocoa_callbacks.mm:86-120.)
LRESULT OnSurfaceNotify(HWND /*host*/, NMHDR* hdr) {
  if (WinViewInApply()) return 0;   // skip re-entrant dispatch during materialize
  Widget* w = ViewServer::Instance().WidgetByTicket((int64_t)hdr->idFrom);
  if (!w) return 0;

  // Tab strip (SysTabControl32): a selection change dispatches kind 4 (select).
  if (w->kind == WidgetKind::kTabs) {
    if (hdr->code == TCN_SELCHANGE) {
      int idx = (int)SendMessageW(w->hwnd, TCM_GETCURSEL, 0, 0);
      Dart_EnterScope();
      Dispatch(w->ticket, /*select*/ 4, idx);
      Dart_ExitScope();
    }
    return 0;
  }

  if (w->kind != WidgetKind::kList) return 0;

  switch (hdr->code) {
    case LVN_GETDISPINFOW: {                          // cell text for a row (kind 3)
      NMLVDISPINFOW* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
      if (di->item.mask & LVIF_TEXT) {
        Dart_EnterScope();
        Dart_Handle r = Dispatch(w->ticket, /*cell*/ 3, di->item.iItem);
        const char* c = nullptr;
        if (r != nullptr && !Dart_IsError(r) &&
            !Dart_IsError(Dart_StringToCString(r, &c)) && c != nullptr) {
          // widen UTF-8 -> the pszText UTF-16 buffer the control gave us
          MultiByteToWideChar(CP_UTF8, 0, c, -1, di->item.pszText, di->item.cchTextMax);
        }
        Dart_ExitScope();
      }
      return 0;
    }
    case LVN_ITEMCHANGED: {                           // selection (kind 4)
      NMLISTVIEW* lv = reinterpret_cast<NMLISTVIEW*>(hdr);
      if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED)) {
        Dart_EnterScope();
        Dispatch(w->ticket, /*select*/ 4, lv->iItem);
        Dart_ExitScope();
      }
      return 0;
    }
    default:
      return 0;
  }
}

// ── Surface resize / close (kinds 7, 8) ─────────────────────────────────────
void OnSurfaceResize(HWND host, int w, int h) {
  Surface* s = ViewServer::Instance().SurfaceByHost(host);
  if (!s) return;
  // Debounce, then push a resize event so the app can re-run build() with new
  // bounds (APP_PANE_PLAN.md §7). Pack w,h into arg (or push new bounds and let
  // the handler read surfaceSize).
  Dart_EnterScope();
  Dispatch(s->ticket, /*resize*/ 7, ((int64_t)w << 32) | (uint32_t)h);
  Dart_ExitScope();
}

void OnSurfaceClose(HWND host) {
  Surface* s = ViewServer::Instance().SurfaceByHost(host);
  if (!s) return;
  Dart_EnterScope();
  Dispatch(s->ticket, /*close*/ 8, 0);   // "I'm done" (APP_PANE_PLAN.md §7)
  Dart_ExitScope();
}

// The field-Enter subclass proc (installed on 'field' widgets only). dwRefData
// carries the ticket. (S4_GUI_HOST_DESIGN.md §3.1, kind 5.)
static LRESULT CALLBACK FieldSubclassProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                          UINT_PTR /*id*/, DWORD_PTR ticket) {
  if (msg == WM_KEYDOWN && w == VK_RETURN) {
    Dart_EnterScope();
    Dispatch((int64_t)ticket, /*enter*/ 5, 0);
    Dart_ExitScope();
    return 0;   // swallow the ding
  }
  if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, FieldSubclassProc, 1);
  return DefSubclassProc(hwnd, msg, w, l);
}
// The materializer calls this when it creates a 'field' with an onEnter handler.
void InstallFieldEnter(HWND field, int64_t ticket) {
  SetWindowSubclass(field, FieldSubclassProc, 1, (DWORD_PTR)ticket);
}

// ─────────────────────────────────────────────────────────────────────────────
// Key-state — POLL, not event stream (cocoa_callbacks.mm:288-346).
// Games read which keys are down at frame time, polled once per frame with the
// pull tick — no event queue can back up. GetAsyncKeyState is ALREADY a poll, so
// unlike Cocoa (which needed an NSEvent local monitor writing a bitset,
// :303-323) there is no monitor and no bitset to maintain.
// ─────────────────────────────────────────────────────────────────────────────

static bool g_key_capture = false;   // cf. cocoa_callbacks.mm:300

// The game-relevant virtual keys we report, paired with the macOS virtual
// keycodes the portable game Dart already expects (cocoa.dart:222). Emitting the
// macOS codes here keeps that Dart UNCHANGED (S4_GUI_HOST_DESIGN.md §3.3, delta 1).
struct KeyMap { int vk; int macCode; };
static const KeyMap kKeys[] = {
    {VK_LEFT, 123}, {VK_RIGHT, 124}, {VK_DOWN, 125}, {VK_UP, 126},
    {VK_SPACE, 49}, {'A', 0}, {'D', 2}, {'W', 13}, {'S', 1},
    // … extend as demos require …
};

// _keyWatch() — idempotent init. No monitor needed on Windows; keep the native so
// the Dart API (keyWatch()) is unchanged (cocoa.dart:207).
void Win_keyWatch(Dart_NativeArguments /*args*/) { /* no-op: GetAsyncKeyState polls */ }

// _keyCapture(int on) — while on, the game-pane child WndProc swallows plain key
// events (no ding, no typing into the workspace) so a game owns the keyboard;
// Alt/Ctrl shortcuts pass through. Just flips the flag the game-pane WndProc reads.
// (cocoa_callbacks.mm:325-332.)
void Win_keyCapture(Dart_NativeArguments args) {
  int64_t on = 0;
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 0), &on);
  g_key_capture = (on != 0);
}
bool KeyCaptureActive() { return g_key_capture; }   // read by the game-pane WndProc (S6)

// _keyState() -> [downMacCodes, modifierMask]  (cocoa_callbacks.mm:334-346).
void Win_keyState(Dart_NativeArguments args) {
  int down[64]; int n = 0;
  for (const KeyMap& k : kKeys) {
    if (GetAsyncKeyState(k.vk) & 0x8000) down[n++] = k.macCode;   // high bit == held
  }
  Dart_Handle list = Dart_NewList(n);
  for (int i = 0; i < n; i++) Dart_ListSetAt(list, i, Dart_NewInteger(down[i]));

  // A macOS-style modifier mask so the portable game Dart reads it unchanged.
  int64_t mods = 0;
  if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= (1 << 17);   // NSShift
  if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= (1 << 18);   // NSControl
  if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= (1 << 19);   // NSOption(Alt)
  if (GetAsyncKeyState(VK_LWIN)    & 0x8000) mods |= (1 << 20);   // NSCommand(Win)

  Dart_Handle out = Dart_NewList(2);
  Dart_ListSetAt(out, 0, list);
  Dart_ListSetAt(out, 1, Dart_NewInteger(mods));
  Dart_SetReturnValue(args, out);
}

}  // namespace bin
}  // namespace dart
