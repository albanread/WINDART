# WINDART Sprint S2 — the JIT milestone (`dart … hello.dart`)

Goal: link the embedder/bin layer + `gen_snapshot` + `dart.exe`, run a trivial
V1 program, and prove it executes JIT-compiled x64 on Windows. Builds on S1's
proven VM-core (`dart_engine`). Reuses the S1 flag set verbatim (/permissive,
/EHsc, /std:c++14, /bigobj, /MT-default, /Z7, the defines + Dart's own /wd list).

## MILESTONE — HIT (dart_bootstrap)
```
> dart_bootstrap.exe e:\windart\hello.dart
hello, windart
sum 1..100 = 5050        (exit 0)
```
`hello.dart` is `main(){ print('hello, windart'); int sum=0; for(...) sum+=i; print(...); }`.
This is the first Dart JIT execution on Windows in WINDART: the VM created an
isolate, bootstrapped the core libraries **from source** (nosnapshot path),
JIT-compiled `main()` + the loop to x64, ran it, and printed via the embedder.
The `5050` proves the JIT emitted and executed correct integer arithmetic — not
just startup. `dart_bootstrap.exe` = 16.9 MB, linked 0 errors.

## What was created / changed (all owned, under `port-win\`)
- **CMakeLists.txt** extended from the S1 VM-core scope to the full S2 target set,
  structured on the mac template with Windows swaps.
- **windart_filter_unsupported.cc** (new, owned) — throwing stubs for the four
  `Filter_*` zlib natives that `io_natives.cc`'s resolver references, since
  `filter.cc` is excluded (zlib deferred to S3). Mirrors upstream
  `filter_unsupported.cc` (whose bodies these are) but unguarded.
- **create_resources.py** — copied from mac, then **fixed for Windows paths**
  (see the bug below).
- **create_snapshot_file.py**, **windart_browser_stubs.cc** — copied from mac
  (OS-neutral / scaffolding), unchanged.

## Targets (mac template → Windows)
- **Two engines:** `dart_engine_nosnap` (DART_NO_SNAPSHOT → gen_snapshot,
  dart_bootstrap) and `dart_engine_snap` (→ dart.exe). Same ENGINE_CC.
- **dart_builtin:** `builtin_impl_sources.gypi` (gen_sources → `_win`; minus the
  `(directory|file)_unsupported` stubs) + **`log_win.cc`** (not log_macos).
- **dart_io:** explicit list, every `*_macos.cc` → `*_win.cc` (eventhandler_win,
  file_system_watcher_win, platform_win, process_win, socket_base_win, socket_win,
  stdio_win, sync_socket_win). TLS OFF (secure_socket_unsupported +
  root_certificates_unsupported + io_service_no_ssl). `filter.cc` EXCLUDED;
  `windart_filter_unsupported.cc` supplies its natives.
- **Windows system libs** (replace MACOS_FRAMEWORKS): `ws2_32 iphlpapi rpcrt4
  shell32 advapi32 ole32 psapi winmm dbghelp`. `dbghelp` added for
  `native_symbol_win.cc` (SymInitialize/StackWalk).
- **gen_snapshot**, the **snapshot pipeline** (run gen_snapshot → vm/isolate
  `.bin` → create_snapshot_file.py → snapshot_gen.cc), and **dart.exe**.
- **dartui / dart:win bridge / embedder patch:** NOT built (S3/S4). Stock dart.exe.

## Config decisions applied (from the architect, reconciled with S1)
- **DART_SHARED_LIB — dropped** (global). Static-lib/exe JIT build → DART_EXPORT
  is plain `extern "C"`, not `__declspec(dllexport)` (dossier §4).
- **DART_PRECOMPILER — dropped globally** (pure JIT, not AOT precompiler). Engine
  recompiled without it — clean (the prior build reached the late resources step,
  so all ~200 engine + bin TUs compiled without the two defines).
  - **One surgical exception:** `bin/main.cc` is compiled WITH `DART_PRECOMPILER`
    (via `set_source_files_properties … TARGET_DIRECTORY`). This is the ONLY way
    to compile out main.cc's Observatory-assets zlib path (`#include "zlib/zlib.h"`
    + `Decompress()` under `#if !defined(DART_PRECOMPILER)`), which is the sole
    remaining zlib user once filter.cc is excluded. It is local to main.cc's
    preprocessor — it does NOT touch the engine ABI, and the JIT-vs-AOT runtime
    split is gated by the *separate* `DART_PRECOMPILED_RUNTIME` (left undefined),
    so JIT behavior is unaffected. The Observatory assets are empty anyway.
- **DART_PRECOMPILED_RUNTIME — undefined** everywhere (→ full JIT VM).

## Bug found + fixed: create_resources.py on Windows paths
The mac `create_resources.py` generated invalid C++ on Windows:
`const char e:_windart_tree_...loader_dart_[]` — the drive-letter **colon** was
never stripped (the symbol regex replaced `/ . - \` but not `:`), and the
`--root_prefix` strip failed because CMake passes forward slashes while
gen_sources yields backslash + uppercase-drive paths, so the whole absolute path
leaked into both the symbol and the resource URL (31 compile errors in
`resources_gen.cc`). **Fix:** normalize both prefix and source to forward slashes,
strip case-insensitively, and build the C identifier with
`re.sub(r"[^0-9A-Za-z_]", "_", name)`. Output now matches mac semantics —
`vmservice_loader_dart_` / URL `/vmservice/loader.dart`.

## Snapshot pipeline (gen_snapshot → .bin → dart.exe) — WORKS
The full pipeline links AND runs on Windows:
- **gen_snapshot.exe** (16.9 MB) linked, then **RAN** (first gen_snapshot
  execution on Windows) and emitted valid snapshots:
  `vm_isolate_snapshot.bin` 835,162 B + `isolate_snapshot.bin` 236,267 B.
- `create_snapshot_file.py` embedded them → `snapshot_gen.cc` (5.1 MB).
- **dart.exe** (17 MB) linked against `dart_engine_snap` + snapshot_gen.cc.
- **Runtime proof (snapshot path):**
  ```
  > dart.exe e:\windart\hello.dart
  hello, windart
  sum 1..100 = 5050            (exit 0)
  > dart.exe --version
  Dart VM version: 1.24.3 (WINDART) on "windows_x64"   (exit 0)
  ```
  So BOTH bring-up paths execute JIT x64 on Windows: `dart_bootstrap` (compiles
  core libs from source) and `dart` (loads them from the embedded snapshot).

Build labels: `make_version.py` `BUILD_TIME_LABEL` corrected MACDART → **WINDART**
(the version string now reads "1.24.3 (WINDART)"; platform auto-reports
"windows_x64").

## Full-build stats
Two-engine + bin + pipeline. Both exes linked with 0 errors / 0 warnings under
the S1 flag set. gen_snapshot + dart + dart_bootstrap all produced and RAN.

## Next (S3)
dart:io real bring-up (already linked, TLS + zlib off), the OS-neutral embedder
patch (`Dart_EvaluateExpr`/`ReloadSources` control-plane), vendor zlib to
re-enable filter.cc.
