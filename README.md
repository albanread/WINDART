# WINDART

A native **Windows x64 JIT** port of the **Dart 1.24.3** virtual machine — the last
release of the **V1** Dart language (optional typing, in-VM parser), from December
2017 — together with a from-scratch native **Win32 + Direct2D + Direct3D 11** IDE:
a live Smalltalk-style workspace with a class browser, a syntax-highlighting editor,
live evaluation, morphing hot-reload, a Direct3D game pane, and a debugger.

It is the Windows sibling of [MACDART](https://github.com/albanread/MACDARTV1) (the
Apple-Silicon port). Where MACDART had to *invent* arm64-JIT-on-macOS, Windows
**inherits** a capability that shipped in production in 2017: Dartium and the
standalone `dart.exe` were first-class Windows x64 JITs, so the VM-core delta is
~zero source changes. The work is the build system and the native GUI.

## What works (all built from source, all verified)

- **VM + JIT** — Dart 1.24.3's x64 JIT compiles under **MSVC 19.50**; runs real V1
  Dart with optimizing tier-up.
- **`dart:io`, isolates, hot-reload** — files, sockets, `Isolate.spawn`, and the
  `Dart_WorkspaceReloadSources` embedder primitive.
- **Native GUI** — a Win32 + Direct2D **view-server**: Dart *describes* widgets and
  *receives* events; a C++ materializer realizes them as native controls. No
  `objc_msgSend` bridge — the view-server deletes that problem.
- **Direct2D canvas** — the demos render (Mandelbrot, plasma, boids, …).
- **Direct3D 11 game pane** — the arcade games render **live in a window** (Space
  Invaders animating over an HLSL starfield), with **XAudio2** sound.
- **The IDE** (`dartui.exe`) — a 9-tab workspace: a live **class browser** (reading
  the running VM's classes), a **syntax editor** with live **Do-It** evaluation,
  **Accept** with **morphing hot-reload** and **SQLite** persistence, **Find**, a
  live user-app **App** pane (a working calculator), **Docs**/**Help**, live **VM**
  counters, and a working **Debugger** (breakpoints, pause, call stack, frame-scoped
  eval, stepping).

## Building

Requires: **MSVC (VS 2022/2026)**, **CMake**, **Ninja**, **Python 3**, and a Dart
1.24.3 source checkout as the reference quarry (the sources are **not** vendored).

```powershell
# 1. Reference sources (the last V1 release), placed at ..\sdk or a path you pass:
git clone --depth 1 --branch 1.24.3 https://github.com/dart-lang/sdk.git ..\sdk

# 2. Extract the needed subset + apply the port patch + vendor SQLite:
python port-win\extract.py

# 3. Build (MSVC via vcvars64, CMake + Ninja):
powershell -ExecutionPolicy Bypass -File port-win\build.ps1 -Clean

# 4. Run:
build\dart.exe hello.dart                       # the console JIT
build\dartui.exe test\workspace.dart            # the full live IDE (9 tabs)
```

See `WINDOWS_PORTING_PLAN.md` for the full design, `SPRINTS.md` for the build order,
and `port-win\*_NOTES.md` for the per-sprint engineering log.

## Layout

- `port-win/` — the owned build system + the native `dart_win32/` layer (the Win32
  host, the view-server materializer, the Direct2D canvas, the D3D11 game engine,
  XAudio2 audio, the SQLite/eval/reload/debug natives) + the port patch + build log.
- `test/` — the Dart apps: `workspace.dart` (the full IDE) and the demos/games.
- `WINDOWS_PORTING_PLAN.md`, `SPRINTS.md`, and the design-note directories
  (`gui-design/`, `gamepane-design/`, `s7-prep/`, `dossier/`, `arch-notes/`).
- `MACDARTV1/`, `tree/`, `build/` — **not tracked** (the reference clone, the
  extracted Dart sources, and the build output; regenerated / obtained separately).

## Licensing

The Dart VM and core libraries this project ports are **BSD-3-Clause** (Copyright
the Dart project authors) with an additional patent grant — they are **not** vendored
here; `port-win/extract.py` fetches them from an upstream checkout and preserves the
licenses. `port-win/windart-port.patch` is the small set of port changes against
pristine 1.24.3. The WINDART port itself (the scripts, the CMake build, the
`dart_win32` layer) is the author's own work.
