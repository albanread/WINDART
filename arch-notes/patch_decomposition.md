# macdart-port.patch — decomposition for the Windows sprint sequence

The mac patch (17KB, `e:\windart\MACDARTV1\macdart\patches\macdart-port.patch`)
mixes three arch/OS fixes with OS-neutral embedder integration. This maps each
hunk to the sprint that needs it. Regenerate as `windart-port.patch` per-sprint.

## DROP (arm64/mac-specific — do not carry to Windows)
- `cpu_arm64.cc` — FlushICache via `sys_icache_invalidate`. **N/A**: x64
  FlushICache is a no-op (`cpu_x64.cc:24`), and this arm64 file is filtered out.
- `stub_code_arm64.cc` — the `str SP,[SP,#-8]!` Apple-Silicon illegal-instr fix.
  **N/A**: arm64-only.

## KEEP — S1 (VM core), the one shared fix
- `flow_graph_compiler.cc` — guard `LoopInfoComment` so `*loop_headers` is never
  formed on the NULL (comments-off) path. This is an **OS/arch-neutral UB fix**
  (a real null-deref that clang-17 exploited). It lives in the VM-core lib, so it
  belongs in S1. Harmless under MSVC even if MSVC's optimizer wouldn't trip it —
  keep it defensively. **This is the ONLY patch hunk S1 needs.**

## KEEP — S3 (embedder + hot-reload), OS-neutral VM API
- `dart_api_impl.cc` — `Dart_WorkspaceReloadSources(bool)` +
  `Dart_WorkspaceVmStats(int64_t*, intptr_t)`. Pure VM API over
  `Isolate::ReloadSources` + heap/compiler-stats. OS-neutral. The hot-reload
  primitive the whole "live workspace" rests on (`HOTRELOAD_DESIGN.md`).
- `dart_tools_api.h` — the two declarations + `kDartWorkspaceVmStatCount 9`.

## ADAPT — S4 (dart:win bridge registration), cocoa→win rename
Mechanically OS-neutral, but all say "cocoa"; on Windows they register `dart:win`
and resolve `WinNativeLookup` (which only exists once `win_natives.cpp` lands in
S4). **Omit these entirely for S2** (stock dart.exe, no bridge):
- `builtin.h` — enum `kCocoaLibrary` + `cocoa_source_paths_` → `kWinLibrary` +
  `win_source_paths_`.
- `builtin.cc` — register `kCocoaLibURL` row → `kWinLibURL`.
- `builtin_natives.cc` — `IONativeLookup` fallthrough → `CocoaNativeLookup` →
  `WinNativeLookup`; same for `*NativeSymbol`. `#include "cocoa_natives.h"` →
  `win_natives.h`.
- `builtin_nolib.cc` — snapshot-mode lib row.
- `dartutils.h/.cc` — `kCocoaLibURL = "dart:cocoa"` → `kWinLibURL = "dart:win"`.
- `gen_snapshot.cc` — load the lib into the snapshot isolate (so `dart:win`
  source is captured). Needed once the snapshot should embed dart:win (S4).

## ADAPT — S4 (host entry), cocoa→win rename
- `main.cc` — `extern "C" int macdart_run_ui_host()` → `windart_run_ui_host()`;
  the `#if defined(DART_UI_HOST)` branch runs the Win32 pump instead of
  `Dart_RunLoop()`; `--compiler_stats` for the toolbar counters.

## DEFER / likely-DROP — S7
- `invocation_mirror_patch.dart` — makes `noSuchMethod` recover named-arg
  *source order* so ObjC keyword selectors (`colorWithRed:green:blue:alpha:`)
  can be rebuilt. Under the **native Direct2D view-server** (no dynamic selector
  bridge), this is probably unnecessary. Revisit only if `win.dart`'s API ends up
  needing call-site named-arg order. Harmless if kept, but prefer to drop it to
  stay minimal.

## Sprint → patch summary
| Sprint | Patch hunks needed |
|---|---|
| S1 (VM core) | `flow_graph_compiler.cc` guard only |
| S2 (stock dart.exe) | **none** (build unpatched upstream bin/) |
| S3 (embedder + reload) | `dart_api_impl.cc` + `dart_tools_api.h` |
| S4 (win host + bridge) | builtin*/dartutils/gen_snapshot (cocoa→win) + main.cc (→windart_run_ui_host) |
| S7 (workspace) | invocation_mirror_patch — only if needed (likely drop) |
