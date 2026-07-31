# WINDART Sprint S7 — the workspace IDE (shell + editor/Do-It + Accept/morph/SQLite)

## S7.3 — THE CAPSTONE: the SQLite image + Accept + morphing hot-reload
Milestone HIT, both proofs. WINDART is now a FULLY LIVE Windows Dart workspace,
and the headline "more live than MACVM" claim is demonstrated.

### (a) Persistence — the SQLite image survives restart (`workspace_userclass_restart.png`)
User class SOURCE lives in a SQLite image at `%USERPROFILE%\.windart\workspace.sqlite`.
```
RUN 1 (define):  Accept -> wrote Foo, Bar, Widget to the image
RUN 2 (restart, a FRESH PROCESS, no define):
     user classes loaded from image: Bar, Foo, Widget
```
The restart PNG shows the browser populated with Bar/Foo/Widget + Bar's source —
loaded purely from the on-disk image (12 KB). SQLite is real, wired, persistent.

### (b) Morphing hot-reload — a live instance morphs across a class-shape change
```
MORPH: v1  gc.n = 3                          (a Counter{n}, bumped 3x)
MORPH: Accept -> counter_scratch.dart rewritten (added `int step = 7`)
MORPH: reload status = "ok"          (dartui: UI reloaded)
MORPH: after reload  gc.n    = 3     (KEPT across the shape change)
MORPH: after reload  gc.step = 7     (NEW field, morphed into the live instance)
MORPH: after one more v2 bump  gc.n = 10   (3 + step -> new method logic is live)
```
The SAME instance survived a structural class change (a field added): it **kept
its state (n=3) AND gained the new field (step=7)**, and the recompiled method
(`n += step`) is live — via the VM's InstanceMorpher/Become. No restart. This is
the second Smalltalk gesture (Accept), and the project's "more live than MACVM".

### S7.3 — what was created (owned; no quarry edits, patch unchanged)
- **Vendored SQLite 3.45.0** (`sqlite3.c`+`.h`) into `tree\runtime\third_party\sqlite\`,
  reproducibly via `extract.py` (from the libsqlite3-sys cargo cache; `SQLITE_SRC`
  env override). Compiled into `dart_win32` as C (quiet, `SQLITE_THREADSAFE=1`,
  `SQLITE_OMIT_LOAD_EXTENSION`).
- **`windart_sqlite_natives.cc`** — the real `Sqlite_open/close/exec/query`
  (ported verbatim from the cocoa dir), replacing the stubs; `windart_gui_stubs.cc`
  deleted (all its symbols are now real). The `Db` wrapper (win.dart, from S4) drives it.
- **The Accept/morph path** reuses S2's snapshot-loading VM + S3's
  `Dart_WorkspaceReloadSources` embedder primitive + S4's host-driven reload
  (`windart_request_ui_reload` -> `PerformUiReload` at the pump top, no Dart frames)
  — the machinery was all already compiled; S7.3 wires it to the editor + a scratch
  library + the image.
- Test apps: `workspace_image.dart` (persistence), `workspace_morph.dart` +
  `counter_scratch.dart` (the morph; the app rewrites the scratch v1->v2 at runtime,
  reset the fixture to v1 before re-running).

### S7.3 notes / honest scope
- The morph runs in the UI isolate (single-isolate for the proof) via the
  host-driven reload; the plan's separate language-isolate model (so the chrome
  stays stable while user code reloads) is the fuller form, deferred. The morph
  mechanism itself is proven end to end.
- New-field default: the morph applied the initializer value (`step = 7`), not
  null — even richer than "gains the field defaulted".
- Regressions all clean (dart.exe, shell 329 classes, editor Do-It 35/27,
  lissajous, plasma, Pong, button). Quarry git-clean; windart-port.patch unchanged.

---

# WINDART Sprint S7 — the workspace IDE (slice-1 shell + S7.2 editor & Do-It)

## S7.2 — the code editor + the LIVE Do-It (WINDART is now a live Dart workspace)
Milestone HIT, two proofs (`e:\windart\build\workspace_editor.png` + stdout):
- **(a) syntax-coloured editor** — a **RichEdit** (`MSFTEDIT_CLASS`, monospace)
  shows a Dart snippet coloured by **`lexDart`** (ported VERBATIM from
  workspace.dart) → **`Win_editorApplySpans`** (`EM_SETCHARFORMAT` per run):
  green comments, blue keywords (`class`/`final`/`int`/`static`/`bool`), teal
  types (`Point`/`String`), dark-red strings, purple numbers. The applySpans
  contract is byte-identical to Cocoa, so `lexDart` needed no change.
- **(b) live Do-It** — expressions evaluated against the RUNNING VM via
  `Workspace_eval → Dart_EvaluateExpr(Dart_RootLibrary())`, captured to stdout:
  ```
  (2 + 3) * 7                                    =>  35
  new List.filled(3, 9).fold(0, (a, b) => a + b) =>  27
  'windart'.toUpperCase()                        =>  WINDART
  new List.generate(6, (i) => i * i)             =>  [0, 1, 4, 9, 16, 25]
  ```
  The gesture: editor text → `Win_editorSelection` (whole buffer when nothing is
  selected = Do-It-all) → `wsEval` → `Dart_EvaluateExpr` → toString. This is the
  Smalltalk Do-It, live on Windows.

### S7.2 — what was created (owned, additive; no tree edits)
- **`text` widget kind** (RichEdit) in `win_view.cpp`; `Msftedit.dll` loaded at
  host init (win_host.cpp) to register `RICHEDIT50W`; Consolas font.
- **`Win_editorApplySpans`** implemented (was a stub): `EM_EXSETSEL` +
  `EM_SETCHARFORMAT(CFM_COLOR)` per `[start,len,kind]`, redraw-suppressed,
  selection preserved. UTF-16 offsets == Dart indices (no remap).
- **`Win_editorSelection`** now returns the whole buffer when nothing is selected
  (Do-It-all), else the selection.
- **`windart_workspace_natives.cc`** (ported from the cocoa dir, cocoa→win): real
  `Workspace_eval`/`reload`/`vmStats` + the ui-host forwarders — REPLACING the
  S4 stubs (`windart_gui_stubs.cc` now holds only the `Sqlite_*` stubs).
- `test/workspace_editor.dart` — the editor app (lexDart + Do-It).
- Regressions all clean (dart.exe, workspace shell 329 classes, lissajous, plasma,
  Pong, dartui button); quarry git-clean; windart-port.patch unchanged.

### S7.2 note — Do-It target scope
`Dart_EvaluateExpr` runs against the current isolate's root library (the running
app's library, which imports dart:core/async/win). A dedicated language isolate +
the scratch-library scope (`Workspace_reload` for Accept) is the S7.3 step; the
transient Do-It works now. Multi-line editor text uses `\r` breaks (RichEdit's
native newline); lexDart offsets align (single char per break).

---

# S7 slice-1 — the workspace IDE shell + read-only class browser

Goal: the first visible slice of the IDE re-host — the workspace SHELL renders
with its tab-deck chrome and a **populated read-only class browser**, PNG-verified.
Per the plan (`s7-prep/S7_REHOST_PLAN.md`) + signoff (`arch-notes/S7_plan_signoff.md`).
NOT the whole 4,853-LOC chrome; the editor/Accept/debugger are later slices.

## MILESTONE — HIT (`e:\windart\build\workspace_shell.png`)
```
> dartui.exe workspace_shell.dart e:\windart\build\workspace_shell.png
WS: SNAP -> ...workspace_shell.png OK (329 classes, 291 members of Float32x4)
```
The PNG shows a real IDE class browser: the **9-tab deck** (Workspace | Browser |
Editor | Find | Docs | App | Debug | VM | Help), the **Classes (329)** pane
populated from the VM's own class table (AsciiCodec, AssertionError, Base64Codec,
ClassMirror, Codec, Comparable, …), the **Members of Float32x4 (291)** pane
(`fn abs`, `fn clamp`, `fn sqrt`, `get signMask`, `get w/x/y/z`, … with fn/get/new
prefixes), and a "Source (read-only)" box — all native Win32 controls, captured
by `Win_surfaceSnapshot` (PrintWindow → PNG).

## What was created (owned, additive — no tree edits, windart-port.patch unchanged)
- **ViewServer widget kinds** (`win_view.cpp`, additive `case`s exactly like the
  S5 `canvas`): **`list`** (`SysListView32` `LVS_REPORT|LVS_OWNERDATA` — a virtual
  pull table: item count via `LVM_SETITEMCOUNT`, cells pulled through the existing
  `LVN_GETDISPINFO`→kind-3 dispatch, `reloadData()`→`set(rows:)` wired in `DoSet`),
  **`tabs`** (`SysTabControl32` + `TCN_SELCHANGE`→kind-4 in win_callbacks),
  **`field`** (`EDIT`), **`popup`** (`COMBOBOX` + items), **`box`** (`BUTTON
  BS_GROUPBOX`). `win.dart` gained `Ui.tabs`/`Ui.box` (field/popup/list existed).
- **`Win_editorSelection(ticket)→[start,len,text]`** (S7 catalog gap 1 — the
  Do-It-on-selection pull; `EM_GETSEL` on EDIT/RichEdit).
- **`Win_surfaceSnapshot` extended to a WINDOW snapshot** (S7 catalog gap 2 — the
  headless drive-and-see capture the whole regression discipline needs): if the
  surface has a canvas → the S5 D2D/WIC readback; else → **`SnapshotHwndToPng`**
  (`PrintWindow` with `PW_RENDERFULLCONTENT` → DIB → WIC PNG), so the native-control
  workspace window snapshots too. The PNG encode is now one shared `EncodeToPng`.
- **`test/workspace_shell.dart`** — the shell app (tab deck + class/member lists +
  source box + status), populating the browser from `dart:mirrors`.

## SQLite image — deferred (documented blocker), class list from the VM instead
The `Db`/SQLite natives are still stubs and I cannot fetch the SQLite amalgamation
(no network — same constraint as S1's zlib). Per the signoff's sanctioned fallback,
this slice renders the class browser **read-only from the VM's own class table via
`dart:mirrors`** (`currentMirrorSystem().libraries` → `ClassMirror`s → 329 classes;
`ClassMirror.declarations` → members). This proves the browser without the image.
**To lift (needed for Accept/persist/respawn):** drop `sqlite3.c`+`sqlite3.h` into
`tree\runtime\third_party\sqlite\`, wire `sqlite_natives.cc` (portable, from the
MACDART cocoa dir) into `dart_win32`, link `sqlite3`, and repoint
`windart_gui_stubs.cc`'s `Sqlite_*` to it. Image path also moves to
`%LOCALAPPDATA%\windart\workspace.sqlite` (plan §6.5).

## Notes / small gaps
- `dart:mirrors` works in the snapshot VM (verified: 330 classes). Filtered to
  public (non-`_`) names.
- The member pane defaults to the class with the most directly-declared public
  members (Float32x4) so it's visibly populated; clicking a class updates it via
  the `onSelect`→`loadMembers`→`set(rows:)`→`commit()` path (the reloadData port).
- Panes are fixed-frame (no draggable splitter — the plan's bring-up decision);
  the tab deck is the clear+re-describe pattern (a real `SysTabControl32` strip
  shows the tabs).
- Compile burn-down: **0 errors** on the first S7 build (the widget cases +
  PrintWindow snapshot compiled clean under the S1 flag set).

## Regressions (all clean)
`dart.exe hello` (hello,windart); demos lissajous 46687 b + plasma 162941 b; Pong
game_pong.png 3247 b; dartui button `BUTTON CLICKED (ticket=257)` — all identical
to prior sprints. Quarry git-clean.

## Next S7 slices (per the plan's phase order)
- S7.2 — the Workspace tab: the **RichEdit editor** + `Win_editorApplySpans`
  highlighting (the crux) + Do It / Print It / Accept (uses `Win_editorSelection`).
- S7.3 — the real class-browser state machine (category→class→member) + editable
  source pane; **land SQLite** first (Accept/persist source of truth).
- S7.5 — the App tab collapses onto this same ViewServer (Sketch D).
- S7.6 — the debugger tab (vm-service client ports verbatim).
The port of `workspace.dart`'s ~2,900 lines of neutral logic + the ~1,250 rewritten
materialization lines is the bulk of the remaining S7 effort, phased per the plan.
