# WINDART Sprint S3 — dart:io real bring-up + hot-reload embedder API

Goal: prove dart:io actually works on Windows (file, directory, isolates,
Platform, stdio), land the OS-neutral workspace hot-reload embedder primitives,
and keep the JIT milestone green. Builds on S2's `dart.exe`/`dart_bootstrap.exe`.

## dart:io — PROVEN WORKING (captured output)
Tests in `e:\windart\test\`, run with `dart.exe` (snapshot path). All exit 0.

### io_test.dart — File / Directory / Platform / stdio
```
platform.operatingSystem = windows
platform.numberOfProcessors = 20
platform.pathSeparator = "\"
platform.executable nonEmpty = true
file readback = "windart io works | second line"
file lengthSync = 29
file existsSync = true
port-win entries = 13; first5 = CMakeLists.txt, S1_NOTES.md, S2_NOTES.md, build.ps1, create_resources.py
stdout.writeln works
stderr.writeln works (goes to stderr)
file deleted, existsSync = false
IO_TEST_OK
```
So: `File.writeAsStringSync/readAsStringSync/lengthSync/existsSync/deleteSync`,
`Directory.listSync`, and `Platform.*` all work on Windows.

### isolate_test.dart — spawn + SendPort/ReceivePort round-trip
```
isolate square(7) = 49
isolate round-trip reply = echo:ping
ISOLATE_TEST_OK
```
Isolates are load-bearing for the whole workspace (every demo/game/app runs in
its own isolate). Both **one-way spawn** (compute-and-reply) and a
**bidirectional** exchange (worker hands back its SendPort, parent sends,
worker echoes) work.

### stdin_test.dart — piped stdin
```
read[1]: alpha        (a UTF-8 BOM prefixes line 1 — a PowerShell pipe artifact,
read[2]: beta          not a VM issue; readLineSync itself is correct)
read[3]: gamma
STDIN_TEST_OK (3 lines)
```

### Deferred / expected-to-fail
- **TLS / HTTPS / SecureSocket** — compiled out (`secure_socket_unsupported` +
  `DART_IO_SECURE_SOCKET_DISABLED`). Any `SecureSocket`/`https` use throws
  "Secure Sockets unsupported" by design (S3 defers TLS).
- **gzip/zlib (`ZLibEncoder`/`GZipCodec`)** — `windart_filter_unsupported.cc`
  stubs throw "not supported (zlib deferred to S3)". Optional zlib vendoring
  below was not done this sprint (not on the S4 critical path).

## Hot-reload embedder API — APPLIED, compiles, links, exports
Applied the two OS-neutral hunks of `macdart-port.patch` (no cocoa→win rename —
they are not cocoa-specific) to `tree\`:
- `runtime/vm/dart_api_impl.cc` — `Dart_WorkspaceReloadSources(bool)` (wraps
  `Isolate::CanReload` + `Isolate::ReloadSources`, i.e. InstanceMorpher + Become)
  and `Dart_WorkspaceVmStats(int64_t*, intptr_t)` (heap + compiler-stats sampler).
- `runtime/include/dart_tools_api.h` — the two declarations + `#define
  kDartWorkspaceVmStatCount 9`.

**Verification:**
- The engine recompiled with the patch — **0 errors** — in both `dart_engine_snap`
  and `dart_engine_nosnap`. The machinery the API calls (`isolate_reload.cc`,
  `JSONStream`, `Become`, `aggregate_compiler_stats`) was already compiled into
  the engine since S1 (confirmed: `isolate_reload.cc.obj` present in both engines,
  no PRODUCT/precompiler guard).
- Both symbols are **defined as `External` with C linkage** in
  `dart_api_impl.cc.obj` (dumpbin /SYMBOLS): `Dart_WorkspaceReloadSources`,
  `Dart_WorkspaceVmStats` — plain names, no C++ mangling, confirming the
  `DART_EXPORT` = `extern "C"` contract (DART_SHARED_LIB stays undefined).
- `dart.exe` + `dart_bootstrap.exe` relinked and still run hello.dart (exit 0).

### Reload live-morph test — DEFERRED to S7 (documented)
The full "morph live instances across a reload" test needs the workspace: the
Dart-visible caller is `workspace_natives.cc`, which is part of the GUI lib
(dart_win32) not built until S4/S7. A standalone mini-embedder that boots a
reloadable isolate (tag handler + script loading) is disproportionate for S3.
Per the architect's sanctioned minimum, S3 lands + compile/link/symbol-verifies
the primitive; the end-to-end live-morph reload test is deferred to S7 when the
workspace can call it. The API path is sound: the entry points link and the
underlying `Isolate::ReloadSources` is already exercised structurally by the VM.

## Source edits to tree\ (all reproducible via extract.py + documented)
`extract.py` now applies every WINDART source edit idempotently by string
insertion after the pristine copy (no external patch tool), and they are also
captured in **`port-win/windart-port.patch`** (3 files, unified diff):
1. `platform/globals.h` — the S1 CRT-macro version-guards.
2. `vm/dart_api_impl.cc` — the two workspace API functions.
3. `include/dart_tools_api.h` — their declarations + the count macro.
The quarry stays pristine; re-running `extract.py` regenerates the tree with all
edits (verified: full clean rebuild from the regenerated tree links + runs).

NOT applied (belong to S4, the dart:win bridge): the embedder-registration hunks
(bin/builtin*, dartutils, gen_snapshot, main.cc dart:win native-lib hookup) and
invocation_mirror_patch.dart. The 3 arm64/clang fixes stay permanently dropped.

## S4 hand-off
- Backend is complete: JIT (S2) + dart:io + isolates (S3) all proven on Windows.
- The reload primitive is in libdart, ready for `workspace_natives.cc` to call.
- Parallel GUI-host design lands at `e:\windart\gui-design\` (win_host.cpp pump,
  dart_win32 resolver, view-server protocol). S4 executes it: the Win32 message
  pump + `Dart_SetMessageNotifyCallback` wake, the resolver table, a minimal
  widget materializer — plus the dart:win embedder-registration hunks above.
- Still deferred: TLS (S-later), zlib/gzip (vendor to re-enable filter.cc).
