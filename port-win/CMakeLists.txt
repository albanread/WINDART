# WINDART — windows-x64 JIT build of the Dart 1.24.3 VM (Sprint S1).
#
# S1 SCOPE IS DELIBERATELY NARROW: compile only the VM-core static library
# (`dart_engine` — the libdart engine objects: VM + core-lib natives + the
# embedding-API impl + the generated core-lib source arrays + bootstrap). This
# is the biggest and riskiest compile (~440 TUs of 2017 C++ under MSVC 2026);
# proving it builds is the whole point. Explicitly OMITTED this sprint:
# dart_win32/dartui (GUI), gen_snapshot, the snapshot pipeline, and the
# dart/dart_bootstrap executables. Those arrive in S2+.
#
# Source lists come from the upstream .gypi manifests, filtered to x64 + win +
# non-test by port/gen_sources.py. Core-library Dart sources are embedded as C
# arrays by port/gen_library_src_paths.py (the "nosnapshot" configuration).
cmake_minimum_required(VERSION 3.20)
project(windart CXX C)
enable_language(RC)   # dartui.rc -> RT_MANIFEST (Common-Controls v6 visual styles)

# 2017-era Dart targets C++11; a modern compiler's default (c++17/20) removes
# features this code uses (dynamic exception specs, etc.). Pin C++14.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)  # DEBUG asserts + heap verification: invaluable during bring-up
endif()

if(NOT MSVC)
  message(FATAL_ERROR "WINDART S1 targets MSVC only (VS 2026 / cl 19.50). No clang-cl.")
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WINDART_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(TREE   e:/windart/tree)                 # extracted + (S1: pristine) source tree
set(RT     ${TREE}/runtime)
set(SDKLIB ${TREE}/sdk/lib)
set(PORT   ${WINDART_ROOT})                 # generators live beside this file
set(DC     ${RT}/third_party/double-conversion/src)
set(GEN    ${CMAKE_CURRENT_BINARY_DIR}/gen)
set(MSVC_COMPAT ${WINDART_ROOT}/msvc_compat.h)
file(MAKE_DIRECTORY ${GEN})

# --- Preprocessor configuration -------------------------------------------
# TARGET_ARCH_X64 is passed explicitly; HOST_OS_WINDOWS / TARGET_OS_WINDOWS and
# HOST_ARCH_X64 auto-derive from _WIN32 / _M_X64 inside platform/globals.h, so
# we deliberately do NOT redefine them (globals.h line 112/393 would collide).
# DART_IO_SECURE_SOCKET_DISABLED defers TLS/BoringSSL (SSL-off posture, as mac).
#
# S2 config decisions (architect, resolved from S1's flags + the dossier):
#   * DART_SHARED_LIB   — NOT defined. For a static-lib/exe JIT build DART_EXPORT
#     should be plain `extern "C"`, not __declspec(dllexport) (dossier §4).
#   * DART_PRECOMPILER  — NOT defined globally. Pure JIT, not the AOT precompiler.
#     (One narrow exception: bin/main.cc is compiled WITH it — see that target —
#     purely to compile out its Observatory-assets zlib path, which is the only
#     zlib user left once filter.cc is excluded. That define is local to main.cc's
#     preprocessor and does not touch the engine ABI or the JIT/AOT-runtime split,
#     which is gated by the separate DART_PRECOMPILED_RUNTIME, left undefined.)
add_compile_definitions(
  TARGET_ARCH_X64
  DART_IO_SECURE_SOCKET_DISABLED
)
# Windows / MSVC conformance + CRT quieting. globals.h also self-defines the
# NOMINMAX/WIN32_LEAN_AND_MEAN/UNICODE guards, but setting them globally ensures
# they hold before ANY header (incl. windows.h pulled in by globals.h) is seen.
add_compile_definitions(
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS
  _CRT_SECURE_NO_DEPRECATE      # dossier §3b: pairs with _NO_WARNINGS (diff header gates)
  _CRT_NONSTDC_NO_DEPRECATE
  _CRT_NONSTDC_NO_WARNINGS
  _WINSOCK_DEPRECATED_NO_WARNINGS
  _SCL_SECURE_NO_WARNINGS
  UNICODE
  _UNICODE
)
# The VM requires exactly one of DEBUG / NDEBUG (globals.h enforces this). Tie
# it to the CMake config. NB Dart uses DEBUG (no underscore), distinct from the
# CRT's _DEBUG that /MDd sets.
add_compile_definitions(
  $<$<CONFIG:Debug>:DEBUG>
  $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)

set(WIN32DIR ${WINDART_ROOT}/dart_win32)   # S4 dart:win host/natives/materializer
include_directories(${RT} ${RT}/include ${DC} ${RT}/third_party
                    ${RT}/third_party/sqlite ${WIN32DIR})

# --- MSVC compile options --------------------------------------------------
# /bigobj          : many Dart TUs blow past the 2^16 section limit (mandatory).
# /Zc:__cplusplus  : report the real __cplusplus (some code feature-tests it).
# /permissive      : NON-strict. The original Dartium/dart.exe compiled under
#                    MSVC 2015/2017 permissive mode; matching that maximises the
#                    chance of a clean compile. Flip to /permissive- later for a
#                    conformance pass (see S1_NOTES.md).
# /FI msvc_compat.h: force-include the owned compat shim ahead of every TU.
# CMake already supplies /EHsc /GR /W3 /DWIN32 /D_WINDOWS for MSVC CXX.
add_compile_options(
  /bigobj
  /Zc:__cplusplus
  /permissive
  # Mirror Dart 1.24.3's OWN curated MSVC suppression set (its build was /WX;
  # ours is not, but carrying the exact set keeps a future /WX pass viable and
  # is what upstream verified safe): build/config/compiler/BUILD.gn:521-529.
  /wd4005   # PRI-macro redefinition (inttypes_support_win.h vs <cinttypes>)
  /wd4091   # 'typedef' warning from dbghelp.h
  /wd4351   # new MSVC array-init semantics (benign)
  /wd4312   # int->pointer of greater size (fired 9x in thread_interrupter_win.cc)
  /wd4838   # narrowing conversion required
  /wd4172   # returning address of local (fired 1x in thread.cc)
  /wd4311   # pointer truncation PVOID->DWORD
  /wd4477   # format string requires wchar_t*
  # Sanctioned optional noise-reducers (dossier §6 tail): common size_t<->int
  # narrowing + the CRT-deprecation warning already gated by the defines above.
  /wd4996 /wd4244 /wd4267
  # C4291 (ZoneAllocated operator new w/ no matching delete): benign by design —
  # Dart's zone allocator frees en masse when the Zone dies, never per-object.
  # Absent from Dart's own list only because Dart compiled exceptions-OFF; our
  # /EHsc (exceptions-on) posture surfaces it (~2100x). Suppress it.
  /wd4291
  "/FI${MSVC_COMPAT}"
)
# Use /Z7 (debug info in .obj) instead of the default /Zi so parallel Ninja
# compiles don't serialise on a single shared .pdb.
string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS_DEBUG}")

# --- Helper: expand .gypi manifest(s) into a filtered source list ----------
function(dart_sources outvar kind)
  set(args "")
  foreach(g ${ARGN})
    list(APPEND args --gypi ${g})
  endforeach()
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${PORT}/gen_sources.py --kind ${kind} ${args}
    OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "gen_sources.py failed (${_rc}) for: ${ARGN}")
  endif()
  string(STRIP "${_out}" _out)
  string(REPLACE "\n" ";" _out "${_out}")
  set(${outvar} ${_out} PARENT_SCOPE)
endfunction()

# --- Generated: version.cc -------------------------------------------------
add_custom_command(
  OUTPUT ${GEN}/version.cc
  COMMAND ${Python3_EXECUTABLE} ${PORT}/make_version.py
          --input ${RT}/vm/version_in.cc --output ${GEN}/version.cc
          --version-file ${TREE}/tools/VERSION --vm-dir ${RT}/vm
  DEPENDS ${PORT}/make_version.py ${RT}/vm/version_in.cc ${TREE}/tools/VERSION
  VERBATIM)

# --- Generated: core-library source arrays ---------------------------------
# Matrix: filebase | library token (var/uri) | has_patch.
set(BOOTSTRAP_LIBS
  "core|core|1"
  "async|async|1"
  "collection|collection|1"
  "convert|convert|1"
  "developer|developer|1"
  "internal|_internal|1"
  "isolate|isolate|1"
  "math|math|1"
  "profiler|profiler|0"
  "typed_data|typed_data|1"
  "vmservice|_vmservice|1"
  "mirrors|mirrors|1"
)

set(GENERATED_LIB_CC "")
foreach(entry ${BOOTSTRAP_LIBS})
  string(REPLACE "|" ";" parts ${entry})
  list(GET parts 0 fb)      # filebase
  list(GET parts 1 tok)     # library token
  list(GET parts 2 haspatch)

  dart_sources(SRC dart ${SDKLIB}/${fb}/${fb}_sources.gypi)
  add_custom_command(
    OUTPUT ${GEN}/${fb}_gen.cc
    COMMAND ${Python3_EXECUTABLE} ${PORT}/gen_library_src_paths.py
            --output ${GEN}/${fb}_gen.cc --input_cc ${RT}/lib/libgen_in.cc
            --include vm/bootstrap.h
            --var_name dart::Bootstrap::${tok}_source_paths_
            --library_name dart:${tok} ${SRC}
    DEPENDS ${PORT}/gen_library_src_paths.py ${RT}/lib/libgen_in.cc ${SRC}
    VERBATIM)
  list(APPEND GENERATED_LIB_CC ${GEN}/${fb}_gen.cc)

  if(haspatch)
    dart_sources(PSRC dart ${RT}/lib/${fb}_sources.gypi)
    add_custom_command(
      OUTPUT ${GEN}/${fb}_patch_gen.cc
      COMMAND ${Python3_EXECUTABLE} ${PORT}/gen_library_src_paths.py
              --output ${GEN}/${fb}_patch_gen.cc --input_cc ${RT}/lib/libgen_in.cc
              --include vm/bootstrap.h
              --var_name dart::Bootstrap::${tok}_patch_paths_
              --library_name dart:${tok} ${PSRC}
      DEPENDS ${PORT}/gen_library_src_paths.py ${RT}/lib/libgen_in.cc ${PSRC}
      VERBATIM)
    list(APPEND GENERATED_LIB_CC ${GEN}/${fb}_patch_gen.cc)
  endif()
endforeach()

# --- Source sets from manifests --------------------------------------------
# VM engine: vm_sources.gypi + service protocol + the platform abstraction
# layer (OS::, Utils::, ...). platform_sources is compiled exactly once, here.
dart_sources(VM_CC cc
  ${RT}/vm/vm_sources.gypi
  ${RT}/vm/service_sources.gypi
  ${RT}/platform/platform_sources.gypi)
# Allocator variant: no tcmalloc/jemalloc on Windows, so malloc_hooks_unsupported.cc
# is our real MallocHooks impl. Drop the tcmalloc/jemalloc backends and the x64
# stack-walker (used only by those backends).
list(FILTER VM_CC EXCLUDE REGEX "malloc_hooks_(tcmalloc|jemalloc|x64)\\.cc$")

# Core-library native methods (the .cc in runtime/lib/*_sources.gypi).
file(GLOB LIB_GYPI ${RT}/lib/*_sources.gypi)
dart_sources(LIB_CC cc ${LIB_GYPI})

# Embedding API implementation + generated version string.
set(API_CC
  ${RT}/vm/dart_api_impl.cc
  ${RT}/vm/debugger_api_impl.cc
  ${RT}/vm/mirrors_api_impl.cc
  ${RT}/vm/native_api_impl.cc
  ${GEN}/version.cc)

# Vendored double-conversion.
file(GLOB DC_CC ${DC}/*.cc)

# --- Targets: engine -------------------------------------------------------
add_library(double_conversion STATIC ${DC_CC})

# The engine is built TWICE, differing only in DART_NO_SNAPSHOT (a compile-time
# switch in Object::Init / Dart::InitOnce). bootstrap.cc is the nosnapshot
# bootstrap (compile core libs from the embedded source arrays); the snapshot
# engine uses the same TU but takes the load-from-snapshot path at runtime.
set(ENGINE_CC ${VM_CC} ${LIB_CC} ${GENERATED_LIB_CC} ${API_CC} ${RT}/vm/bootstrap.cc)
function(add_engine name)
  add_library(${name} STATIC ${ENGINE_CC})
  target_link_libraries(${name} PUBLIC double_conversion)
  target_compile_definitions(${name} PRIVATE ${ARGN})
endfunction()
add_engine(dart_engine_nosnap DART_NO_SNAPSHOT)  # gen_snapshot, dart_bootstrap
add_engine(dart_engine_snap)                     # the snapshot `dart`

# ===========================================================================
# Embedder / bin layer  ->  gen_snapshot, dart_bootstrap, dart
# ===========================================================================

# --- Generated: dart:_builtin, dart:io library + patch source arrays -------
# Same generator as the core libs, but the bin template (builtin_in.cc) and the
# Builtin:: variable namespace.
function(dart_gen_builtin outfile var libname)
  add_custom_command(
    OUTPUT ${GEN}/${outfile}
    COMMAND ${Python3_EXECUTABLE} ${PORT}/gen_library_src_paths.py
            --output ${GEN}/${outfile} --input_cc ${RT}/bin/builtin_in.cc
            --include bin/builtin.h
            --var_name ${var} --library_name ${libname} ${ARGN}
    DEPENDS ${PORT}/gen_library_src_paths.py ${RT}/bin/builtin_in.cc ${ARGN}
    VERBATIM)
endfunction()

dart_sources(BUILTIN_DART dart ${RT}/bin/builtin_sources.gypi)
dart_gen_builtin(builtin_gen.cc dart::bin::Builtin::_builtin_source_paths_ dart:_builtin ${BUILTIN_DART})

dart_sources(IO_DART dart ${SDKLIB}/io/io_sources.gypi)
dart_gen_builtin(io_gen.cc dart::bin::Builtin::io_source_paths_ dart:io ${IO_DART})

dart_sources(IO_PATCH_DART dart ${RT}/bin/io_sources.gypi)
dart_gen_builtin(io_patch_gen.cc dart::bin::Builtin::io_patch_paths_ dart:io ${IO_PATCH_DART})

# --- Generated: vmservice resources (service isolate; symbols must link) ----
dart_sources(VMSERVICE_DART dart ${RT}/bin/vmservice/vmservice_sources.gypi)
add_custom_command(
  OUTPUT ${GEN}/resources_gen.cc
  COMMAND ${Python3_EXECUTABLE} ${PORT}/create_resources.py
          --output ${GEN}/resources_gen.cc --outer_namespace dart
          --inner_namespace bin --table_name service_bin --root_prefix ${RT}/bin/
          ${VMSERVICE_DART}
  DEPENDS ${PORT}/create_resources.py ${VMSERVICE_DART}
  VERBATIM)

# --- libdart_builtin: the C++ file/dir/process/platform layer --------------
dart_sources(BUILTIN_CC cc ${RT}/bin/builtin_impl_sources.gypi)
# Use the real file/directory implementations, not the io-disabled stubs.
list(FILTER BUILTIN_CC EXCLUDE REGEX "(directory|file)_unsupported\\.cc$")
add_library(dart_builtin STATIC ${BUILTIN_CC} ${RT}/bin/log_win.cc)

# --- libdart_io: dart:io native layer, curated for io-on / ssl-off / win ----
# Explicit file set (the io manifest picks SSL/zlib variants via gyp conditionals
# our resolver can't evaluate). Every *_win.cc where mac used *_macos.cc. TLS is
# off (secure_socket_unsupported + certificates stubs); zlib is deferred, so
# filter.cc is EXCLUDED and windart_filter_unsupported.cc supplies the four
# Filter_* natives that io_natives.cc's resolver table still references.
set(IO_CC
  ${RT}/bin/eventhandler.cc            ${RT}/bin/eventhandler_win.cc
  ${RT}/bin/file_system_watcher.cc     ${RT}/bin/file_system_watcher_win.cc
  ${PORT}/windart_filter_unsupported.cc
  ${RT}/bin/io_service_no_ssl.cc
  ${RT}/bin/platform.cc                ${RT}/bin/platform_win.cc
  ${RT}/bin/process.cc                 ${RT}/bin/process_win.cc
  ${RT}/bin/secure_socket_unsupported.cc
  ${RT}/bin/root_certificates_unsupported.cc
  ${RT}/bin/socket.cc                  ${RT}/bin/socket_base.cc
  ${RT}/bin/socket_base_win.cc         ${RT}/bin/socket_win.cc
  ${RT}/bin/stdio.cc                   ${RT}/bin/stdio_win.cc
  ${RT}/bin/sync_socket.cc             ${RT}/bin/sync_socket_win.cc
  ${RT}/bin/io_natives.cc)
add_library(dart_io STATIC ${IO_CC})

# Windows system libraries (replace mac's MACOS_FRAMEWORKS). Add more from link
# errors as the surface grows.
set(WIN_SYS_LIBS ws2_32 iphlpapi rpcrt4 shell32 advapi32 ole32 psapi winmm dbghelp)

# --- dart_bootstrap: nosnapshot JIT (compiles core libs from source) --------
# The proven bring-up binary: needs no valid snapshot, so if the snapshot path
# has trouble this still proves the VM runs + JITs. main.cc is compiled WITH
# DART_PRECOMPILER (local zlib-elision only — see the global-defines note).
add_executable(dart_bootstrap
  ${RT}/bin/builtin.cc ${RT}/bin/builtin_natives.cc ${RT}/bin/error_exit.cc
  ${RT}/bin/main.cc ${RT}/bin/loader.cc ${RT}/bin/snapshot_utils.cc
  ${RT}/bin/snapshot_empty.cc ${RT}/bin/observatory_assets_empty.cc
  ${RT}/bin/vmservice_impl.cc ${PORT}/windart_browser_stubs.cc
  ${GEN}/builtin_gen.cc ${GEN}/io_gen.cc ${GEN}/io_patch_gen.cc ${GEN}/resources_gen.cc)
target_compile_definitions(dart_bootstrap PRIVATE DART_NO_SNAPSHOT)
set_source_files_properties(${RT}/bin/main.cc TARGET_DIRECTORY dart_bootstrap
  PROPERTIES COMPILE_DEFINITIONS DART_PRECOMPILER)
target_link_libraries(dart_bootstrap PRIVATE dart_io dart_builtin dart_engine_nosnap dart_win32 ${WIN_SYS_LIBS})

# --- gen_snapshot: emits the core VM + isolate snapshots --------------------
# Uses gen_snapshot.cc (not main.cc) + builtin_gen_snapshot.cc (minimal native
# resolver). No main.cc here, so no zlib concern.
add_executable(gen_snapshot
  ${RT}/bin/gen_snapshot.cc ${RT}/bin/builtin_gen_snapshot.cc ${RT}/bin/builtin.cc
  ${RT}/bin/loader.cc ${RT}/bin/vmservice_impl.cc ${RT}/bin/address_sanitizer.cc
  ${PORT}/windart_browser_stubs.cc
  ${GEN}/builtin_gen.cc ${GEN}/io_gen.cc ${GEN}/io_patch_gen.cc ${GEN}/resources_gen.cc)
target_compile_definitions(gen_snapshot PRIVATE DART_NO_SNAPSHOT PLATFORM_DISABLE_SOCKET)
target_link_libraries(gen_snapshot PRIVATE dart_io dart_builtin dart_engine_nosnap dart_win32 ${WIN_SYS_LIBS})

# --- Snapshot pipeline: gen_snapshot -> .bin -> snapshot_gen.cc -------------
add_custom_command(
  OUTPUT ${GEN}/vm_isolate_snapshot.bin ${GEN}/isolate_snapshot.bin
  COMMAND $<TARGET_FILE:gen_snapshot> --ignore_unrecognized_flags --snapshot_kind=core
          --vm_snapshot_data=${GEN}/vm_isolate_snapshot.bin
          --isolate_snapshot_data=${GEN}/isolate_snapshot.bin
  DEPENDS gen_snapshot
  COMMENT "Generating core snapshot (vm_isolate_snapshot.bin, isolate_snapshot.bin)"
  VERBATIM)
add_custom_command(
  OUTPUT ${GEN}/snapshot_gen.cc
  COMMAND ${Python3_EXECUTABLE} ${PORT}/create_snapshot_file.py
          --vm_input_bin ${GEN}/vm_isolate_snapshot.bin
          --input_bin ${GEN}/isolate_snapshot.bin
          --input_cc ${RT}/bin/snapshot_in.cc --output ${GEN}/snapshot_gen.cc
  DEPENDS ${PORT}/create_snapshot_file.py ${RT}/bin/snapshot_in.cc
          ${GEN}/vm_isolate_snapshot.bin ${GEN}/isolate_snapshot.bin
  VERBATIM)

# --- dart: snapshot-loading JIT (fast startup) ------------------------------
# Loads core libs from the embedded snapshot: builtin_nolib.cc (no source-array
# table), snapshot_gen.cc, snap engine (no DART_NO_SNAPSHOT).
add_executable(dart
  ${RT}/bin/builtin_nolib.cc ${RT}/bin/builtin_natives.cc ${RT}/bin/error_exit.cc
  ${RT}/bin/main.cc ${RT}/bin/loader.cc ${RT}/bin/snapshot_utils.cc
  ${RT}/bin/observatory_assets_empty.cc ${RT}/bin/vmservice_impl.cc
  ${GEN}/snapshot_gen.cc ${GEN}/resources_gen.cc)
set_source_files_properties(${RT}/bin/main.cc TARGET_DIRECTORY dart
  PROPERTIES COMPILE_DEFINITIONS DART_PRECOMPILER)
target_link_libraries(dart PRIVATE dart_io dart_builtin dart_engine_snap dart_win32 ${WIN_SYS_LIBS})

# ===========================================================================
# S4: dart_win32 (Win32 GUI host + dart:win view-server) + dartui.exe
# ===========================================================================
# The dart:win library source (win_source_paths_) is generated exactly like the
# core libs / dart:io — from win.dart. dart:win is registered UNCONDITIONALLY in
# the embedder (builtin.cc/dartutils.cc, cocoa->win hunks), so win_source_paths_
# and WinNativeLookup must exist in EVERY binary that links the bin layer; hence
# dart_win32 is linked into dart_bootstrap/dart/gen_snapshot too (as dart_cocoa
# was on the mac side). Only dartui defines DART_UI_HOST → runs the Win32 pump.
dart_gen_builtin(win_gen.cc dart::bin::Builtin::win_source_paths_ dart:win ${WIN32DIR}/win.dart)

add_library(dart_win32 STATIC
  ${WIN32DIR}/win_host.cpp        # Win32 UI-thread pump + wake + host reload
  ${WIN32DIR}/win_natives.cpp     # WIN_NATIVE_LIST resolver + op-catalog + materialize entry
  ${WIN32DIR}/win_callbacks.cpp   # WNDPROC/subclass -> ticket -> _winDispatch; key-state
  ${WIN32DIR}/win_view.cpp        # the materializer (ViewServer registry) — S4 net-new
  ${WIN32DIR}/win_canvas.cpp      # Direct2D canvas: demo draw-cmd replay + WIC PNG readback (S5)
  ${WIN32DIR}/win_toolbar.cpp     # icon toolbar (ToolbarWindow32 + D2D icons) (polish)
  ${WIN32DIR}/gp_engine_d3d.cpp   # Direct3D 11 game-pane engine (indexed/sprite/text/shader/blit) (S6) + live present (T3)
  ${WIN32DIR}/gp_natives_win.cpp  # the gp* natives + the gpApply frame wire (S6) + SFX wire (T3)
  ${WIN32DIR}/gp_audio_win.cpp    # XAudio2 SFX playback (GpSfx) (T3)
  ${WIN32DIR}/gp_synth.cc         # the SFX synthesizer (pure C++, reused verbatim) (T3)
  ${WIN32DIR}/windart_workspace_natives.cc # Workspace_eval (Do It) / reload / vmStats (S7.2)
  ${WIN32DIR}/windart_debug_natives.cc     # Workspace_debug* — the in-process debugger (T4)
  ${WIN32DIR}/windart_sqlite_natives.cc    # Sqlite_* (the workspace image, S7.3)
  ${WIN32DIR}/windart_userlib.cc           # userlib: tag handler — DB-served user source (Stage 1)
  ${WIN32DIR}/windart_snapshot_blob.cc     # snapshot-as-blob: boot snapshot from the image (W1)
  ${RT}/third_party/sqlite/sqlite3.c       # vendored SQLite 3.45.0 amalgamation
  ${GEN}/win_gen.cc)              # dart:win library source array
# SQLite: compile the amalgamation as C, quiet (it's a large generated file), with
# a minimal Windows-friendly config.
set_source_files_properties(${RT}/third_party/sqlite/sqlite3.c PROPERTIES
  COMPILE_FLAGS "/w"
  COMPILE_DEFINITIONS "SQLITE_THREADSAFE=1;SQLITE_OMIT_LOAD_EXTENSION=1;_CRT_SECURE_NO_WARNINGS")
target_link_libraries(dart_win32 PUBLIC
  comctl32 d2d1 dwrite windowscodecs ole32 uxtheme user32 gdi32
  d3d11 dxgi d3dcompiler xaudio2)   # S6/T3 game pane (D3D11 + live present + XAudio2 SFX)

# dartui: the GUI host. dart.exe's recipe + DART_UI_HOST (main.cc runs the Win32
# pump instead of Dart_RunLoop) + dart_win32. Snapshot-based like dart.exe; the
# snapshot already carries dart:win (gen_snapshot loads it). main.cc also gets
# DART_PRECOMPILER (per-source) to elide its Observatory-assets zlib path.
add_executable(dartui
  ${RT}/bin/builtin_nolib.cc ${RT}/bin/builtin_natives.cc ${RT}/bin/error_exit.cc
  ${RT}/bin/main.cc ${RT}/bin/loader.cc ${RT}/bin/snapshot_utils.cc
  ${RT}/bin/observatory_assets_empty.cc ${RT}/bin/vmservice_impl.cc
  ${GEN}/snapshot_gen.cc ${GEN}/resources_gen.cc
  ${WIN32DIR}/dartui.rc)                    # RT_MANIFEST: Common-Controls v6 + DPI
target_compile_definitions(dartui PRIVATE DART_UI_HOST)
# We embed our OWN manifest via dartui.rc (RT_MANIFEST id 1); tell the linker not
# to also auto-generate one, else CVTRES fails with "duplicate resource MANIFEST".
target_link_options(dartui PRIVATE /MANIFEST:NO)
set_source_files_properties(${RT}/bin/main.cc TARGET_DIRECTORY dartui
  PROPERTIES COMPILE_DEFINITIONS DART_PRECOMPILER)
target_link_libraries(dartui PRIVATE dart_io dart_builtin dart_engine_snap dart_win32 ${WIN_SYS_LIBS})
# Stage 2: place the core snapshot .bin next to dartui.exe so it can boot from
# disk (windart_load_snapshots_from_disk) and be swapped without a relink.
add_custom_command(TARGET dartui POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${GEN}/vm_isolate_snapshot.bin ${GEN}/isolate_snapshot.bin
          $<TARGET_FILE_DIR:dartui>
  VERBATIM)

# Report the compile set size at configure time.
list(LENGTH VM_CC _n_vm)
list(LENGTH LIB_CC _n_lib)
list(LENGTH GENERATED_LIB_CC _n_gen)
list(LENGTH BUILTIN_CC _n_bi)
list(LENGTH IO_CC _n_io)
message(STATUS "WINDART S4: ${_n_vm} VM+platform, ${_n_lib} lib-native, ${_n_gen} gen core-lib, ${_n_bi} builtin-impl, ${_n_io} io; + dart_win32 + dartui")
