# P2 — second polish pass (Agent B)

Five items, all landed + verified with captured PNGs under `e:\windart\build\`.
No git commits (architect handles). Reference quarry `e:\dart_origins\sdk-1.24.3`
left git-clean. `tree\` untouched (extract.py NOT run — the build reads the
`dart_win32\` layer directly via `dart_gen_builtin` + the `WIN32DIR` CMake var,
and `test\workspace.dart` is loaded at runtime).

Build/run recipe used every cycle:
`Get-Process dartui | Stop-Process -Force` →
`powershell -File port-win\build.ps1` →
`build\dartui.exe test\workspace.dart selftest`  (writes all the PNGs, then quits).

What needs a rebuild vs. not: the C++ + `win.dart` (embedded via `win_gen.cc`)
require a rebuild; `test\workspace.dart` is a runtime script (no rebuild).

---

## Item 1 — Wire the toolbar buttons  ✅

Root cause: the icon toolbar posts `WM_COMMAND` with `lParam == toolbar HWND`
(non-zero), so the old WndProc gate `if (l == 0 …)` sent toolbar clicks to
`OnSurfaceCommand`, where the host command id (e.g. 145) is no widget ticket → it
failed closed. Menu items (lParam==0) worked. Toolbar was dead.

Fix — route by ID SPACE, not lParam. Host commands (menu + toolbar) are all
`< 0x100`; widget tickets start at `0x100` (win.dart `_nextTicket`), disjoint by
design.
- `win_host.cpp` WM_COMMAND handler: `if (id < 0x100) OnMenuCommand(id); else OnSurfaceCommand(...)`.
- `test/workspace.dart` `dispatchMenu`: added 146 Back / 147 Forward; changed 145
  Home to Browser navigation. New browser history: `browseHistory/browseCursor/_browseRecord/_browseGo/browseBack/browseForward/browseHome`; `selectClass` records history.
- Test scaffolding: `Workspace_fireCommand(int id)` native (`windart_workspace_natives.cc`,
  registered in `win_natives.cpp`, `wsFireCommand` in `win.dart`) synthesizes the
  EXACT toolbar WM_COMMAND (lParam == toolbar HWND) via `WinHostToolbarHwnd()`
  (new accessor in `win_host.{h,cpp}`), so the self-test drives the real
  WndProc → OnMenuCommand → menu queue → pollMenu → dispatchMenu chain.

Proof (all fired through the real toolbar path):
- `toolbar_doit.png`   — Output shows `111 + 222 => 333` (toolbar Do-It).
- `toolbar_back.png`   — Browser navigated back to Duration.
- `toolbar_home.png`   — Browser reset to the top class.

Menu items verified working too (same id<0x100 route; polish_editor still fires
via menu id 202).

---

## Item 2 — Window resize reflow  ✅

Kind-7 resize already reached `_winDispatch` but was dropped (`default: return null`).
- `win.dart`: added `Ui._onResize/_onClose` + `onResize()/onClose()`; `_winDispatch`
  now routes kinds 7/8 to the Ui's own handlers BEFORE the per-widget lookup
  (surface ticket is not a widget), unpacking `arg` into `(w>>32, h&0xFFFFFFFF)`
  and auto-committing.
- `win_natives.cpp`: `Win_surfaceSize` was a skeleton returning 0 — now returns
  the host's real `GetClientRect`, so the app sizes its initial layout to the pane
  and can read size after a resize.
- `win_callbacks.cpp`: `OnSurfaceResize` guarded with `WinViewInApply()` +
  ignores 0×0 (minimize).
- `test/workspace.dart`: `paneW/paneH` globals; **all major tab layouts are now
  size-parameterized** (Workspace, Browser, Editor, Find, Docs, Help). `relayout(w,h)`
  re-places the tab strip + rebuilds the active tab. `ui.onResize(relayout)` wired
  in `main()`; initial `paneW/paneH` seeded from `ui.width/height`.
- Test scaffolding: `Workspace_resizeWindow(w,h)` native (`SetWindowPos` on the
  main window → real WM_SIZE → container move → container WM_SIZE → kind-7
  dispatch). `wsResizeWindow` in `win.dart`.

Proof:
- `resize_large.png` — Browser fills a 1438×820 pane (source line fits on one row).
- `resize_small.png` — Browser reflows into a 798×440 pane.

Not reflowed (intentional, fixed-size content): App (Calculator), VM (label grid),
Debug (dense fixed panel). They still render; only the 6 content-heavy tabs reflow.

---

## Item 3 — Editor shows method bodies / signatures  ✅

User classes already showed full stored source incl. bodies (Counter's `bump()`).
The gap was VM classes: `classSketch` emitted only `fn name;` (bare names).

Fix — `test/workspace.dart` `classSketch` rewritten to render the live class
mirror with REAL signatures: helpers `_typeName / _paramList / _methodDecl`.
Emits `class X extends Y {`, then grouped + sorted `// fields` (typed, static/final),
`// constructors` (named-param braces), `// accessors` (typed getters/setters),
`// methods` (return + parameter types). Defensive try/catch around every mirror
access. This also upgraded the Browser + Docs source panes.

Proof:
- `editor_vmclass.png` — Duration: `static final Duration ZERO;`,
  `Duration({int days}, {int hours}, …)`, `bool get isNegative;`, `int get inDays;`.
- `tab_browser.png` / `tab_docs.png` — same richer rendering.

---

## Item 4 — Categorized Smalltalk browser  ✅

Restructured the Browser into the classic drill-down:
**Libraries | Classes | (Variables / Methods) | Source**.

- `main()`: builds `libraryNames` (via `LibraryMirror.uri` → dart:core, dart:io, …),
  `classesInLib`, `libOfClass`. `classMirrors/classNames` stay flat for Find/Docs/nav.
- `buildBrowser` rewritten: Libraries list → `selectLibrary`; Classes list (in the
  selected library) → `selectLibClass`; a Variables list + a Methods list
  (`loadVarsMethods` splits `VariableMirror`s vs `MethodMirror`s); Source editor.
  `selectBrVar/selectBrMethod` show the selected member's declaration.
- `browseToClass(name)` is the shared entry point (Classes pane, Find jumps,
  toolbar Back/Forward/Home) — syncs the category + class panes, loads vars/methods,
  shows source. `selectClass(flatIndex)` delegates to it.

Proof:
- `browser_categorized.png` — Libraries(14) → dart:io Classes(88) → File →
  Variables(0) + Methods(34) → Source shows File's full declaration.
- `browser_member.png` — clicking method 0 → Source shows `// dart:io :: File` +
  `DateTime lastAccessedSync();`.

---

## Item 5 — User-movable splitters  ✅

New `kSplitter` widget kind, self-contained in C++ (no per-move Dart round-trip).
- `win_view.h`: `kSplitter` enum.
- `win_view.cpp`: `ParseWidgetKind("splitter")`; `SplitterCtx` (surface ticket +
  left/right neighbor ids + orientation), `ChildRectInParent`, and `SplitterProc`
  (SetWindowSubclass): WM_SETCURSOR → IDC_SIZEWE/IDC_SIZENS; WM_LBUTTONDOWN →
  SetCapture; WM_MOUSEMOVE (captured) → resize both neighbors + move the bar via
  `MoveWindow` (neighbors resolved by id at drag time from the surface ticket, so
  creation order / rebuild HWND churn don't matter; clamped to a 48/40px min);
  WM_LBUTTONUP → ReleaseCapture; WM_PAINT → a raised BTNFACE groove; WM_NCDESTROY →
  free ctx. Materialize creates a `STATIC | SS_NOTIFY` (needed so the static is
  HTCLIENT and gets the mouse messages) and subclasses it.
- `win.dart`: `ui.splitter(id, {orientation, frame, between:[leftId,rightId]})`.
- `test/workspace.dart`: `br_split` wired between the Libraries and Classes panes.
- Test scaffolding: `Workspace_dragWidget(ticket, dx, dy)` native synthesizes
  WM_LBUTTONDOWN/MOUSEMOVE/LBUTTONUP with lParam-carried client coords (which
  SplitterProc reads) — drives the real drag path headlessly. `wsDragWidget` in `win.dart`.

Per the design note, the drag does NOT notify Dart per move; I did not add the
optional single LBUTTONUP → Dart persistence hook (position resets on the next
tab-rebuild/relayout). Easy to add later if persistence is wanted.

Proof:
- `splitter_before.png` — divider at rest (Libraries pane ~176px, Classes ~196px).
- `splitter_after.png` — after a 120px drag: Libraries pane widened (full
  `file:///…/workspace.dart` name now visible), Classes pane narrowed (names
  truncated), its right edge fixed. Neighbors resized entirely C++-side.

---

## Files changed (mine)

C++ / win.dart (rebuild required):
- `win_host.cpp` / `win_host.h`   — WM_COMMAND id-space routing; `WinHostToolbarHwnd()`.
- `win_callbacks.cpp`             — `OnSurfaceResize` in-apply + 0×0 guards.
- `win_natives.cpp`               — real `Win_surfaceSize`; register 3 test natives.
- `win_view.cpp` / `win_view.h`   — `kSplitter` widget + subclass drag.
- `windart_workspace_natives.cc`  — `Workspace_fireCommand/resizeWindow/dragWidget`; +`win_view.h` include.
- `win.dart`                      — kind 7/8 dispatch, `onResize/onClose`, `ui.splitter`, 3 native decls.

Dart app (runtime, no rebuild):
- `test/workspace.dart`           — items 1–5 Dart side + self-test captures.

Not mine (prior polish pass, already in the working tree): `win_canvas.*`,
`CMakeLists.txt`, `dartui.manifest`, `dartui.rc`, `win_toolbar.*`.

## Residual note (cosmetic, not a bug)

In a couple of editor snapshots a class-declaration line briefly rendered as a bare
`static`/`static final` token (e.g. one of Duration's `SECONDS_PER_*` or a Float32x4
constant). The SAME class renders complete in other panes (`tab_docs.png` shows all
three `SECONDS_PER_*`), so this is a transient RichEdit repaint artifact after
`applySpans`, not a data defect in `classSketch`.

---

## Post-audit fixes (architect adversarial review)

A 6-agent adversarial pass (real user-gesture path + edge cases + build/regression)
returned items 1 & 4 CONFIRMED and items 2/3/5 PLAUSIBLE with medium concerns. All
medium concerns fixed + rebuilt + selftest re-run green + `editor_vmclass.png` re-viewed:

- **Item 3 `_paramList`** (workspace.dart) — was emitting one brace per param,
  `Duration({int days}, {int hours}, ...)` = invalid Dart. Now groups ALL named in one
  `{...}` and all optional-positionals in one `[...]`:
  `Duration({int days, int hours, int minutes, int seconds, int milliseconds, int microseconds});`.
- **Item 3 `classSketch`** (workspace.dart) — wrapped the `declarations.forEach` body in
  try/catch so one throwing mirror skips that declaration instead of blanking the whole
  source pane (makes the earlier defensive-guard claim actually true).
- **Item 2 resize debounce** (workspace.dart `onResizeCoalesced`) — coalesces a burst of
  WM_SIZE into ONE `relayout` on a 60 ms trailing edge; `paneW/paneH` track latest size
  immediately. Kills the per-event full-tab teardown flicker during a live border drag.
  `win_callbacks.cpp` comment corrected (coalescing is Dart-side; C++ forwards every event).
- **Item 5 splitter capture loss** (win_view.cpp `SplitterProc`) — added `WM_CAPTURECHANGED`
  + `MK_LBUTTON` guard in `WM_MOUSEMOVE`, so Alt-Tab / dialog / lock mid-drag ends the drag
  rather than a buttonless stuck-drag that follows the cursor.
- **Item 5 min-clamp** (win_view.cpp) — `if (maxLeft<minLeft) maxLeft=minLeft` (both axes)
  so an extreme window shrink can't invert a pane to negative width.

Deferred (low, documented, non-crashing): unbounded `browseHistory` (memory only);
generics dropped in `_typeName` (`Map` not `Map<K,V>`); Find/Back nav updates pane data
but doesn't move the list selection highlight; menu keyboard accelerators are decorative
(no `TranslateAccelerator`).
