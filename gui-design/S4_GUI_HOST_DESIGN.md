# WINDART S4 — GUI host layer design (`dart_win32`)

Design-before-code spec for Sprint **S4** (`SPRINTS.md` §S4): the Win32 UI-thread
host, the native resolver + view-server materializer, the reverse-callback
dispatcher, and the Dart-facing `dart:win` API. Written so the S4 coder fills in
skeletons, not a blank page.

**Locked architecture** (`WINDOWS_PORTING_PLAN.md` §3, do not relitigate): the
IDE chrome, the app pane, the demo canvas and the game pane are all served by
**one native Win32 + Direct2D view-server**. User/IDE Dart *describes* widgets
and *receives* events over a message protocol; a C++ materializer realizes them.
There is **no** dynamic "call any selector" bridge on Windows — the `dart:win`
native surface is a **fixed catalog of concrete ops**, and that catalog *is* the
widget/canvas/event protocol.

The port is a **transliteration, not a redesign**. Two sources define the
contract:
- The **thread model + wire contract + patterns** come from MACDART's Cocoa host
  (`macdart/cocoa/*.mm`, `.dart`). Preserved verbatim in shape.
- The **Win32 window/pump/wake machinery** comes from WINVM's shipping Rust host
  (`E:\WINVM\gui\src\shell\win.rs`). Transliterated to C++. Its WebView2 half is
  dropped (`win.rs:435-588`); we materialize native controls + Direct2D instead.

Skeletons in this directory:

| file | replaces | role |
|---|---|---|
| `win_host.h` / `win_host.cpp` | `cocoa_host.mm` | UI-thread window + message pump + wake + host-driven reload |
| `win_natives.h` / `win_natives.cpp` | `cocoa_natives.mm` | `WIN_NATIVE_LIST` resolver, wire contract, the view-server op catalog + materializer |
| `win_callbacks.cpp` | `cocoa_callbacks.mm` | WNDPROC/subclass → ticket → single Dart dispatcher; key-state poll |
| `win_view.h` | (new) | the materializer's internal model (surface + widget registry) |
| `win.dart` | `cocoa.dart` | the Dart client API; **no** `noSuchMethod` selector bridge |

---

## 0. Thread model — the constraint that decides everything

MACDART pins the UI isolate to **OS thread 0** because AppKit rejects `NSWindow`
work off the main thread (`cocoa_host.mm:6-8`). Win32 has the *same* rule for a
different reason: **window/thread affinity** — a window's messages are delivered
only to the thread that created it, and that thread must run the message pump.
So the constraint survives the port unchanged, and its resolution is identical in
spirit:

- `RunMainIsolate` (in patched `bin/main.cc`) runs on the process main thread
  with the UI isolate **entered** and `main()` already queued as the startup
  message.
- Instead of `Dart_RunLoop()` (which services the isolate on a VM pool thread —
  wrong thread for windows), we call **`windart_run_ui_host()`** which owns that
  thread with a `GetMessageW` pump (`win.rs:685-693`).
- `Dart_SetMessageNotifyCallback(&NotifyUi)` routes the UI isolate's wakeups to
  our pump; `NotifyUi` (any thread) only **wakes**, `Dart_HandleMessages()` runs
  on the pump thread (`cocoa_host.mm:120-131` → `PostMessageW` instead of
  `CFRunLoopSourceSignal`).

Everything the UI isolate does — `main()`, the control socket, timers,
cross-isolate replies from the language isolate, and every widget callback — runs
on this one thread, where Win32 and the single-threaded Direct2D device are both
legal. Direct2D/DirectWrite factories are created `SINGLE_THREADED`; COM is
`APARTMENTTHREADED` (`win.rs:227`).

**One transliteration insight that `win.rs` alone does not give you:** `win.rs`'s
`WM_VM_DRAIN` handler (`win.rs:235-238`) has **no re-entrancy guard**, but
`cocoa_host.mm`'s `PumpPerform` does (`cocoa_host.mm:85-119`, `g_in_pump` /
`g_pending`). Windows needs the guard too, and for exactly AppKit's reason: a
**modal Win32 loop** — `MessageBox`, `IFileOpenDialog`, menu tracking, or the
`DefWindowProc` move/size loop — spins a nested `GetMessage` pump that will
dispatch our wake message **re-entrantly** while `Dart_HandleMessages()` is
already on the stack. So `win_host.cpp` takes `win.rs`'s machinery **and**
`cocoa_host.mm`'s `g_in_pump`/`g_pending` guard. This is the single most
important merge point of the two references.

---

## 1. `win_host.cpp` — the Win32 UI-thread host

Transliterates `cocoa_host.mm` (structure) over `win.rs` (Win32 calls). ~250 lines.

### 1.1 The seam (the C++ equivalent of `shell/mod.rs`)

`shell/mod.rs:31-74` documents a deliberately narrow boundary: the shell owns the
loop and calls **up** into `main.rs`; `main.rs` calls **down** for effects.
`win_host.h` mirrors this. The direction of control:

| concern | mac (`cocoa_host`) | win (`win_host`) | WINVM ref |
|---|---|---|---|
| own the loop | `[NSApp run]` | `GetMessageW`/`TranslateMessage`/`DispatchMessageW` | `win.rs:685-693` |
| window | (AppKit, in Dart) | `RegisterClassW` + `CreateWindowExW` | `win.rs:382-412` |
| worker→UI wake | `CFRunLoopSourceSignal`+`WakeUp` | `PostMessageW(WM_WINDART_WAKE)` | `win.rs:159-211` |
| drain isolate | `Dart_HandleMessages` in `PumpPerform` | `Dart_HandleMessages` in `OnWake` | `cocoa_host.mm:96-117` |
| host reload | `PerformUiReload` at pump top | `PerformUiReload` at pump top | `cocoa_host.mm:67-80` |
| menu | (in Dart via bridge) | `HMENU` + `WM_COMMAND` | `win.rs:302-355` |
| entry | `macdart_run_ui_host` | `windart_run_ui_host` | — |
| ready flag | `macdart_ui_ready` | `windart_ui_ready` | `cocoa_host.mm:43` |

**Up-calls** the WNDPROC makes (the `crate::on_*` set, `win.rs:235-273`):
`OnWake()`, `OnMenuCommand(id)`, `OnSize(hwnd)`, `OnSetFocus(hwnd)`,
`OnDestroy()`, plus per-surface routing which goes to `win_callbacks.cpp`.

**Down-calls / accessors** `win_natives`/`win_callbacks` make into the host:
`WinHostMainHwnd()` (reads the HWND atomic — see §1.3), `WinHostRequestUiReload()`.

### 1.2 Startup sequence (`windart_run_ui_host`)

Transliterated call sequence, citing both refs:

```cpp
extern "C" int windart_run_ui_host(void) {
  // 1. Process-wide prerequisites (win.rs:223-229). PER_MONITOR_AWARE_V2 so
  //    Windows does not bitmap-stretch our Direct2D output on a scaled monitor
  //    (would read as a blurry-font bug); APARTMENTTHREADED COM for D2D/DWrite,
  //    WIC, and IFileOpenDialog.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // S_FALSE == already-init == ok

  // 2. Register the window class + create the main workspace window
  //    (win.rs:382-412). Store the HWND in an atomic BEFORE anything can wake us.
  RegisterMainWindowClass();                 // lpfnWndProc = WndProc
  HWND main = CreateWindowExW(0, kClassName, L"WINDART",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1100, 800, nullptr, nullptr, GetModuleHandleW(nullptr),
                             nullptr);
  g_main_hwnd.store((intptr_t)main, std::memory_order_relaxed);   // win.rs:411
  ShowWindow(main, SW_SHOW);

  // 3. Route THIS (UI) isolate's message wakeups to our pump. Per-isolate; the
  //    language/compute isolates keep the VM's default off-thread scheduling.
  //    (cocoa_host.mm:150.)
  Dart_SetMessageNotifyCallback(&NotifyUi);

  // 4. Kick the first drain so the queued main() runs, then own the thread.
  //    (cocoa_host.mm:164-167 → PostMessage instead of CFRunLoopSource.)
  PostMessageW(main, WM_WINDART_WAKE, 0, 0);

  // 5. The pump (win.rs:685-693). Runs until WM_QUIT (posted by WM_DESTROY).
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {   // >0: 0=WM_QUIT, -1=error
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  Dart_SetMessageNotifyCallback(nullptr);          // cocoa_host.mm:169
  return 0;
}
```

Note vs. `win.rs`: no WebView2 environment/controller creation (`win.rs:435-503`)
— that whole nested-async startup is gone. We create native controls on demand
via the materializer instead.

### 1.3 The wake — `NotifyUi` and the HWND atomic (the ported bug-fix)

`win.rs:159-211` documents a real bug it hit and fixed: the worker is spawned
**before** the window exists, so a waker that captured the HWND at construction
would capture 0 and silently drop every wakeup forever. The fix: read the HWND
from an **atomic at notify time**. WINDART inherits the identical hazard — the VM
starts isolates before `windart_run_ui_host` creates the window — so we inherit
the fix verbatim:

```cpp
static std::atomic<intptr_t> g_main_hwnd{0};           // win.rs:95

// May run on ANY thread (IO event-handler thread, another isolate) — exactly
// cocoa_host.mm:121's contract. Only wakes; the drain happens on the pump thread.
static void NotifyUi(Dart_Isolate /*dest*/) {
  intptr_t h = g_main_hwnd.load(std::memory_order_relaxed);
  if (h == 0) return;                                  // window not up yet (win.rs:198)
  PostMessageW((HWND)h, WM_WINDART_WAKE, 0, 0);        // documented thread-safe (win.rs:202)
}
```

`WM_WINDART_WAKE = WM_APP + 1` (`win.rs:69`). `PostMessageW` is the one
documented-thread-safe entry into a window's queue — the same property
`performSelectorOnMainThread:` was chosen for on macOS (`shell/mod.rs:23`).

**Coalescing (optional, recommended):** each wake handler drains *all* pending
isolate messages, so redundant `WM_WINDART_WAKE` are cheap no-ops and the
baseline (post-per-notify, like `win.rs`) is correct as-is. If the queue depth
ever matters under a hot push loop, add single-outstanding backpressure with an
`std::atomic<bool> g_wake_posted` (the pattern the plan credits to WINVM's
`vm_host.rs`): `NotifyUi` posts only on a `false→true` exchange; `OnWake` stores
`false` before draining. Left out of the baseline skeleton to keep it faithful.

### 1.4 The drain — `OnWake` (= `PumpPerform`) with the re-entrancy guard

Faithful transliteration of `cocoa_host.mm:85-119`, including the fatal-before-
ready policy (`cocoa_host.mm:100-113`):

```cpp
static bool g_in_pump = false;   // cocoa_host.mm:28  — nested-loop guard
static bool g_pending = false;   // cocoa_host.mm:29  — don't lose a wake taken by a nested loop

static void OnWake(void) {
  if (g_in_pump) { g_pending = true; return; }   // re-entered by a modal/menu loop (§0)
  g_in_pump = true;
  do {
    g_pending = false;
    Dart_EnterScope();
    Dart_Handle r = Dart_HandleMessages();
    if (Dart_IsError(r)) {
      fprintf(stderr, "dartui: UI isolate error: %s\n", Dart_GetError(r));
      if (!g_ui_ready) {                            // never built a window → fatal
        fprintf(stderr, "dartui: the UI never started — restore last good source\n");
        Dart_ExitScope(); exit(70);
      }
      // after ready: a callback that throws is survivable (leak-over-crash)
    }
    Dart_ExitScope();
    if (g_ui_reload_requested) PerformUiReload();   // Dart off the stack — only safe moment
  } while (g_pending);
  g_in_pump = false;
}
```

### 1.5 Host-driven UI hot-reload (`cocoa_host.mm:31-80`)

The UI isolate cannot reload itself from its own stack (it would rewrite the
frames it stands on, with Win32 holding its callbacks). The host does it, at the
pump top, with no Dart frames live — the same flag-and-drain the modal case
needs. Ported unchanged; `Dart_WorkspaceReloadSources(true)` is atomic (a syntax
error is cancelled, running code untouched):

```cpp
extern "C" void windart_ui_ready(void)          { g_ui_ready = true; }          // :43
extern "C" void windart_request_ui_reload(void) {                               // :45
  g_ui_reload_requested = true;
  PostMessageW(WinHostMainHwnd(), WM_WINDART_WAKE, 0, 0);   // signal + wake
}
extern "C" const char* windart_take_ui_reload_status(void); // :55  ("ok"/"ERR: …"/NULL)
static void PerformUiReload(void);                          // :67  (Enter/Reload/Exit)
```

### 1.6 WNDPROC and menu bar

`WndProc` mirrors `win.rs:231-277`; the menu bar mirrors `win.rs:302-355`
(`CreateMenu`/`CreatePopupMenu`/`AppendMenuW`/`SetMenu`) with `WM_COMMAND` ids in
a fixed low block (`win.rs:79-89`). See `win_host.cpp` for the full body. The one
addition over `win.rs`: `WM_WINDART_WAKE → OnWake()`, and the surface routing
(`WM_COMMAND` from a materialized control, `WM_NOTIFY` from a list view) is
forwarded to `win_callbacks.cpp` rather than handled here.

---

## 2. `win_natives.cpp` — resolver, wire contract, op catalog

Keeps the **exact shape** of `cocoa_natives.mm` — the `X`-macro table
(`cocoa_natives.mm:516`, `COCOA_NATIVE_LIST`) and the `WinNativeLookup`
name+argc → `Dart_NativeFunction` resolver (`cocoa_natives.mm:568-583`). Only the
entries and bodies change. The resolver is wired into the builtin chain exactly
as cocoa's was (see §7).

### 2.1 The wire contract (preserved verbatim in shape, `WINDOWS_PORTING_PLAN.md` §5)

| concern | Cocoa | Windows | note |
|---|---|---|---|
| native object handle | `int64` id in a `Cocoa` wrapper | `int64` **ticket** in a `Win` wrapper | Dart never holds an `HWND`; it holds the materializer's stable widget-id/ticket |
| strings | `Dart_StringToCString` UTF-8, used directly | `Dart_StringToCString` UTF-8 → `MultiByteToWideChar` → UTF-16 for `…W` APIs | the one systematic add: widen at the boundary |
| rect / point | `List<num>` | `List<num>` `[x,y,w,h]` | **top-left origin, no flip** (Win32 child coords are already top-left; Cocoa needed the height-y flip — `APP_PANE_PLAN.md` §6). A genuine simplification. |
| bulk pixels | `Dart_NewExternalTypedData` over `MTLBuffer` | `Dart_NewExternalTypedData` over a WIC/D2D bitmap (S5) or D3D dynamic buffer (S6) | zero-copy, same shape |
| GC ownership | `Dart_NewWeakPersistentHandle` finalizer → `[obj release]` (`cocoa_natives.mm:119-124`) | finalizer → `DestroyWindow`/`delete`/COM `Release` | **but** widgets are owned by their surface and destroyed on `['remove']`/`['clear']`; the finalizer is the backstop for handle-only objects (bitmaps, dialogs) |

Helper skeletons carried over: `IntArg` (`cocoa_natives.mm:35-39`),
`DoubleFromDart` (`:75-79`); new `Utf16FromDart(Dart_Handle) -> std::wstring` at
the string boundary. The `Win` type is looked up + cached once
(`WinType()`, cf. `CocoaType()` `:84-93`); wrapping uses `Dart_Allocate` + field
set, not a named constructor, for the same hot-path reason
(`MakeCocoa`, `:145-157`).

### 2.2 The op catalog (the fixed catalog that replaces the dynamic send)

`cocoa_natives.mm`'s heart was `Cocoa_send` (`:195-306`) — a general `objc_msgSend`
driven by runtime `@encode`. **That is deleted.** In its place: a curated set of
concrete natives. This is the "delete the problem, don't solve it" move
(`WINDOWS_PORTING_PLAN.md` §3, §5). Enumerated below — **skeletons only**, bodies
land in S4/S5:

**Surface + materializer (the view-server core):**
- `Win_surfaceOpenPane(w, h) -> ticket` — a `WS_CHILD` HWND filling a region of
  the workspace window.
- `Win_surfaceOpenWindow(title, w, h) -> ticket` — a `WS_OVERLAPPEDWINDOW` HWND.
  The app cannot tell pane from window apart (`APP_PANE_PLAN.md` §2).
- `Win_surfaceClose(surface)`.
- `Win_surfaceApply(surface, gen, List cmds)` — **the batched materialize**, the
  Windows analog of `gpApply` (whole frame in one call, applied atomically,
  `WINDOWS_PORTING_PLAN.md` §6). `cmds` is the `['add'|'set'|'remove'|'place'|
  'title'|'focus'|'clear']` vocabulary (§4). This is where most of the S4 work
  lives.
- `Win_surfaceSize(surface) -> [w, h]`.

**Read-back / measurement (ops that need a synchronous return, so cannot be a
push command):**
- `Win_widgetText(ticket) -> String` — a field/editor's current text (the Dart
  `text`/`enter` handler pulls it; mirrors Cocoa's `onTextChange` handler reading
  `sender.stringValue`).
- `Win_measureText(str, fontSpec) -> [w, h]` — DirectWrite text metrics for
  language-side layout.

**Dialogs (Win32 modal — note each spins a nested loop, §0):**
- `Win_openFileDialog(optsJson) -> String` — `IFileOpenDialog`.
- `Win_saveFileDialog(optsJson) -> String` — `IFileSaveDialog`.
- `Win_messageBox(title, body, kind) -> int` — `MessageBoxW`.

**Editor (the hard widget, §4.7):**
- `Win_editorApplySpans(ticket, List runs)` — flat `[start,len,kind,…]` syntax
  runs, the direct port of `Cocoa_applySpans` (`cocoa_callbacks.mm:265-286`).
  RichEdit bring-up: `EM_SETCHARFORMAT` per run; Direct2D form: a DWrite color
  span table.

**Canvas / game (enumerated; bodies are S5/S6, out of S4 scope):**
- `Win_canvasBlit(ticket, ExternalTypedData px, w, h)` — Direct2D `WIC`/bitmap
  blit behind the unchanged demo draw protocol.
- `Win_gpOpen/Close/Apply/Snap/Stat/Fullscreen/Backbuffer` — Direct3D 11 game
  pane; **same 7-native shape** as `cocoa_natives.mm:502-508`, D3D bodies in S6.

**Reverse-callback + key-state** — declared here, defined in `win_callbacks.cpp`
(§3), listed in the table exactly as cocoa did (`cocoa_natives.mm:490-498`).

**Workspace primitives — reused as-is** (`workspace_natives.cc`, OS-neutral):
`Workspace_eval`, `Workspace_reload`, `Workspace_vmStats`,
`Workspace_requestUiReload`, `Workspace_uiReloadStatus`, `Workspace_uiReady`
(`cocoa_natives.mm:531-536`).

### 2.3 Why batched apply, not piecemeal sends

The dynamic bridge aborted the process on an unknown selector — bitten twice
(`APP_PANE_PLAN.md` §5). A batched, validated command list cannot reach an
illegal call: an unknown `kind` is rejected in C++ and reported, never crashes.
It also matches machinery that already exists on the wire (`['appui', surface,
gen, cmds]`, `APP_PANE_PLAN.md` §4) and the demo/game whole-frame discipline. The
UI isolate stays dumb; layout (`grid`/`row`/`column`) is language-side arithmetic
producing absolute frames (`APP_PANE_PLAN.md` §6), so the materializer never does
layout — it only creates, places, sets, and destroys.

---

## 3. `win_callbacks.cpp` — WNDPROC/subclass → ticket → Dart

Ports the ticket design of `cocoa_callbacks.mm` **directly** — the plan calls this
out as a direct map onto `WNDPROC`/`SetWindowSubclass`
(`WINDOWS_PORTING_PLAN.md` §5). The invariants are preserved:

- The native side stores only an **integer ticket**, never a Dart handle
  (`cocoa_callbacks.mm:6-8`, `:30`).
- One Dart closure `_winDispatch(ticket, kind, arg)` is the single funnel
  (`cocoa_callbacks.mm:23-50`, `Dart_InvokeClosure`).
- A **dead ticket fails closed** (`cocoa_callbacks.mm:40`).

### 3.1 The Windows mapping: the control-id *is* the ticket

Cocoa needed a synthesized `MacdartActionTarget` class whose IMPs are the
target/delegate/data-source (`cocoa_callbacks.mm:158-175`). Win32 is simpler:
controls notify their **parent** (the surface window) via `WM_COMMAND` /
`WM_NOTIFY`, and the child-window **control id** is chosen by us at
`CreateWindowExW` time. So we assign **control-id = ticket**, and the surface
WNDPROC is the single dispatch hub — no synthesized class, no per-control target
object.

```cpp
// control-id (== ticket) -> its HWND and widget kind, for routing + read-back.
static std::unordered_map<int64_t, WidgetRef> g_widgets;   // cf. g_ticket_of (:30)
static Dart_PersistentHandle g_dispatch = nullptr;         // cf. :27

static Dart_Handle Dispatch(int64_t ticket, int kind, int64_t arg) {  // cf. :35-50
  if (g_dispatch == nullptr) return nullptr;
  Dart_Handle fn = Dart_HandleFromPersistent(g_dispatch);
  Dart_Handle a[3] = { Dart_NewInteger(ticket), Dart_NewInteger(kind),
                       Dart_NewInteger(arg) };
  return Dart_InvokeClosure(fn, 3, a);
}
```

The surface WNDPROC (installed by the materializer on every surface) routes:

| Win32 message | control | kind | arg | Cocoa analog |
|---|---|---|---|---|
| `WM_COMMAND` + `BN_CLICKED` | button | 0 action | 0 | `ActionIMP` (:53) |
| `WM_COMMAND` + `BN_CLICKED` | checkbox | 6 toggle | checked 0/1 | — |
| `WM_COMMAND` + `EN_CHANGE` | edit field | 1 text | 0 (handler pulls via `Win_widgetText`) | `ControlTextDidChangeIMP` (:81) |
| `WM_NOTIFY` + `LVN_GETDISPINFO` | list (owner-data) | 3 cell | row | `ObjectValueIMP` (:97) |
| `WM_NOTIFY` + `LVN_ODCACHEHINT`/count set | list | 2 rowCount | 0 | `NumRowsIMP` (:86) |
| `WM_NOTIFY` + `LVN_ITEMCHANGED` | list | 4 select | row | `SelectionChangedIMP` (:113) |
| subclass `WM_KEYDOWN` `VK_RETURN` | edit field | 5 enter | 0 | (field enter) |
| `WM_CLOSE` | window surface | 8 close | 0 | (window close) |
| `WM_SIZE` (debounced) | surface | 7 resize | packed w,h | (resize) |

The dispatcher **return value** is marshaled per kind, exactly as cocoa
(`cocoa_callbacks.mm` `NumRowsIMP`/`ObjectValueIMP`): `void` for 0/1/4/5/6/7/8,
an `int` for 2 (row count), a `String` for 3 (cell text, widened to UTF-16 for
the `LVN_GETDISPINFO` `pszText`). List views use **`LVS_OWNERDATA`** (virtual) so
the data source is pull-based, matching `NSTableView`'s data source model.

`SetWindowSubclass` (`comctl32`) is used only where the parent-routed model is
insufficient: field **Enter** detection (kind 5), and custom Direct2D widgets
(their own `WM_PAINT`/`WM_LBUTTONDOWN` → hit-test → kind 0). The ticket rides in
the subclass `dwRefData`.

### 3.2 `disposeCallbacks` and the rebuild-ticket trap

`cocoa.dart:disposeCallbacks` (`:156-158`) clears the handler map so stale
tickets fail closed after a teardown. The Windows equivalent clears `g_widgets`
**and** destroys the child HWNDs (Win32 controls are owned, unlike AppKit's
weakly-held targets). The `rebuildUi` trap (`APP_PANE_PLAN.md` §7) — a pop-out
window surviving a teardown with dead buttons — applies identically; the fix is
the same "re-materialise every surface after rebuild" spec-replay path.

### 3.3 Key-state — poll, not event stream (`cocoa_callbacks.mm:288-346`)

The model is unchanged: games read *which keys are down at frame time*, polled
once per frame with the pull tick, so no event queue can back up
(`cocoa_callbacks.mm:288-297`). Cocoa needed an `NSEvent` local monitor writing a
bitset (`:303-323`). **Windows is simpler: `GetAsyncKeyState` is already a poll**,
so `Win_keyState` reads it directly — no monitor, no bitset to maintain:

```cpp
void Win_keyState(Dart_NativeArguments args) {          // cf. Cocoa_keyState (:334)
  // Return [downVks, modifierMask]. Poll the game-relevant VKs.
  // GetAsyncKeyState high bit (0x8000) == currently down.
}
void Win_keyCapture(Dart_NativeArguments args);         // cf. :325 — see below
void Win_keyWatch(Dart_NativeArguments args);           // cf. :303 — no-op / idempotent init
```

Two real deltas to flag for the coder:

1. **Keycode namespace.** The Dart game code today uses **macOS virtual
   keycodes** (`cocoa.dart:222`: left 123, right 124, down 125, up 126, space 49,
   A 0, D 2). Windows uses **VK_ codes** (`VK_LEFT 0x25`, `VK_SPACE 0x20`, …). The
   port must remap — either a translation table in `Win_keyState` that emits the
   macOS codes the Dart expects (least Dart churn), or update the Dart game
   constants. Recommend the C-side table so portable game Dart stays unchanged.
2. **Capture** (swallow keys so a running game owns the keyboard, but let
   Alt/Ctrl shortcuts through — `cocoa_callbacks.mm:317-321`). With no event
   monitor, capture becomes: the game-pane child window's WNDPROC eats
   `WM_KEYDOWN`/`WM_KEYUP`/`WM_CHAR` (returns 0) while capture is on, except when
   `GetKeyState(VK_MENU/VK_CONTROL)` is held. `Win_keyCapture` just flips a flag
   the game-pane WNDPROC reads.

---

## 4. The view-server widget protocol

The message vocabulary, mapped onto Win32 controls / Direct2D. This is the
protocol the materializer (`Win_surfaceApply`, §2.2) implements and `win.dart`
(§5) speaks. It **is** the `APP_PANE_PLAN.md` wire (`§4`) — reused wholesale.

### 4.1 Surfaces

`'win'` = top-level `WS_OVERLAPPEDWINDOW`; `'pane'` = `WS_CHILD` embedded in the
workspace window. Byte-identical `build(ui)` either way; only `ui.width/height`
differ (`APP_PANE_PLAN.md` §2). The envelope names its surface; individual
commands do not (`APP_PANE_PLAN.md` §2, §4).

### 4.2 Commands (app → UI, batched in `Win_surfaceApply(surface, gen, cmds)`)

```
['clear']                        DestroyWindow all children; clear registry
['add',    kind, id, props]      CreateWindowExW the control; control-id = ticket;
                                 record (surface,id)->(hwnd,ticket); subclass if needed
['set',    id, props]            SetWindowTextW / state / enable / items / value
['remove', id]                   DestroyWindow; forget the ticket (fails closed after)
['place',  id, [x,y,w,h]]        MoveWindow / SetWindowPos  (top-left; no flip)
['title',  text]                 SetWindowTextW on the surface caption
['focus',  id]                   SetFocus(hwnd)
```

`props` keys: `text`, `title`, `align` (`left`/`center`/`right`), `readOnly`,
`enabled`, `checked`, `items` (popup/list), `value` (slider/progress), `font`,
`color`, `rows` (list owner-data count).

### 4.3 Events (UI → app) — the `_winDispatch` kinds

`0` click · `1` text · `2` rowCount · `3` cell(row) · `4` select(row) · `5` enter
· `6` toggle(checked) · `7` resize(w,h) · `8` close. (§3.1 for the message
mapping; kinds 0-4 line up 1:1 with `cocoa_callbacks.mm`'s dispatch kinds.)

### 4.4 Widget → backing map

| kind | Win32 control (bring-up) | Direct2D form (finished) | proven-in-cocoa analog |
|---|---|---|---|
| `label` | `STATIC` `SS_LEFT` | DWrite `DrawText` | `label()` (`APP_PANE_PLAN.md` §5) |
| `field` | `EDIT` (`ES_AUTOHSCROLL`, `ES_READONLY`, `ES_RIGHT`) | D2D custom | `NSTextField` editable |
| `button` | `BUTTON` `BS_PUSHBUTTON` | D2D custom + hit-test | `NSButton` |
| `checkbox`/`radio` | `BUTTON` `BS_AUTOCHECKBOX`/`BS_AUTORADIOBUTTON` | D2D | (probed set) |
| `popup` | `COMBOBOX` `CBS_DROPDOWNLIST` | D2D dropdown | `NSPopUpButton` |
| `list` | `SysListView32` `LVS_REPORT\|LVS_OWNERDATA` | D2D virtualized | `NSTableView` + `onTable` |
| `text` (editor) | **RichEdit** `MSFTEDIT_CLASS` | **D2D/DWrite custom** ← hardest | `NSTextView`+`NSTextStorage` |
| `box` | `BUTTON` `BS_GROUPBOX` | D2D panel | `NSBox` |
| `image` | `STATIC` `SS_BITMAP` | D2D bitmap | `NSImageView` |
| `canvas` | child HWND + D2D RT + `Win_canvasBlit` | (same) | demo `Pixmap` blit |
| `tabs` | `SysTabControl32` `WC_TABCONTROL` | D2D tab strip | `NSTabView` |
| `slider` | `msctls_trackbar32` | D2D | (probed) |
| `progress` | `msctls_progress32` | D2D | (probed) |
| menus | `HMENU` + `WM_COMMAND` (host, `win.rs:302-355`) | — | `NSMenu` |

**Bring-up vs finished:** stand common-controls up first (fast to a working
window, `SPRINTS.md` §S4 exit = "a window with a button whose action is a Dart
closure"), migrate to Direct2D custom-drawn widgets where the pixel-identical IDE
look demands it. Both are served by the one materializer behind the one protocol.

### 4.5 Coordinates & layout

Absolute `[x,y,w,h]` frames, **top-left origin, no flip** (Win32 child coords are
already top-left — `APP_PANE_PLAN.md` §6 wanted top-left and Cocoa had to convert;
here it is free). `grid`/`row`/`column` are pure Dart arithmetic in the
language-side proxy producing absolute frames; the materializer never lays out.

### 4.6 Menus

Host-level `HMENU` + `WM_COMMAND`, transliterated from `win.rs:302-355`
(`CreateMenu`/`CreatePopupMenu`/`AppendMenuW`/`SetMenu`/`DrawMenuBar`), ids in a
fixed low block. Menu-item actions reach the focused editor via the standard
command ids the host dispatches (`win.rs:279-297`), replacing Cocoa's
responder-chain `setSelectorAction` trick (`cocoa_callbacks.mm:218-229`) — which
was only needed because a SEL cannot be built from Dart; Win32 has no such
constraint.

### 4.7 The editor — the single hardest widget

Verdict from `WINDOWS_PORTING_PLAN.md` §5: **RichEdit for bring-up, Direct2D/
DirectWrite for the finished form.** Cocoa colored spans with
`NSTextStorage addAttribute:` batched under `beginEditing/endEditing`
(`cocoa_callbacks.mm:265-286`). Options in preference order:
- **(b) RichEdit** (`MSFTEDIT_CLASS`): `EM_SETCHARFORMAT` per run,
  `EM_EXSETSEL`. Fastest to working, least control. **Recommended for S4/S7
  bring-up.**
- **(a) Direct2D/DirectWrite custom view**: full control, matches "pure DirectX",
  most work. The finished form.
- **(c) Scintilla** (BSD): a real code editor, pragmatic middle. Fallback if (a)
  slips.

`Win_editorApplySpans` (§2.2) presents the identical Dart-facing contract as
`applySpans` (`cocoa.dart:282`) regardless of backing, so the editor backend can
be swapped without touching Dart.

---

## 5. `win.dart` — the Dart-facing API

Mirrors `cocoa.dart`'s **shape** but **drops the dynamic selector bridge**:
- **Deleted:** the `Cocoa` class's `noSuchMethod` → `_send` machinery
  (`cocoa.dart:326-387`), `import 'dart:mirrors'`, `_send`/`Cocoa_send`, the
  `@encode` classifier, and the Accept-time selector-lint natives
  (`_cocoaClassExists`/`_cocoaSelectorInfo`/`_cocoaNearestSelectors`,
  `cocoa.dart:62-78`) — there is no selector to misspell.
- **Kept in shape:** the `native "…"` extern declarations bound by
  `WinNativeLookup`; the ticket-keyed callback registry (`cocoa.dart:122-193`);
  `keyWatch`/`keyCapture`/`keyState` (`:207-223`); the reused workspace primitives
  (`wsEval`/`wsReload`/`wsVmStats`/`wsRequestUiReload`/`wsUiReloadStatus`/
  `wsUiReady`, `cocoa.dart:20-52`); the `Db` SQLite wrapper (`:294-304`, natives
  reused as-is).
- **New:** a thin `Ui` client (`surface`, `apply(cmds)`, `set`, `title`, `focus`)
  — the view-server client side. The `build(ui)` API from `APP_PANE_PLAN.md` §3
  is unchanged; only its backing native (`Win_surfaceApply`) differs.

The single dispatcher `_winDispatch(ticket, kind, arg)` (`win.dart`) is the exact
counterpart of `_cocoaDispatch` (`cocoa.dart:133-143`), extended with kinds 5-8.
See `win.dart` for the full sketch.

---

## 6. The `DART_UI_HOST` hook (`bin/main.cc`) and the resolver chain

Regenerate `windart-port.patch` from `macdart-port.patch` by swapping
`cocoa`→`win` in the OS-neutral embedder hunks (`S1_NOTES.md` defers these to
S2/S3; S4 needs the UI-host + `dart:win` registration ones live). The four hunks:

1. **`bin/main.cc` — the host hook** (`macdart-port.patch:213-246`): under
   `#if defined(DART_UI_HOST)`, replace `Dart_RunLoop()` with
   `windart_run_ui_host()`; declare `extern "C" int windart_run_ui_host(void);`.
   Verbatim except the symbol name. (The `--compiler_stats` add at
   `:250-261` carries over so the workspace toolbar's JIT counters are non-zero.)
2. **`bin/builtin_natives.cc` — the resolver chain** (`:125-154`): after
   `IONativeLookup` returns NULL, fall through to `WinNativeLookup`
   (`#include "win_natives.h"`); symmetric `WinNativeSymbol` fallback.
3. **`bin/builtin.cc` / `builtin_nolib.cc` / `dartutils.{h,cc}` — library
   registration** (`:114-187`): register `kWinLibURL = "dart:win"`, its source
   paths, and `SetNativeResolver(kWinLibrary)` alongside `dart:io`
   (`:188-195`). `gen_snapshot.cc` loads `dart:win` into the snapshot isolate
   (`:199-208`).
4. **`runtime/lib/invocation_mirror_patch.dart`** — the source-order named-args
   fix was only load-bearing for the `noSuchMethod` selector bridge; with that
   deleted, **it can be dropped** for WINDART (no C++ impact either way — noted so
   the coder does not chase it).

`DART_UI_HOST` gates the `dartui.exe` target (links `win_host`/`win_natives`/
`win_callbacks`/`win_view` + `dart_win32`, per `WINDOWS_PORTING_PLAN.md` §4's lib
list: `d2d1 dwrite windowscodecs d3d11 dxgi d3dcompiler xaudio2 comctl32 dcomp`);
plain `dart.exe` (S2) omits it and keeps `Dart_RunLoop()`.

---

## 7. Risks & open questions (for the architect)

### Mechanical (low risk — a transliteration with a known-good reference)
- **The pump, wake, window, menu, clipboard, DPI/COM.** `win.rs` is a *shipping*
  Win32 host; these are line-for-line ports (§1). The only non-obvious merge is
  adding `cocoa_host.mm`'s re-entrancy guard to `win.rs`'s wake handler (§0) — a
  named, understood delta, not an unknown.
- **The ticket dispatcher.** `control-id = ticket` makes the Cocoa design *simpler*
  on Windows (no synthesized target class, parent-routed `WM_COMMAND`/`WM_NOTIFY`)
  — §3. Low risk.
- **The wire contract.** Handles/strings/structs/ExternalTypedData port directly;
  the only systematic add is UTF-8→UTF-16 widening at the boundary, and the
  coordinate flip *goes away* (§2.1).
- **Common-controls widget bring-up** (button/label/field/list/popup/tabs). Stock
  Win32; the S4 exit milestone (button → Dart closure) is a small slice of this.

### Genuinely hard — the three unknowns to watch
1. **The syntax-highlighting code editor (§4.7).** The crux, same as the mac port
   (`WINDOWS_PORTING_PLAN.md` §9 risk #1). RichEdit gets a working editor fast but
   its span/selection/undo model is quirky and its look is not the IDE's; the
   Direct2D/DirectWrite custom view is a substantial build (text layout, caret,
   selection, scrolling, IME, hit-testing) and is where "pure DirectX" is
   cashed. **Open:** commit to RichEdit through S7 and defer the D2D editor to
   polish, or build the D2D editor once and skip the RichEdit throwaway? Staging
   RichEdit→D2D is the recommendation; the `Win_editorApplySpans` contract (§2.2)
   makes the swap invisible to Dart.
2. **The materializer's layout/ownership logic — the new C++ surface with no
   reference to lift.** `win_host.cpp` transliterates `win.rs`; `win_callbacks`
   transliterates `cocoa_callbacks`. But the **materializer** (`Win_surfaceApply` +
   `win_view.h` registry: create/place/set/remove/clear over `(surface,id)`
   tuples, the `LVS_OWNERDATA` data-source plumbing, subclass lifetime, the
   `rebuildUi` re-materialise path from `APP_PANE_PLAN.md` §7) is **net-new C++**
   — WINVM materialized nothing (it was WebView2, `WINDOWS_PORTING_PLAN.md` §7).
   Bounded and well-specified by the protocol (§4), but it is the largest piece of
   *original* code in S4, and the `rebuildUi` ticket trap (dead buttons on a
   surviving pop-out window) is a real correctness hazard the mac plan already
   flagged.
3. **Direct2D custom-drawn controls vs. common controls — the visual-fidelity
   fork.** Bring-up on common controls is cheap, but the IDE chrome's look
   (`workspace.dart`, 4,853 LOC) was tuned against AppKit; matching it may force
   Direct2D custom widgets sooner than "polish." **Open:** which widgets *must* be
   custom-drawn for the chrome to look right (likely the editor, tabs, and list
   rows), and which can ship as themed common controls indefinitely? This decides
   how much of the D2D widget toolkit S4 must front-load vs. defer to S7.

### Smaller open questions
- **Keycode namespace** (§3.3): remap VK→macOS codes C-side (keep game Dart
  unchanged) vs. update the Dart constants. Recommend C-side table.
- **Wake coalescing** (§1.3): ship post-per-notify (faithful to `win.rs`) or add
  `vm_host.rs`-style single-outstanding backpressure now? Recommend defer until a
  hot push loop proves it necessary.
- **`disposeCallbacks` ownership delta** (§3.2): Win32 controls are *owned*
  (must `DestroyWindow`), AppKit targets were weakly held — the teardown must
  destroy HWNDs, not just clear the ticket map. Easy to get subtly wrong.
- **List data-source model:** `LVS_OWNERDATA` (virtual, pull, matches
  `NSTableView`) vs. pushing rows. Owner-data is the faithful port; confirm it
  interacts cleanly with the batched `apply`.
