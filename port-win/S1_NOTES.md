# WINDART Sprint S1 — VM-core compile bring-up (notes)

Build-system port of Dart 1.24.3 to native Windows x64 / MSVC. This file is the
running log for S1: what was authored, the exact build invocation, the error
burn-down, and the patch hunks deferred to later sprints.

## Ground truth (confirmed this sprint)
- Compiler: **MSVC 19.50.35730.0** (VS 2026 Professional, cl 14.50.35717), via
  `C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat`.
  CMake 4.x + Ninja + Python 3.12.10. `cl.exe` only on PATH after vcvars64.
- Reference quarry (READ-ONLY, untouched): `e:\dart_origins\sdk-1.24.3`.

## What was created (all under `e:\windart\port-win\`, owned/tracked)
- `extract.py` — shutil port of the mac `extract.sh`. Copies runtime/{vm,platform,
  lib,bin,include}, runtime/third_party/double-conversion, sdk/lib (browser libs
  excluded), the gyp manifest, tools/VERSION, LICENSE/PATENTS into `e:\windart\tree\`.
  Same exclude globs (`*_test.cc/.h`, `*_test_*.cc`, `.git`). Writes the Windows
  zlib stub. Idempotent. Keeps the quarry pristine.
- `gen_sources.py` — mac resolver with the two filters INVERTED: keep `_x64`/`_win`,
  drop `_ia32|_arm64|_arm|_mips|_dbc|simulator` and `_linux|_macos|_android|
  _fuchsia|_openbsd|_solaris`.
- `gen_library_src_paths.py`, `make_version.py` — copied verbatim from the mac
  port (OS-neutral Python generators; no changes needed).
- `CMakeLists.txt` — Windows/MSVC VM-core build (narrow S1 scope: `dart_engine`
  static lib only).
- `msvc_compat.h` — force-included (`/FI`) compat shim; starts empty, populated
  from the error burn-down.
- `build.ps1` — vcvars64 → cmake -G Ninja → ninja, teeing to `build\build.log`.

Generated (not tracked): `e:\windart\tree\` (extract output), `e:\windart\build\`.

## Exact working build invocation
```
powershell -ExecutionPolicy Bypass -File e:\windart\port-win\build.ps1 -Clean
```
Or manually (the env from vcvars does not survive back to the parent shell, so
configure+build must share one `cmd /c`):
```
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" ^
  && cmake -G Ninja -S e:\windart\port-win -B e:\windart\build -DCMAKE_BUILD_TYPE=Debug ^
  && ninja -C e:\windart\build -k 0'
```
Run `extract.py` once first (populates `tree\`).

## Configure result
Clean. `-- WINDART S1: 159 VM+platform, 28 lib-native, 23 generated core-lib,
8 double-conversion sources`. Engine TU set ≈ 215 (VM+lib+api+version+bootstrap).

### gen_sources filter verification
- x64 KEPT (12): assembler_x64, code_patcher_x64, cpu_x64, debugger_x64,
  disassembler_x64, flow_graph_compiler_x64, instructions_x64,
  intermediate_language_x64, intrinsifier_x64, malloc_hooks_x64,
  runtime_entry_x64, stub_code_x64 — the full JIT backend.
- win KEPT (8): floating_point_win, cpuinfo_win, native_symbol_win,
  os_thread_win, os_win, signal_handler_win, thread_interrupter_win,
  virtual_memory_win — the full Windows OS layer.
- arm64 / macos / linux / ia32 all DROPPED (0 each).
- malloc_hooks: gen_sources keeps {x64, unsupported, tcmalloc, jemalloc}; CMake
  `list(FILTER ... EXCLUDE malloc_hooks_(tcmalloc|jemalloc|x64))` leaves exactly
  `malloc_hooks_unsupported.cc` (the real no-tcmalloc impl).

## Design decisions
- **OS macros auto-derive; not redefined.** `platform/globals.h` self-defines
  `HOST_OS_WINDOWS` (from `_WIN32`, line 112), `TARGET_OS_WINDOWS` (line 393),
  and `HOST_ARCH_X64` (from `_M_X64`, line 208). Redefining them on the command
  line would collide, so only `TARGET_ARCH_X64` is passed explicitly (arch is the
  one thing not auto-derivable). `DART_HOST_OS_WINDOWS` is a *later*-Dart macro,
  unused in 1.24.3 — omitted.
- **`/permissive` (non-strict), not `/permissive-`.** Dartium/dart.exe compiled
  under MSVC 2015/2017 permissive mode; matching that maximises the chance of a
  clean compile. A `/permissive-` conformance pass is a later refinement.
- **`/Z7` not `/Zi`** so parallel Ninja compiles don't serialise on one shared PDB.
- **`DART_NO_SNAPSHOT`** on the engine so bootstrap.cc + the generated core-lib
  source arrays are the live (compiled-from-source) path — the config S2's
  gen_snapshot/dart_bootstrap build first.
- **Tree kept pristine** (only the owned zlib stub added). WINDOWS_PORTING_PLAN
  §10 mandates building pristine 1.24.3 first to isolate 2017-C++-under-MSVC-2026
  issues from any port edits. The port patch is therefore documented, not applied.

## Port-patch hunks deferred (NOT applied in S1)
`macdart-port.patch` mixes three arch fixes (dropped) with OS-neutral embedder
integration (belongs to S2/S3, all touch bin/* or the embedder API, none in the
S1 VM-core-only compile):
- **Dropped permanently (arch/clang, N/A on x64/MSVC):** `cpu_arm64.cc`
  FlushICache, `stub_code_arm64.cc` SP-writeback trap, `flow_graph_compiler.cc`
  VisitBlocks null-guard (a clang-17 UB fix; harmless but unnecessary under MSVC).
- **Carry to S2/S3 (embedder):** `bin/builtin{.h,.cc,_natives.cc,_nolib.cc}`,
  `bin/dartutils.{h,cc}`, `bin/gen_snapshot.cc`, `bin/main.cc` (dart:win native-lib
  registration + UI host hook), `runtime/lib/invocation_mirror_patch.dart`
  (source-order named args — a .dart change, no C++ impact), and the
  `dart_api_impl.cc` + `include/dart_tools_api.h` workspace primitives
  (`Dart_WorkspaceReloadSources`, `Dart_WorkspaceVmStats`). These will be
  regenerated as `windart-port.patch` and applied with `APPLY_PATCH=1` env when
  the embedder is built.

## zlib handling
No system zlib on Windows and none vendored this sprint. `extract.py` writes
`runtime/third_party/zlib/zlib.h` as an `#error` stub (dart:io's `filter.h`
includes `"zlib/zlib.h"`). `bin/filter.cc` is the only compiler of it and is not
in any S1 target, so the stub is inert here — pure future-proofing. To lift:
vendor zlib into `runtime/third_party/zlib` and re-add `bin/filter.cc` to
`dart_io` (S3).

## Compile result — CLEAN (S1 exit criterion MET)
`ninja -k 0` exit 0. **250/250 steps, final step linked `dart_engine.lib`.**
- **0 errors**, 0 FAILED steps. No error burn-down needed.
- **224 TUs compiled** (216 engine objects + 8 double-conversion), including the
  entire risky surface: x64 JIT backend (assembler_x64, stub_code_x64,
  code_patcher_x64, flow_graph_compiler_x64, intrinsifier_x64,
  intermediate_language_x64, runtime_entry_x64, disassembler_x64), the Windows OS
  layer (virtual_memory_win, os_win, os_thread_win, signal_handler_win,
  thread_interrupter_win, cpuinfo_win, native_symbol_win), the 9.3 MB
  `vm/object.cc` monster TU, and `dart_api_impl.cc`.
- **Artifacts:** `build\dart_engine.lib` (289 MB, `lib /LIST` → 216 members),
  `build\double_conversion.lib` (573 KB). Validated as genuine COFF archives.
- **Warnings: 10 total, all benign** — 9× C4312 (pointer-width reinterpret_cast
  in `thread_interrupter_win.cc`'s CONTEXT register access + `thread.cc`; correct
  on Win64) and 1× C4172 (returning temporary address). No warning-as-error.

**Source-tree modifications: NONE.** The quarry stayed pristine; `tree\` is a
verbatim extract (only the owned zlib stub added, which never compiles in S1).
`msvc_compat.h` remained empty — no compat shim was needed. The 2017 Dartium C++
compiles under MSVC 19.50 out of the box in permissive mode, confirming the plan's
core thesis (the Windows VM-core delta is ~zero source changes).

### Why it was clean (root cause of the good outcome)
Dart 1.24.3 *was* MSVC's own child — Dartium and the standalone `dart.exe`
shipped as first-class MSVC-compiled Windows x64 binaries in 2017. `globals.h`
carries live `_MSC_VER` paths, `platform/*_win.*` and c99/inttypes support
headers, and the whole `_win.cc` OS layer. MSVC 19.50 in `/permissive` mode is
close enough to 2015/2017 that nothing bit. (A `/permissive-` strict pass would
likely surface two-phase-lookup / conformance nits — deferred, non-blocking.)

## Reconciliation with the MSVC2026 compat dossier (`dossier\MSVC2026_COMPAT.md`)
The parallel recon predicted a "#1 landmine" (globals.h snprintf/strtoll macros)
that would "cascade into many errors." My pristine build compiled clean anyway.
Not a contradiction — the dossier itself notes the C2039 collision is
**order-dependent** (Google-style "system headers first" ordering hides it, which
the VM-core TUs happen to use). Reconciled + hardened, all re-verified green:
- **APPLIED — globals.h version-guards (dossier §3a, the one required source edit).**
  Even though S1 compiles clean either way, the *unguarded* macros are a latent
  RUNTIME bug: `snprintf`→`_snprintf` returns -1 on truncation, breaking the C99
  `(NULL,0)`-measure idiom used in `platform/assert.cc:25` (S1) and the embedder
  (S2). `extract.py` now version-guards both (`_MSC_VER < 1900`) — see the tree
  modification list below. This also removes the order-dependent compile hazard
  for S2's larger TU set.
- **APPLIED — mirror Dart's own suppression list** (`build/config/compiler/BUILD.gn:521-529`)
  instead of guessed `/wd`s: `/wd4005 /wd4091 /wd4351 /wd4312 /wd4838 /wd4172
  /wd4311 /wd4477` (+ sanctioned `/wd4996 /wd4244 /wd4267`). `/wd4005` is the PRI-
  macro redefinition guard; `/wd4312`+`/wd4172` silence exactly the 10 warnings
  the first build emitted.
- **APPLIED — `_CRT_SECURE_NO_DEPRECATE`** alongside `_CRT_SECURE_NO_WARNINGS` (§3b).
- **CONFIRMED — `/bigobj`** present; `vm/object.cc` (23,362 lines → 9.3 MB obj) and
  `kernel_to_il.cc` (5.2 MB obj) both compiled — no C1128.
- **CONFIRMED — `/permissive` (not `/permissive-`)**; dossier §6 + architect both
  say do NOT enable strict conformance mode. We pass the *lenient* form explicitly.
- **CONFIRMED — `/EHsc`, exceptions ON** (not `_HAS_EXCEPTIONS=0`). The dossier's
  heaviest-STL exceptions probe target, `kernel_to_il.cc`, compiled clean under
  `/EHsc` — i.e. the dossier's sanctioned fallback works; `_HAS_EXCEPTIONS=0` is
  not needed.
- **KEPT `/std:c++14`** (proven clean). Dossier prefers c++17 but only "if c++14
  gives STL trouble" — it did not. c++17 remains a one-line switch if ever needed.

### Divergences flagged for the architect (S2 decisions, not S1 blockers)
Two defines mirror the *mac* template but the dossier argues they should be OFF
for a pure JIT build. Both compiled clean, so they don't affect S1, but they
affect S2 linking/runtime and should be decided before `dart.exe`:
- **`DART_SHARED_LIB`** — makes `DART_EXPORT` = `__declspec(dllexport)`. Dossier §4:
  "for a static-lib/exe JIT build, leave DART_SHARED_LIB undefined." Harmless when
  archiving a `.lib`; revisit when linking the executable.
- **`DART_PRECOMPILER`** — dossier §1/point 8: leave undefined for JIT. Mac defined
  it ("matches the stock bootstrap target"). Compiles both ways; decide the intended
  runtime config (pure JIT vs. snapshot-generator) in S2.
- **`TARGET_ARCH_X64`** — task-mandated explicit; dossier prefers auto-derive from
  `_M_X64`. Harmless for a vcvars64-pinned x64 build (values match); kept per task.

## Next steps (S2 hand-off)
1. **S2 is unblocked.** Add the embedder/bin layer + `gen_snapshot` + `dart.exe`;
   apply the OS-neutral embedder patch (regen as `windart-port.patch`, run
   `extract.py` with `APPLY_PATCH=1`).
2. Optional hardening: a `/permissive-` conformance pass; build the snapshot
   (non-`DART_NO_SNAPSHOT`) engine variant too; wire `dart:io` (`bin/*_win.cc`,
   vendor zlib to re-enable `filter.cc`).
