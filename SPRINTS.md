# WINDART — Sprint plan (execution)

Companion to `WINDOWS_PORTING_PLAN.md` (the design). This is the executable
breakdown: bounded sprints with objective exit criteria, run by a small agent
team (2–3) that the architect coordinates and reviews between iterations.

## Ground truth (probed 2026-07-29)
- Toolchain: **MSVC 19.50 (VS 2026 Professional)** via
  `C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat`.
  **No clang-cl** — MSVC only. CMake 4.3, Ninja 1.13, Python 3.12 present.
  (Confirmed: `cl.exe` compiles + runs a smoke `.cpp`.)
- Reference source (full, pristine 1.24.3): `e:\dart_origins\sdk-1.24.3`.
- Mac port template (scripts + CMake + GUI to port): `e:\windart\MACDARTV1\macdart`.

## Workspace layout
- `e:\windart\port-win\` — Windows build scaffolding (extract.py, gen_sources.py,
  CMakeLists.txt, build.ps1, windart-port.patch, notes). **Owned, tracked.**
- `e:\windart\tree\` — extracted + patched Windows source tree (populated by
  extract.py from the reference quarry). **Generated, not tracked.**
- `e:\windart\build\` — CMake/Ninja out-of-source build dir. **Generated.**
- Reference quarry stays read-only at `e:\dart_origins\sdk-1.24.3`.

## Risk note carried into every sprint
MSVC 2026 ≫ the MSVC 2015/2017 Dartium targeted. Expect conformance friction:
CRT deprecations, `/permissive-` strictness, `NOMINMAX`/`WIN32_LEAN_AND_MEAN`
needs, changed STL, removed non-standard extensions. This is the dominant W0
cost and is mechanical burn-down, not redesign.

---

## S1 — Build-system port + VM-core compile bring-up  ← ACTIVE
**Goal:** an owned `windart` tree that compiles the Dart VM core (`libdart`
engine objects) under MSVC. **Exit:** `cmake -G Ninja` configures clean and the
VM-core object set compiles (or a precisely triaged, categorized error burn-down
list if it stalls, with the count trending down each iteration).
- Port `extract.sh` → `extract.py` (shutil copy from the reference quarry, same
  exclude globs, `git apply` for the patch, zlib handling for Windows).
- Port `gen_sources.py` filters: keep `_x64`/`_win`, drop `_arm*`/`_macos`/etc.
- Author `CMakeLists.txt` (Windows) from the mac template: `TARGET_ARCH_X64`,
  `HOST_OS_WINDOWS`, `NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `_CRT_SECURE_NO_WARNINGS`,
  MSVC flags (`/std:c++14 /EHsc /permissive-` tuned), Windows libs.
- `build.ps1` — activates vcvars64, runs cmake+ninja, tees logs.
- Drive the compile; categorize + fix errors (platform macros, CRT, STL,
  conformance). Keep the reference quarry untouched.

## S2 — gen_snapshot + dart.exe: the JIT milestone
**Goal:** link `gen_snapshot`, emit the core snapshot, link `dart.exe`, run
`dart.exe hello.dart` executing JIT x64. **Exit:** a V1 `hello.dart` prints via
JIT-compiled native code on Windows. (Depends on S1. Low *design* risk —
upstream capability — but the snapshot bootstrap + link surface is real work.)

## S3 — dart:io + embedder integration
**Goal:** the Windows embedder (`bin/*_win.cc`) + the OS-neutral half of the
embedder patch (native-lib registration, `Dart_EvaluateExpr`/`ReloadSources`,
debugger control-plane). TLS deferred. **Exit:** real V1 programs (file I/O,
isolates) run; the hot-reload primitive works.

## S4 — dart_win32 host + view-server skeleton
**Goal:** `win_host.cpp` (Win32 pump + `Dart_SetMessageNotifyCallback` wake,
transliterated from WINVM `gui/src/shell/win.rs`), the resolver table, a minimal
widget materializer, `win.dart`. **Exit:** `dartui.exe` opens a window with a
button whose action is a Dart closure.

## S5 — Direct2D canvas + demos
**Goal:** `d2d_canvas.cpp` (Direct2D + DirectWrite + WIC) behind the unchanged
draw-command protocol. **Exit:** the non-game demos render.

## S6 — Direct3D game pane
**Goal:** `gp_engine_d3d.cpp` (D3D11; MSL→HLSL for the 5 layers + compute
blitter + swapchain + offscreen readback + direct framebuffer), XAudio2;
`gp_synth.cc` reused. **Exit:** Pong, then Invaders/Brickout; `gpsnap` readback.

## S7 — IDE chrome re-host (workspace.dart → view-server)
**Goal:** re-host the 4,853-LOC IDE chrome onto the widget protocol — class
browser, editor (RichEdit→Direct2D), tabs, tables, menus, debugger. **Exit:**
the full live workspace IDE. (Largest Dart-side effort; unifies with S4.)

---

## Team & cadence
- **Architect (me):** define/adjust sprints, set scaffolding + interfaces,
  adversarially review each agent round, integrate, gate on exit criteria,
  decide the next iteration. Hold ultracode quality: verify claims, don't accept
  "it compiles" without the artifact/log.
- **Agents (2–3, iterating):** bounded work with objective success (configures /
  compiles / links / runs / test passes). Read-only on the reference quarry;
  write only under `e:\windart\port-win\` (+ generated `tree\`/`build\`).
- **Loop:** dispatch → agents run background → architect reviews artifacts+logs →
  integrate/redirect → next round. Continue autonomously until a real blocker or
  a sprint's exit criteria are met.
