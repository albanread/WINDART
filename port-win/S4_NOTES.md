# WINDART Sprint S4 — GUI host vertical slice (`dartui.exe` + `dart:win`)

Goal (iteration 1): the thinnest slice that proves the whole framework —
`dartui.exe` opens a window, materializes a **button** from a Dart describe call,
and a click round-trips to a Dart closure. Built on S1-S3 (JIT + dart:io +
reload API). Design: `gui-design/S4_GUI_HOST_DESIGN.md` (+ 8 skeletons), decisions
`arch-notes/S4_design_decisions.md`.

## MILESTONE — HIT (headless, captured)
```
> set WINDART_SELFTEST=1 & dartui.exe e:\windart\test\dartui_button_test.dart
  stdout:
dartui: surface materialized (button ticket=257)
BUTTON CLICKED (ticket=257)
  stderr:
dartui: [selftest] synthesizing BM_CLICK on the button
```
Full round-trip proven with no human: `dartui.exe` opened the workspace window →
the UI isolate's `main()` described a button via `dart:win` → the **materializer**
(`ViewServer::Apply`) created a real Win32 `BUTTON` control with **control-id ==
ticket (257)** → the self-test synthesized `BM_CLICK` → `WM_COMMAND`/`BN_CLICKED`
→ `OnSurfaceCommand` → `Dispatch(257,0,0)` → `_winDispatch` → the Dart `onClick`
closure printed. The ticket matches end-to-end; the process exited cleanly via
`PostQuitMessage`. JIT active (compiler stats: 409 functions compiled — the
`--compiler_stats` DART_UI_HOST hunk).

## What was created (owned)
- **`port-win/dart_win32/`** — the `dart_win32` lib sources. Skeletons from
  `gui-design/` filled in; the one NET-NEW file is the materializer:
  - **`win_view.cpp`** (NEW) — `ViewServer`: OpenPane/OpenWindow, the batched
    `Apply` verb-decoder (`clear|add|place|set|remove|title|focus`), the
    `(surface,id)<->(hwnd,ticket)` registry, `WidgetByTicket`/`SurfaceByHost`,
    `ClearSurface` (destroys OWNED HWNDs, not just the map — the §3.2 gate).
    S4 scope: **button + label** materialize; other kinds parse then report
    "unsupported (S4 slice)" — never an illegal Win32 call.
  - **`windart_gui_stubs.cc`** (NEW) — `Workspace_*`/`Sqlite_*` native stubs so
    the resolver table links (real bodies = S7). `Workspace_uiReady/requestUiReload/
    uiReloadStatus` forward to the host (live).
  - `win_host.cpp` — filled: exposed `WinHostWindowClassName()`, added
    `InitCommonControlsEx`, `<objbase.h>` (CoInitializeEx under WIN32_LEAN_AND_MEAN),
    and the **headless self-click** (`WINDART_SELFTEST` → one-shot timer →
    EnumChildWindows finds the BUTTON → `BM_CLICK` → clean `WM_CLOSE`).
  - `win_natives.cpp`, `win_callbacks.cpp`, `win_view.h`, `win_host.h`,
    `win_callbacks.h`, `win.dart` — from the design; win.dart wired (active Ui +
    dispatch on construct, ticket carried in the `add` command, `ticketOf`/`uiReady`).
- **`test/dartui_button_test.dart`** — the slice app (one label + one button).
- **CMake:** `dart_win32` static lib (+ generated `win_gen.cc` from win.dart) and
  the **`dartui.exe`** target (dart.exe recipe + `DART_UI_HOST` + `dart_win32`).

## Embedder patch — 8 cocoa->win hunks (in windart-port.patch, applied to tree)
1. `bin/main.cc` — under `DART_UI_HOST`, `windart_run_ui_host()` replaces
   `Dart_RunLoop()`; `extern "C"` decl; `--compiler_stats`.
2. `bin/builtin.h` — `kWinLibrary` enum + `win_source_paths_` decl.
3. `bin/builtin.cc` / `builtin_nolib.cc` — the `dart:win` table entry.
4. `bin/builtin_natives.cc` — `WinNativeLookup`/`WinNativeSymbol` fallthrough.
5. `bin/dartutils.{h,cc}` — `kWinLibURL="dart:win"` + load + `SetNativeResolver`.
6. `bin/gen_snapshot.cc` — load `dart:win` into the snapshot isolate.
`invocation_mirror_patch.dart` DROPPED (it only served the deleted selector bridge).

## Integration decision (why dart_win32 links into every binary)
The `dart:win` registration is **unconditional** (the cocoa parity path): the
enum-index coupling (`kWinLibrary` == table index) and the resolver setup living
in `dartutils.cc` (part of the shared `dart_builtin` lib, compiled without
`DART_UI_HOST`) make a guarded "dartui-only" wiring fragile. So — exactly as the
mac port linked `dart_cocoa` everywhere — `dart_win32` is linked into
`dart_bootstrap`/`dart`/`gen_snapshot`/`dartui`, and `dart:win` is loaded in all
of them. Only **dartui** defines `DART_UI_HOST` (→ runs the Win32 pump); the
headless binaries load dart:win but never create a window. **Regression
confirmed:** `dart.exe hello.dart` still prints `hello, windart` / `sum=5050`.
*Future cleanup (non-blocking):* keep the GUI lib out of the headless binaries.

## Decisions applied (from arch-notes)
- Editor: RichEdit-first (deferred; not in the slice). Common controls for bring-up
  (BUTTON/STATIC now; EDIT/COMBOBOX/SysListView32/... additive). D2D custom widgets
  deferred to S7. Keycode VK->macOS remap table C-side (in win_callbacks.cpp).
  Wake coalescing deferred (faithful post-per-notify). `LVS_OWNERDATA` list.
- **Re-entrancy guard PRESENT:** `win_host.cpp`'s `OnWake` carries
  `cocoa_host.mm`'s `g_in_pump`/`g_pending` guard (the one merge `win.rs` lacks),
  so a modal Win32 loop re-entering the wake won't nest `Dart_HandleMessages`.

## Compile burn-down
Iteration 1: **2 errors** — `CoInitializeEx`/`COINIT_APARTMENTTHREADED` undeclared
(WIN32_LEAN_AND_MEAN omits COM). Fix: `#include <objbase.h>` in win_host.cpp.
Iteration 2: **0 errors** — dart_win32 + dartui + regenerated snapshot all built.

## What's stubbed / deferred
- Widget kinds beyond button+label (field/checkbox/popup/list/editor/box/image/
  canvas/tabs/slider/progress) — materialize returns "unsupported (S4 slice)".
- `Win_measureText`, dialogs (`IFileOpenDialog`/`MessageBox` — MessageBox is
  actually wired), `Win_editorApplySpans`, canvas blit (S5), game pane `Win_gp*`
  (S6 — a parallel agent is prepping MSL->HLSL; left stubbed).
- `Workspace_eval/reload/vmStats` + `Sqlite_*` — inert stubs (S7).
- Pane-as-WS_CHILD region: the slice's `OpenPane` host is the main window itself
  (button is its direct child); a dedicated child host per pane is a refinement.

## Reproducibility
`extract.py` now applies **all** WINDART source edits from the single
`port-win/windart-port.patch` (11 files) via GNU `patch` (git-apply mangles
absolute Windows `--directory` paths). Verified: full clean rebuild from a freshly
`extract.py`-regenerated tree links + runs the milestone. Quarry stays pristine.

## S5/S6 hand-off
- Framework is live: describe->materialize->event->closure works. Adding a widget
  kind = a `case` in `ViewServer` + a `win.dart` method (additive, per the design).
- S5 (Direct2D canvas): implement `Win_canvasBlit` + a canvas surface (D2D RT).
- S6 (D3D11 game pane): the 7 `Win_gp*` natives (stubbed) + key-state (wired).
