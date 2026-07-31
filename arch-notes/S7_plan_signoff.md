# S7 IDE-chrome rehost plan — architect sign-off

Reviewed `e:\windart\s7-prep\S7_REHOST_PLAN.md`. **Verdict: APPROVED as the S7
roadmap.** Clarifying analysis; the 60/30/10 split (neutral logic / rewritten
materialization / deleted) makes S7 smaller than 4,853 LOC implies. Execute after
S5 + S6, as a phased, incrementally-runnable sprint (the 8-phase order in the
plan), NOT a big-bang rewrite.

## Key structural findings (accepted)
- **language.dart = 1-line port** (`import 'dart:cocoa'`→`'dart:win'`; its 4 uses
  re-export identically). Already a view-server client by construction. The
  `defer` re-entrancy pattern must port verbatim (same Dart_InvokeClosure hazard).
- **The chrome's own NSView app-pane materializer DELETES** — the C++ ViewServer
  (S4) now owns materialization. Biggest single LOC win. The selector-lint deletes
  too (nothing to lint without the objc bridge).
- **Tab deck (9 tabless tabs) + nested splitters + containers** — the view-server
  has none. BRING-UP: language-side fixed-frame layout + clear/re-describe the
  deck (zero new C++). FINISHED: persistent groups + a self-contained C++ splitter
  (never a Dart round-trip — the setSplitMinSize history proves it). Stage it.

## Two catalog GAPS to close (not widgets — easy to under-scope)
1. **Text-selection pull op** — Do-It-on-selection needs to READ the editor's
   current selection; the S4 op catalog has `Win_widgetText` but not selection.
   ADD `Win_editorSelection(ticket) -> [start,len,text]` (or similar). On the S7
   editor critical path.
2. **Offscreen surface→PNG snapshot native** — `Win_surfaceSnapshot(surface) ->
   PNG bytes`. This IS the project's headless drive-and-see regression loop
   (the mac `bitmapImageRepForCachingDisplayInRect`). **It's the SAME WIC-PNG
   readback A is building RIGHT NOW for S5 canvas verify** — so direct A (at S5
   handoff) to generalize its demo-PNG-readback into a reusable
   `Win_surfaceSnapshot` native, and S6/S7 reuse it. Don't let it stay a
   demo-only hack.

## SQLite image (the Accept/persist/respawn source of truth)
The `Db`/SQLite natives are STUBBED today (windart_gui_stubs.cc). Per the S1
cocoa inventory, `sqlite_natives.cc` is portable C — S7 needs it wired with a
bundled SQLite amalgamation (drop into tree\third_party\ + add to dart_win32 +
link). Not hard, but load-bearing for the live workspace (Accept survives
restart; morphing reload). Schedule early in S7.

## The 3 hardest S7 risks (all have staged answers)
1. Syntax-highlighting editor (RichEdit bring-up via Win_editorApplySpans →
   D2D finished; the applySpans contract keeps lexDart byte-identical) — the crux.
2. Structural widgets (tab deck/splitters) — language-side layout bring-up, C++
   splitter later.
3. Non-widget infra (SQLite image + PNG snapshot) — both above; land early.

Both remaining GUI sprints now fully specced: S6 (D3D11 games, signed off) and
S7 (this). Design phase complete; the rest is execution (S5 finishing → S6 → S7).
