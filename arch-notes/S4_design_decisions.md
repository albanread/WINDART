# S4 GUI-host design — architect review + decisions

Reviewed `e:\windart\gui-design\S4_GUI_HOST_DESIGN.md` (+ 8 skeleton files).
**Verdict: APPROVED for execution.** Grounded transliteration, correct deletion
of the objc_msgSend machinery, sound thread model. The three flagged unknowns are
real and correctly scoped; resolved below. Execute S4 *after* S3 lands (the win
host needs the dart:win registration + a working dart:io), as a compile-iterate
sprint (write → build → fix), NOT speculative blind authoring of the materializer.

## Decisions on the three hard unknowns

1. **Editor widget → RichEdit first, Direct2D/DirectWrite as the finished form.**
   The `Win_editorApplySpans` contract hides the backend from Dart, so staging is
   free of Dart churn. RichEdit (`MSFTEDIT_CLASS`, `EM_SETCHARFORMAT` per syntax
   run) gets a *working* workspace editor fast; the D2D custom view (text layout,
   caret, selection, scroll, IME, hit-test) is deferred to post-functional polish.
   Do NOT build the D2D editor first. (Scintilla is the fallback only if RichEdit's
   span/undo model proves too quirky for the workspace.)

2. **Materializer (`Win_surfaceApply` + `win_view` registry) = S4's core original
   code.** No shortcut exists — WINVM was WebView2, so this is the one large
   net-new C++ surface. Accept it. Two correctness GATES (both flagged, both real):
   - **Ownership:** Win32 controls are *owned* — teardown must `DestroyWindow`
     the children, not just clear the ticket map (AppKit held targets weakly).
   - **The rebuildUi ticket trap** (`APP_PANE_PLAN.md` §7): after a hot-reload
     re-materialise, a surviving pop-out window must get its surfaces re-built and
     tickets re-bound, or its buttons go dead. Re-materialise every surface from
     the spec on rebuild. Make these two an explicit S4 test.

3. **Visual fidelity → common controls for bring-up (S4), Direct2D custom widgets
   only where the IDE chrome demands (S7).** Do NOT front-load the D2D toolkit.
   S4's exit milestone (a window with a button whose action is a Dart closure) and
   the whole app-pane/demo surface ship on themed common controls
   (BUTTON/EDIT/STATIC/COMBOBOX/SysListView32/SysTabControl32). Migrate to D2D
   custom-drawn widgets in S7 only for the pieces where matching the AppKit-tuned
   chrome requires it — likely the editor (already staged), tabs, and list rows.
   The one materializer behind the one protocol serves both, so this is a
   per-widget migration, not a rewrite.

## Smaller decisions (accept the agent's recommendations)
- **Keycode namespace:** C-side VK→macOS-keycode remap table in `Win_keyState`,
  so portable game Dart (which uses macOS keycodes) stays unchanged.
- **Wake coalescing:** DEFER — ship the faithful post-per-notify baseline; add
  single-outstanding backpressure only if a hot push loop proves it necessary.
- **List data source:** `LVS_OWNERDATA` (virtual/pull, faithful to `NSTableView`).

## S4 execution scope (when S3 done)
1. Regenerate `windart-port.patch` — the 4 cocoa→win embedder hunks (main.cc
   DART_UI_HOST→windart_run_ui_host; builtin_natives resolver fallthrough to
   WinNativeLookup; builtin/dartutils/gen_snapshot dart:win registration). DROP
   invocation_mirror_patch.dart (selector bridge is gone).
2. New `dart_win32` static lib (win_host.cpp, win_natives.cpp, win_callbacks.cpp,
   win_view + materializer, win.dart) — link libs `comctl32 d2d1 dwrite
   windowscodecs ole32 uxtheme` (add d3d11/dxgi/xaudio2 in S6). Fill the skeletons.
3. New `dartui.exe` target (dart.exe + DART_UI_HOST + dart_win32). 
4. **S4 exit:** `dartui.exe` opens a window with a button whose action fires a Dart
   closure (round-trips: Dart describes button → materializer creates it →
   WM_COMMAND → ticket → _winDispatch → Dart handler runs). Compile-iterate.
