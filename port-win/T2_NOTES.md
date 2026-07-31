# WINDART T2 — finishing the remaining workspace tabs

Goal (user's #1 priority, continued from T1): the user wants the tabs FINISHED
before the game pane. T1 wired Workspace/Browser/Editor/VM and left Find/Docs/
App/Help as placeholders. T2 makes those four **functional** inside the SAME one
persistent `dartui.exe` app (`test/workspace.dart`). Debug stays a placeholder
(deferred to T4, after the game pane T3).

## MILESTONE — HIT (eight per-tab PNGs in e:\windart\build\)
`dartui.exe workspace.dart` opens ONE window; every tab now has real content.
Verified headlessly (`selftest` arg drives each tab + snapshots). The four new:
- **tab_find.png** — a query field + **Find** button; typing `Codec` yields
  **20 matches** (classes + `Class.member`) in a virtual list; clicking a result
  jumps to the Browser and selects that class. (`doFind` filters `classNames` +
  each class's mirror declarations, capped at 500.)
- **tab_app.png** — a **live user app (Calculator)** materialized in the app pane:
  a 4x4 keypad + `C`, a right-aligned read-only display. Its buttons are 0-arg
  Dart closures that do the arithmetic and `ui.set` the display; `_winDispatch`
  auto-commits. Snapshot shows the display reading **12** after a synthetic
  `7 + 5 =`. The Calculator is **inlined** (not imported — a source-loaded library
  next to `dart:mirrors` crashes the VM, see T1) and uses ONLY the `Ui` API.
- **tab_docs.png** — the VM class reference: a class list -> a syntax-highlighted
  member sketch (e.g. `Duration`, 38 members) built from the same mirror data.
- **tab_help.png** — a usage/keybindings panel (TABS + GESTURES) in a read-only
  editor.
Plus the four T1 tabs still green: tab_workspace (Do-It => 35), tab_browser
(Float32x4, 291 members), tab_editor (Counter + Accept), tab_vm (live counters).
**The app STAYS OPEN** (event-driven) unless run with `selftest` — re-verified:
launched without the arg, pid still alive after 3s, no self-exit.

## Two bugs found + fixed (both in dart_win32/win_view.cpp, no quarry edits)
1. **Double frames decoded to 0 (invisible widgets).** The Calculator lays out
   with `<double>` frames (grid arithmetic yields doubles); `DartInt` only read
   `Dart_IsInteger`, so every `place` x/y/w/h decoded to 0 -> widgets stacked at
   the origin with zero size. Fix: `DartInt` now also accepts `Dart_IsDouble`
   (rounds to the nearest pixel). General win — any app whose layout math produces
   doubles now places correctly, not just the Calculator.
2. **App-pane snapshot caught mid-creation.** Building the 17-button keypad and
   snapshotting in the SAME synchronous timer tick captured only the buttons that
   had already painted — freshly-created controls that had not yet processed a
   `WM_PAINT` came out blank/absent. This is a snapshot-timing artifact only
   (interactively the message loop repaints immediately; a user never sees it).
   Fix (self-test): build the keypad in tick N, do the presses + snapshot in tick
   N+1, so a full message-loop cycle repaints every control first. With the split,
   all 17 keys render and the display reads 12.

## What was added (owned)
- `win_view.cpp` — `DartInt` accepts doubles (the fix above). The T2-earlier
  additions are in place: `MapBool`; `kField` honours `align` (ES_RIGHT/ES_CENTER)
  + `readOnly` (ES_READONLY); the `g_in_apply` re-entrancy guard
  (`WinViewInApply()`) that `win_callbacks.cpp` checks to skip re-entrant dispatch
  during `Apply`.
- `win.dart` — `Ui.field(align, readOnly, onEnter)`; `widgetIds`/`hasPending`
  getters (so `buildApp` tracks exactly the ids the user app adds); `_winDispatch`
  **auto-commits** after a handler that described changes (the app-pane model:
  handlers describe, the runtime flushes).
- `test/workspace.dart` — `buildFind`/`doFind`/`openFindResult`; the inlined
  `Calculator` + `buildApp`; `buildDocs`/`selectDocsClass`; `buildHelp`; and a
  real `buildPlaceholder` for Debug (points at T4). Self-test extended to
  snapshot all eight tabs.
- No quarry edits; `windart-port.patch` unchanged.

## Verification
0 errors; build exit 0 (only `win_view.cpp` recompiled + dartui relinked).
All eight tab PNGs viewed and correct. App stays interactive (pid alive at 3s).
Regressions clean: dart.exe `hello, windart` / `sum 1..100 = 5050`; lissajous
46687b; plasma 157395b; Pong 3247b (HUD+ball+paddle); standalone shell 53614b
(329 classes, Float32x4/291); standalone editor 33915b. Quarry pristine (nothing
under the reference SDK modified).

## Next (user priority order): T3 (game-pane live present + audio + shaders), then T4 (debugger)
Still open (carried from T1, not blocking): a live editor-onText highlight via a
deferred (non-re-entrant) dispatch; the separate-language-isolate model so the
Editor tab's Accept morphs live instances in-app (mirrors + a source library
coexisting); wiring field `onEnter` (kind 5) needs a Win32 subclass in the kField
materialize — today the Find button drives the search, `onEnter` is registered but
not yet delivered.
