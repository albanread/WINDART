# WINDART T4 — the debugger (the Debug tab). The LAST item.

The Debug tab is now a working debugger. It debugs a spawned target isolate via
the classic in-process embedder debug API and drives the full core loop:
**set a line breakpoint → run → PAUSE at it → show the call stack + the current
line marked in source → evaluate an expression in the paused frame → step →
resume → complete.** No quarry edits; all T4 code is owned
(`port-win/dart_win32/` + `test/`).

## MILESTONE — HIT (viewable: `e:\windart\build\tab_debug.png`)
Running the workspace self-test drives one debug session in the Debug tab and
snapshots it. `tab_debug.png` shows: the target source (syntax-highlighted, line
numbers) with **`* 15`** the breakpoint and **`> 16`** the paused line after the
step; the **call stack** (`#0 factorial (debug_target.dart:15)`, `#1 main (:25)`,
`#2 <closure>`, `#3 _RawReceivePortImpl._handleMessage`); the eval field with
**`eval "n * n" => 25`**; the full session transcript; and Run/Step Into/Step
Out/Resume controls. Captured stdout (the headless proof):
```
DBG: attached (debugger handshake); breakpoint set at .../debug_target.dart:15 -> id 1
DBG: PAUSED at breakpoint: factorial line 15
DBG: call stack:  #0 factorial (...:15)  #1 main (...:25)  ...
DBG: frame locals: n=5
DBG: frame-eval  n * n  =>  25
DBG: step over ->
DBG: stepped to factorial line 16; resuming to completion
DBGTGT: factorial(5) = 120
DBG: target isolate finished — debug session complete
```

## The debug mechanism chosen (and why)
Dart 1.24.3 has **two** debug paths. I used the **classic in-process embedder
API** (`dart_tools_api.h`), not the vm-service JSON-RPC (which MACDART uses).
Recon findings that made this the clean route:
- `debugger_api_impl.cc` is **compiled** into our VM (CMakeLists) — 52 exports.
- Our build is **Debug (non-PRODUCT)**, so the classic API is the *functional*
  definition (there is a PRODUCT stub; we don't hit it), and `FLAG_support_debugger`
  defaults **true**.
- `Dart_SetPausedEventHandler` installs a **GLOBAL** bridge (`Debugger::event_handler_`
  is a static) → one handler covers every isolate, including a spawned debuggee.
- `Debugger::Pause` calls the handler **synchronously on the paused isolate's
  thread**, then resumes per the action the handler set (`Dart_SetStepOver/Into/Out`,
  or none = continue). `Dart_ActivationFrameInfo` gives the line number directly;
  `Dart_ActivationFrameEvaluate` is real frame-scoped eval.

## Architecture (owned: `windart_debug_natives.cc`)
- The **target** (`test/debug_target.dart`) runs in **its own isolate** (spawned by
  the Debug tab via `Isolate.spawnUri` — the working demo/game spawn machinery),
  never the UI isolate, so pausing it never freezes the pump. It is self-contained
  (dart:core + dart:developer + dart:isolate — **no mirrors, no source-loaded
  libs**, so it debugs cleanly).
- **Attach handshake:** the target calls `debugger()` at entry → first pause. In
  that pause the handler sets the real **line breakpoint** using the paused
  location's script URL (`Dart_SetBreakpoint`), then resumes to run on to it.
- **At the breakpoint hit** the handler (on the debuggee's pool thread; the UI pump
  is free) captures the stack (`Dart_GetStackTrace` → frames → `ActivationFrameInfo`),
  the top-frame locals (`Dart_GetLocalVariables`), evaluates the UI's expression in
  frame 0 (`Dart_ActivationFrameEvaluate`), then steps (`Dart_SetStepOver/Into/Out`
  by the button chosen) or resumes. Everything is written to a **mutex-guarded
  transcript**; the UI polls it (`Workspace_debugPoll`) on a 100 ms `Timer` and
  refreshes the stack list, the paused-line marker, the eval result, and the log.
- Natives: `Workspace_debugArm(breakLine, evalExpr, stepKind)`,
  `Workspace_debugPoll()`, `Workspace_debugDone()` (wired in `win_natives.cpp`;
  `wsDebug*` in `win.dart`).
- **De-risked first** with a standalone harness (`test/debug_probe.dart`) before
  wiring the tab — it proved the loop headlessly (stack/locals/`n*n=25`/step).

## The Debug tab UI (reuses the tab framework — `buildDebug()` in workspace.dart)
Source view (marked `*`/`>` + syntax highlight), a `SysListView32` call-stack, an
eval field (type any expression → evaluated in the paused frame on the next Run),
a session transcript, and Run/Step-Into/Step-Out/Resume buttons — each Run drives a
session with that step mode.

## Honest limitations (scope calls, all documented)
- **Scripted, not click-by-click.** One Run auto-drives breakpoint→pause→stack→
  eval→step→resume (the architect's stated milestone: "drive a scripted debug
  session"). True interactive *pause-and-manually-step* would need a cross-thread
  **command mailbox** (the handler blocking on the debuggee thread awaiting each UI
  command). The frame-eval IS interactive (any expression you type is evaluated in
  the paused frame on Run); the step **mode** is chosen by button.
- **`debugger()` attach point** in the target — the debuggee opts into debugging
  (as a program launched under a debugger does). Attaching to an already-running
  arbitrary isolate with no handshake is not implemented.
- **Stretch, deferred (as the architect scoped):** conditional breakpoints, a live
  isolate picker (the target is fixed: `debug_target.dart`), and multi-isolate
  polish. Step Into/Out at a non-call line follow VM stepping semantics.

## Verification / regressions (all clean)
Build exit 0. Debug loop verified by `tab_debug.png` + captured stdout + the
standalone probe (`evalResult=25`, `factorial(5)=120`). Regressions: `dart.exe`
`hello, windart`; **all 9 workspace tabs** snapshot OK (adding `dart:isolate`
beside `dart:mirrors` in the UI isolate is fine — the T1 crash was mirrors + a
*source-loaded* lib, not a built-in); offscreen + **live** invaders; lissajous;
Pong. Quarry `e:\dart_origins\sdk-1.24.3` pristine; `windart-port.patch` untouched;
all T4 code in `port-win/dart_win32/` + `test/`.

## Done
With T4 the full stack is complete: **VM + JIT (S1–S3) → Win32/Direct2D GUI (S4–S5)
→ D3D11 live games + audio (S6/T3) → the full tabbed IDE (S7/T1/T2) → the
debugger (T4).** The workspace is ready to launch live.
