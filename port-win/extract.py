#!/usr/bin/env python3
# WINDART Phase 0 (Sprint S1) — deterministic extraction from the read-only
# reference tree into an owned Windows source tree.
#
# Python port of macdart/port/extract.sh (bash + rsync + patch). Copies the
# subset of dart-lang/sdk @ 1.24.3 needed to build a windows-x64 JIT `dart`
# into e:\windart\tree. The reference quarry (e:\dart_origins\sdk-1.24.3) is
# NEVER modified.
#
# Policy (identical to the mac extract):
#   * Keep ALL headers and arch/OS source files on disk (cheap; prevents broken
#     #includes). The COMPILE set is narrowed later, in CMake, by arch/OS/test
#     filters — see port/gen_sources.py. "Files present" is generous; "files
#     compiled" is precise. The two are deliberately decoupled.
#   * Prune only what is large and definitely unused: unit tests (*_test.*) and
#     the browser-only sdk/lib libraries (html/js/web_*/...).
#
# Idempotent: re-running re-syncs (target subtrees are removed then re-copied).
from __future__ import annotations

import os
import shutil
import subprocess
import sys

# --- Fixed locations (this is a Windows-only, path-pinned port) --------------
HERE = os.path.dirname(os.path.abspath(__file__))          # e:\windart\port-win
SRC = r"e:\dart_origins\sdk-1.24.3"                        # reference quarry (RO)
DEST = r"e:\windart\tree"                                   # owned Windows tree
PATCH = os.path.join(HERE, "windart-port.patch")           # optional (see below)

# Test/junk excludes applied to every runtime subtree copy (fnmatch on basename).
COMMON_EXCLUDES = ("*_test.cc", "*_test.h", "*_test_*.cc", ".git")

# Browser-only core libraries we will never target (excluded from sdk/lib).
SDKLIB_DIR_EXCLUDES = {
    "html", "js", "js_util", "web_audio", "web_gl", "web_sql",
    "indexed_db", "svg", "_blink", "_chrome",
}


def log(msg: str) -> None:
    sys.stdout.write("extract: " + msg + "\n")
    sys.stdout.flush()


def _ignore_common(_dir, names):
    """shutil ignore callback: drop test files and .git anywhere in the tree."""
    import fnmatch
    drop = set()
    for name in names:
        for pat in COMMON_EXCLUDES:
            if fnmatch.fnmatch(name, pat):
                drop.add(name)
                break
    return drop


def _ignore_sdklib(dirpath, names):
    """Like _ignore_common, plus drop the browser library subdirs at sdk/lib root."""
    drop = _ignore_common(dirpath, names)
    # Only prune the browser dirs at the top level of sdk/lib.
    if os.path.normpath(dirpath) == os.path.normpath(os.path.join(SRC, "sdk", "lib")):
        for name in names:
            if name in SDKLIB_DIR_EXCLUDES:
                drop.add(name)
    return drop


def copy_tree(src_rel: str, dest_rel: str, ignore=_ignore_common) -> None:
    s = os.path.join(SRC, src_rel)
    d = os.path.join(DEST, dest_rel)
    if not os.path.isdir(s):
        raise RuntimeError(f"missing source subtree: {s}")
    if os.path.isdir(d):
        shutil.rmtree(d)                       # rsync --delete equivalent
    shutil.copytree(s, d, ignore=ignore)


def copy_file(src_rel: str, dest_rel: str, optional: bool = False) -> None:
    s = os.path.join(SRC, src_rel)
    d = os.path.join(DEST, dest_rel)
    if not os.path.exists(s):
        if optional:
            return
        raise RuntimeError(f"missing source file: {s}")
    os.makedirs(os.path.dirname(d), exist_ok=True)
    shutil.copy2(s, d)


def apply_windart_patch() -> None:
    """Apply ALL WINDART source edits to the extracted tree in one shot via
    GNU `patch`, from the single source of truth port-win/windart-port.patch:
      * S1  platform/globals.h    - version-guard 2 obsolete CRT macros.
      * S3  vm/dart_api_impl.cc + include/dart_tools_api.h - workspace reload API.
      * S4  the 8 cocoa->win embedder hunks (bin/main.cc DART_UI_HOST host hook;
            builtin.{h,cc}/builtin_nolib.cc/dartutils.{h,cc} dart:win registration;
            builtin_natives.cc WinNativeLookup fallthrough; gen_snapshot.cc load).
    The quarry stays pristine; the patch applies against the fresh pristine copy
    this extract just made. Run from a shell where `patch` is on PATH (git-bash);
    GNU patch is used rather than `git apply` (which mangles absolute Windows
    --directory paths). The 3 arm64/clang fixes + the dart:win named-arg hunk are
    intentionally excluded (N/A on x64/MSVC; the selector bridge is gone).
    """
    if not os.path.isfile(PATCH):
        raise RuntimeError(f'missing patch: {PATCH}')
    patch_exe = shutil.which('patch')
    if patch_exe is None:
        for cand in ('C:/Program Files/Git/usr/bin/patch.exe',
                     'C:/Program Files/Git/bin/patch.exe'):
            if os.path.isfile(cand):
                patch_exe = cand
                break
    if patch_exe is None:
        raise RuntimeError('GNU `patch` not found on PATH - run extract.py from git-bash')
    with open(PATCH, 'rb') as fh:
        subprocess.run([patch_exe, '-p1', '-d', DEST], stdin=fh, check=True)
    log(f'applied windart-port.patch (13 files) via {os.path.basename(patch_exe)}')


def count_ext(root_rel: str, ext: str) -> int:
    n = 0
    for _root, _dirs, files in os.walk(os.path.join(DEST, root_rel)):
        n += sum(1 for f in files if f.endswith(ext))
    return n


def main() -> int:
    log(f"SRC={SRC}")
    log(f"DEST={DEST}")
    if not os.path.isfile(os.path.join(SRC, "tools", "VERSION")):
        sys.stderr.write(f"ERROR: reference SDK not found at {SRC}\n")
        return 1

    os.makedirs(DEST, exist_ok=True)

    # --- VM engine (x64 + portable; other arches kept on disk, unbuilt) ------
    copy_tree("runtime/vm", "runtime/vm")
    # --- Platform abstraction layer ------------------------------------------
    copy_tree("runtime/platform", "runtime/platform")
    # --- Core-library native method implementations (+ patch .dart sources) --
    copy_tree("runtime/lib", "runtime/lib")
    # --- Embedder: dart:io, builtin, main() (+ builtin/io .dart sources) -----
    copy_tree("runtime/bin", "runtime/bin")
    # --- Public embedding API headers (include/dart_api.h, ...) --------------
    copy_tree("runtime/include", "runtime/include")
    # --- Vendored dependency: double-conversion (grisu float<->string) -------
    copy_tree("runtime/third_party/double-conversion",
              "runtime/third_party/double-conversion")
    # --- Core libraries, as Dart source (compiled to C arrays at build time) -
    copy_tree("sdk/lib", "sdk/lib", ignore=_ignore_sdklib)

    # --- Reference build manifests (parsed by our CMake, not executed) -------
    copy_file("runtime/dart-runtime.gyp", "runtime/dart-runtime.gyp")

    # --- VERSION + license provenance ----------------------------------------
    copy_file("tools/VERSION", "tools/VERSION")
    copy_file("LICENSE", "LICENSE.dart", optional=True)
    copy_file("PATENTS", "PATENTS.dart", optional=True)

    # --- WINDART-owned files inside the extracted tree -----------------------
    # zlib shim. dart:io's filter.h includes "zlib/zlib.h" (a vendored path
    # absent here). macOS redirects it to the system zlib; Windows has NO system
    # zlib, so we vendor nothing this sprint: emit a stub that #errors with a
    # clear message IF it is ever compiled. filter.cc (the only compiler of it)
    # is excluded from the io build this sprint (TLS/zlib deferred — matching
    # the mac build's SSL-off posture). S1 builds only the VM core, which never
    # includes filter.h, so this stub is inert here and future-proofing only.
    zlib_dir = os.path.join(DEST, "runtime", "third_party", "zlib")
    os.makedirs(zlib_dir, exist_ok=True)
    with open(os.path.join(zlib_dir, "zlib.h"), "w", newline="\n") as fh:
        fh.write(
            "// WINDART zlib stub (Sprint S1).\n"
            "//\n"
            "// There is no system zlib on Windows and we have not vendored the\n"
            "// zlib sources yet (TLS + compression are deferred; see the mac\n"
            "// build's SSL-off posture). dart:io's filter.h includes\n"
            '// \"zlib/zlib.h\"; if that translation unit (bin/filter.cc) is ever\n'
            "// added to a build target before zlib is vendored, fail loudly\n"
            "// rather than silently miscompiling.\n"
            "//\n"
            "// To lift this: drop the real zlib into runtime/third_party/zlib\n"
            "// (or point this include at a vendored copy) and add bin/filter.cc\n"
            "// back to the dart_io target.\n"
            "#error \"WINDART: zlib is not vendored yet. bin/filter.cc (dart:io \"\\\n"
            "        \"compression) is deferred this sprint; exclude it from the \"\\\n"
            "        \"build, or vendor zlib into runtime/third_party/zlib.\"\n"
        )
    log("wrote zlib stub (runtime/third_party/zlib/zlib.h)")

    # --- Vendor SQLite (S7.3): the workspace image store -----------------------
    # The sqlite3 amalgamation (sqlite3.c + sqlite3.h) is not in the quarry; source
    # it from a local copy. Default: the libsqlite3-sys cargo-cache amalgamation on
    # this machine; override with the SQLITE_SRC env var (a dir with sqlite3.c/.h).
    sqlite_dst = os.path.join(DEST, "runtime", "third_party", "sqlite")
    sqlite_src = os.environ.get("SQLITE_SRC") or (
        r"C:\Users\alban\.cargo\registry\src"
        r"\index.crates.io-1949cf8c6b5b557f\libsqlite3-sys-0.28.0\sqlite3")
    sc, sh = os.path.join(sqlite_src, "sqlite3.c"), os.path.join(sqlite_src, "sqlite3.h")
    if os.path.isfile(sc) and os.path.isfile(sh):
        os.makedirs(sqlite_dst, exist_ok=True)
        shutil.copy2(sc, os.path.join(sqlite_dst, "sqlite3.c"))
        shutil.copy2(sh, os.path.join(sqlite_dst, "sqlite3.h"))
        log(f"vendored SQLite amalgamation from {sqlite_src}")
    else:
        log(f"WARNING: SQLite amalgamation not found at {sqlite_src} "
            "(set SQLITE_SRC); the workspace image will be unavailable")

    # WINDART source edits to tree\ (S1 CRT guards + S3 workspace API + S4
    # dart:win embedder hunks), applied from port-win/windart-port.patch.
    apply_windart_patch()

    # --- Port patch scope (what is and isn't applied) ------------------------
    # The macdart-port.patch splits into: (a) 3 arm64/clang fixes — DROPPED (N/A
    # on x64/MSVC: cpu_arm64 FlushICache, stub_code_arm64 SP trap,
    # flow_graph_compiler VisitBlocks clang-UB guard); (b) OS-neutral hunks. Of
    # (b), the two workspace hot-reload API functions are applied above (S3, via
    # patch_workspace_api). The remaining OS-neutral hunks are the *dart:win*
    # embedder-registration bits (bin/builtin*, dartutils, gen_snapshot, main.cc
    # dart:win native-lib hookup) + invocation_mirror_patch.dart — those belong to
    # S4 (the GUI/view-server bridge), not S3, and are intentionally NOT applied
    # here. All applied edits are mirrored in port-win/windart-port.patch.
    _ = (subprocess, PATCH)  # retained for a future git-apply path if desired

    # --- Report --------------------------------------------------------------
    log(f"VM .cc on disk      : {count_ext('runtime/vm', '.cc')}")
    log(f"bin .cc on disk     : {count_ext('runtime/bin', '.cc')}")
    log(f"lib .cc on disk     : {count_ext('runtime/lib', '.cc')}")
    log(f"core-lib .dart files: {count_ext('sdk/lib', '.dart')}")
    log("done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
