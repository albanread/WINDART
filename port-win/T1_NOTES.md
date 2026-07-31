# WINDART T1 — the consolidated interactive workspace IDE (working tabs)

Goal (user's #1 priority): ONE persistent dartui workspace application whose tab
strip switches content — not a set of separate demo apps. Consolidates
workspace_live (Workspace), workspace_shell (Browser), workspace_editor/morph
(Editor) into `test/workspace.dart`.

## MILESTONE — HIT (four per-tab PNGs in e:\windart\build\)
`dartui.exe workspace.dart` opens ONE window; clicking a tab **switches its
content** (the tab strip + chrome persist). Verified headlessly (`selftest` arg
drives each tab + snapshots):
- **tab_workspace.png** — the editor (`(2 + 3) * 7`) + **Do It** button + Output
  pane showing `(2 + 3) * 7  =>  35` (live `Dart_EvaluateExpr`).
- **tab_browser.png** — Classes (329) list -> select `Float32x4` -> Members (291,
  `fn abs`/`get signMask`/…) -> syntax-highlighted Source sketch. Live navigation
  via the list-select (kind-4) dispatch.
- **tab_editor.png** — a user class (`Counter`) source, syntax-highlighted, +
  **Accept** (persists to the SQLite image, hot-reloads) + status.
- **tab_vm.png** — live `Dart_WorkspaceVmStats` counters (heap new/old used+cap,
  scavenges, mark-sweeps, functions compiled 840 / optimized 35, code bytes
  463667), refreshed on a 1s Timer.
Find/Docs/App/Help are placeholders; Debug is a placeholder (deferred to T4).
**The app STAYS OPEN** (event-driven; no hostQuit) unless run with `selftest`.

## The tab-switch mechanism (clear + re-describe)
`tabs.onSelect(i)` (kind 4, TCN_SELCHANGE) -> `buildTab(i)`: `clearContent()`
removes the tracked content-widget ids (`ui.remove` each), then the tab's
`buildX()` re-adds its widgets — the tab strip stays. Programmatic switch (the
self-test) also sets the strip via `ui.set('tabs', {'tab': i})` -> `TCM_SETCURSEL`
(new `DoSet` case; TCM_SETCURSEL doesn't fire TCN_SELCHANGE, so the caller
rebuilds). Fixed pane frames, computed language-side — the plan's bring-up model.

## Three bugs found + fixed (all in dart_win32, no quarry edits)
1. **RichEdit re-entrancy crash** (`allocation.cc:37 top==this`). A `text`/RichEdit
   created WITH text + `ENM_CHANGE` fires `EN_CHANGE` synchronously **during**
   `Win_surfaceApply`, re-entering `OnSurfaceCommand` -> `Dart_EnterScope` inside
   another native's execution, corrupting the StackResource stack. Fix: don't set
   `ENM_CHANGE` on the editor (live onText must be a deferred, not re-entrant,
   dispatch — the plan's `defer` pattern).
2. **mirrors + a from-source library crash** (same assert, at load). Statically
   importing a source-loaded user library (counter_scratch.dart) alongside
   `dart:mirrors` trips the VM (reflecting over a non-snapshot library). Fix: the
   workspace app (needs mirrors for the Browser) does NOT import the scratch; the
   live-instance-morph proof stays in the standalone `workspace_morph.dart` (S7.3),
   and T1's Editor tab does persist + Accept-reload. (VM interaction worth a
   follow-up; a separate language isolate is the clean answer, per the plan.)
3. **Stale pixels on tab-switch.** Removing a child left its pixels behind (the
   window class had no background brush; the parent lacked `WS_CLIPCHILDREN`).
   Fix: `wc.hbrBackground = COLOR_WINDOW+1` + `WS_CLIPCHILDREN` on the main window
   — the brush erases the *revealed* gaps only, children keep their content.

## What was added (owned)
- `win_view.cpp` — `DoSet` tab-select (TCM_SETCURSEL); the `text`/RichEdit kind
  already existed (S7.2). `win_host.cpp` — bg brush + WS_CLIPCHILDREN.
- `test/workspace.dart` — the consolidated app (tab switching + all 4 tabs).
- No quarry edits; windart-port.patch unchanged.

## Verification
0 errors. Interactive: the app stays open. Regressions clean: dart.exe
`hello,windart`; workspace shell 53614b; workspace editor + Do-It; lissajous
46687b; Pong 3247b; dartui button ticket=257. Quarry git-clean.

## Next (user priority order): T3 (game-pane) then T4 (debugger)
Also open: deepen Find/Docs/App tabs; a live editor-onText highlight via a
deferred dispatch; the separate-language-isolate model so the Editor tab's Accept
morphs live instances in-app (mirrors + scratch coexist).
