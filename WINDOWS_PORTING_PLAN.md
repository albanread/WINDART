# WINDART — Windows x64 Porting Plan

**Goal:** A native **Windows x64 JIT** build of the Dart **V1** VM (1.24.3) with
its live IDE, using **Win32 + Direct2D/Direct3D** — no Cocoa, no Metal, no
WebView. The Windows equivalent of MACDART's `PORTING_PLAN.md`.

**Sources:**
- `e:\windart\MACDARTV1` — the MACDART project (arm64/macOS port + native IDE).
- `e:\dart_origins\sdk-1.24.3` — pristine Dart 1.24.3 (the reference quarry).
- `E:\WINVM` — a Rust Smalltalk VM with a shipping Win32 host (reuse reference).

Design decision (locked, 2026-07-29): the IDE chrome is re-hosted as a
**native Direct2D view-server**, not a `dart:win` dynamic bridge and not
WebView2. This unifies the whole GUI on one protocol (see §4).

---

## 1. The core insight — the hard part was already shipped

MACDART's mac port rests on one fact: Dart 1.24.3's arm64 backend existed but
had **never run as a JIT** on Apple hardware (iOS used it AOT-only), so the port
had to *invent* arm64-JIT-on-macOS (~10 lines + an entitlement).

**The Windows port inherits a capability that shipped in production in 2017.**
Dartium and the standalone `dart.exe` were first-class Windows x64 JITs. Verified
against `e:\dart_origins\sdk-1.24.3`:

- **Complete x64 JIT backend** — all 17 `runtime/vm/*_x64.cc`: `assembler_x64`,
  **`code_patcher_x64`** (self-modifying code — the JIT-defining capability),
  `flow_graph_compiler_x64`, `stub_code_x64`, `intrinsifier_x64`,
  `runtime_entry_x64`, `instructions_x64`, `debugger_x64`, `disassembler_x64`.
- **Complete Windows OS layer** — `virtual_memory_win.cc`, `os_win.cc`,
  `os_thread_win.cc`, `signal_handler_win.cc`, `thread_interrupter_win.cc`,
  `cpuinfo_win.cc`, `native_symbol_win.cc`.
- **Complete Windows embedder (`dart:io`)** — 17 `runtime/bin/*_win.cc`:
  `file`, `directory`, `socket`, `socket_base`, `process`, `stdio`,
  `eventhandler`, `file_system_watcher`, `platform`, `sync_socket`, `crypto`,
  `security_context` (TLS — we defer it), `extensions`.

**None of the three mac patches apply to Windows:**

| Mac fix | Windows status |
|---|---|
| `CPU::FlushICache` via `sys_icache_invalidate` | **Not needed** — `cpu_x64.cc:24` is a documented no-op ("Nothing to be done here"): x86 has a coherent I-cache. |
| `MAP_JIT` on executable allocations | **Not needed** — an Apple hardened-runtime concept; no Windows analog. |
| Apple-Silicon `str SP,[SP,#-8]!` trap | **Not applicable** — arm64-only illegal-instruction quirk. |

**W^X already works on Windows out of the box.** `virtual_memory_win.cc` maps
executable pages and flips `PAGE_EXECUTE_READ ↔ PAGE_EXECUTE_READWRITE` via
`VirtualProtect` (lines 87–94). The `FLAG_write_protect_code=true` page-flip
chain the mac plan identified as arch/OS-neutral runs unchanged.

**Consequence:** the Windows **VM-core delta is ~zero source changes.** The one
shared, non-arch fix worth keeping from the mac patch is the
`flow_graph_compiler.cc` null-guard (a clang-17 UB fix; harmless and defensive
under MSVC/clang-cl). Everything else in the port is **build system + GUI**, not
VM.

---

## 2. What we inherit vs. what we build

| Concern | Status in 1.24.3 | Action |
|---|---|---|
| x64 codegen (assembler/compiler/stubs/intrinsics/patcher), JIT-proven | ✅ Shipped 2017 | Extract as-is |
| Object model, generational GC, isolates, in-VM V1 parser | ✅ Mature | Extract as-is |
| Core libraries (`dart:core`, `async`, …) | ✅ Dart source | Extract; snapshot at build |
| W^X page-flip infra (`write_protect_code`, `VirtualProtect`) | ✅ **Present, works on Windows** | **Reuse as-is** |
| Windows OS layer (vm) + embedder (bin, `dart:io`) | ✅ Complete (Dartium) | Extract; select `_win.cc` |
| `CPU::FlushICache` / `MAP_JIT` / SP-trap | ✅ No-op / N/A on x64 | **Nothing to do** |
| Embedder integration (register natives, control-plane API, eval/reload) | MACDART's own | Reuse the OS-neutral half of `macdart-port.patch` |
| Build system | gyp/GN/gclient | **Replace** — adapt MACDART's CMake + Python |
| **Native GUI** (`dart:cocoa`, Metal engine, thread-0 host) | Cocoa/Metal | **Rewrite** as Win32/Direct2D/Direct3D (§4–§6) |
| TLS (`dart:io` secure socket) | Present (`security_context_win`) | Defer behind `SECURE_SOCKET_DISABLED` (as mac does) |

---

## 3. The port boundary — three layers

```
┌─ Layer 3: PORTABLE (unchanged) ─────────────────────────────────────────┐
│  All Dart: demos/*, games (invaders/brickout/pong), apps/*,             │
│  language.dart (hot-reload orchestration), the debugger, the isolate    │
│  graph, the pull-paced frame protocol, the retained-scene game wire.    │
│  Portable C++: gp_synth.cc (SFX synth), BuildSmf (MIDI), sqlite_natives,│
│  workspace_natives (Dart_EvaluateExpr / ReloadSources). TCL control     │
│  plane (regress.tcl, dartui.tcl — headless tests survive verbatim).     │
├─ Layer 2: THE WORK — native platform library `dart_win32` ──────────────┤
│  win_host (message pump) · win_natives (resolver + view-server ops) ·   │
│  win_callbacks (WNDPROC→ticket→Dart) · d2d_canvas (Direct2D/DirectWrite)│
│  · gp_engine_d3d (Direct3D 11) · gp_audio (XAudio2) · win.dart (API)    │
├─ Layer 1: BUILD SYSTEM — mechanical re-parameterization ────────────────┤
│  gen_sources.py filters · CMakeLists arch/OS/libs · extract for Windows │
├─ Layer 0: VM CORE — inherited, ~0 changes ──────────────────────────────┤
│  Upstream 1.24.3 x64 JIT + Windows OS layer + Windows embedder.         │
└─────────────────────────────────────────────────────────────────────────┘
```

**The unifying idea (why the user's Direct2D-view-server choice is the right
one).** MACDART already runs *user apps* as a **view-server**: the app never
imports `dart:cocoa`; it *describes* widgets (`build(ui)`) and *receives* events
over isolate ports, and the UI isolate materializes real controls
(`APP_PANE_PLAN.md`). Demos push draw-commands; games push retained-scene
deltas — all over the same message discipline. The **only** code that calls the
native UI directly today is the IDE chrome itself (`workspace.dart`, 4,853 LOC,
133 `dart:cocoa` sites), because it *is* the UI isolate.

The Windows port **makes the IDE chrome a view-server client too.** Instead of
`workspace.dart` calling `NSWindow`/`NSTextView` through a dynamic bridge, it
describes its class browser / editor / tabs / tables over the same widget
protocol the app pane already uses, and **one C++ Direct2D materializer** serves
the chrome, the app pane, and the demo canvas alike. This deletes the
`objc_msgSend` problem instead of solving it, and it is "pure Windows API +
DirectX" end to end.

---

## 4. Layer 1 — Build system port (mechanical, well-bounded)

The arch/OS **compile** selection is astonishingly small: two regexes plus a few
CMake variables.

**`macdart/port/gen_sources.py` — invert the two filters:**
```python
# KEEP x64; drop the other backends (arm/arm64/ia32/mips/dbc/simulator).
OTHER_ARCH_RE = re.compile(r"(_ia32[\._]|_arm64[\._]|_arm[\._]|_mips[\._]|_dbc[\._]|^simulator_)")
# KEEP win; drop the other host OSes.
OTHER_OS_RE   = re.compile(r"(_linux[\._]|_macos[\._]|_android[\._]|_fuchsia[\._]|_openbsd[\._]|_solaris[\._])")
```
(Note `_x64` is now *absent* from the arch drop-list, so it survives; `_win` is
absent from the OS drop-list, so it survives. Everything else is filtered out.)

**`macdart/CMakeLists.txt` deltas:**
- Defines: `TARGET_ARCH_ARM64` → `TARGET_ARCH_X64`; drop `CMAKE_OSX_ARCHITECTURES`;
  the OS auto-derives from `__APPLE__` today → force `TARGET_OS_WINDOWS` /
  `HOST_OS_WINDOWS` (and `_WIN32` is compiler-defined).
- `malloc_hooks_arm64` exclude → `malloc_hooks_x64`.
- `dart_builtin`: `log_macos.cc` → `log_win.cc`.
- `dart_io` (explicit file list): swap every `*_macos.cc` → `*_win.cc`
  (`eventhandler_win`, `file_system_watcher_win`, `platform_win`, `process_win`,
  `socket_base_win`, `socket_win`, `stdio_win`, `sync_socket_win` — all present
  upstream). Keep TLS off initially (`secure_socket_unsupported`,
  `root_certificates_unsupported`, `io_service_no_ssl`), exactly as mac does.
- `MACOS_FRAMEWORKS` → Windows system libs: `ws2_32 iphlpapi rpcrt4 shell32
  advapi32 ole32 psapi` (+ the GUI libs go on `dart_win32`, below).
- **`dart_cocoa` → `dart_win32`** (§5): replace the `.mm/.m` sources; link
  `d2d1 dwrite windowscodecs d3d11 dxgi d3dcompiler xaudio2 comctl32 dcomp`.
- Objective-C flags (`-fno-objc-arc -fobjc-exceptions`) → removed; MSVC/clang-cl
  flags instead (`/EHsc`, `/std:c++14`). `-rdynamic` → not needed (Windows
  exports via the resolver table, not dynamic symbols).
- The snapshot pipeline (`gen_snapshot` → `.bin` → `create_snapshot_file.py`)
  and all `port/*.py` generators are OS-neutral Python — **unchanged**.

**`extract.sh` → Windows.** It is bash + `rsync` + `patch`. Git-Bash is present
here, but `rsync`/`patch` may not be — port it to a `extract.py` (shutil-based
copy with the same exclude globs; `patch` via `git apply` or a Python patcher).
The `zlib.h` shim redirects to the system zlib on mac; on Windows, vendor the
zlib source or use a prebuilt (no system zlib on Windows).

**`macdart-port.patch` — split it.** The 3 arm64/clang fixes drop; the rest is
OS-neutral embedder integration and **carries over**: `bin/builtin*.cc`,
`dartutils`, `gen_snapshot.cc`, `main.cc` (native-lib registration for
`dart:win`), `invocation_mirror_patch.dart`, `dart_api_impl.cc` +
`dart_tools_api.h` (the `Dart_EvaluateExpr` / reload / debugger control-plane
primitives that `HOTRELOAD_DESIGN.md` documents). Regenerate as
`windart-port.patch`.

**Toolchain choice.** Prefer **clang-cl** over MSVC: it reuses the mac port's
clang-17 fixes and 2017 Dart compiled clean under clang. MSVC is viable but
stricter (more `#pragma`/`__declspec` friction across 435k lines of 2017 C++).
Either way, front-load Phase W0 to convert unknowns into a concrete error list.

---

## 5. Layer 2 — The native platform library `dart_win32`

Replaces `dart_cocoa`. **Keep the architecture; swap the platform.** What is
preserved verbatim (all confirmed portable by the boundary analysis):

- **The resolver table** (`COCOA_NATIVE_LIST` macro-table matching
  name+argc → `Dart_NativeFunction`, `cocoa_natives.mm:516`) — the ABI contract
  shape stays; only the entries and bodies change.
- **The wire contract** — native objects as int64 handles in a wrapper; strings
  as UTF-8 (`Dart_StringToCString`); rects/points as `List<num>`; bulk pixels as
  zero-copy `Dart_NewExternalTypedData`. **Unchanged.**
- **Ticket-based callbacks** — native stores an integer ticket, never a Dart
  handle; one Dart dispatcher `_cocoaDispatch(ticket, kind, arg)` via
  `Dart_InvokeClosure`; dead ticket fails closed. Ports **directly** onto
  `WNDPROC`/`SetWindowSubclass`.
- **GC-finalizer refcount discipline** — `Dart_NewWeakPersistentHandle`
  finalizer → release. Maps onto COM `AddRef`/`Release` (or `delete` for owned
  C++ objects).

What is **deleted** (no Windows equivalent): `cocoa_abi.cc` (`@encode`→AAPCS64
classifier), `objc_shim.m` (the fixed-shape `objc_msgSend`). The dynamic
"call any selector by name" model is replaced by a **fixed catalog of concrete
native ops** — and because the IDE chrome is now a view-server (§3), that
catalog is the *widget/canvas/game protocol*, not a general Win32 binding.

**Components (new C++ / reused):**

| File | Replaces | Does | Reference |
|---|---|---|---|
| `win_host.cpp` | `cocoa_host.mm` | Win32 message pump on the UI thread; `Dart_SetMessageNotifyCallback` posts `WM_APP+n` via `PostMessage` to wake `Dart_HandleMessages()`; host-driven UI hot-reload at pump top. ~200 lines. | **WINVM `gui/src/shell/win.rs:159-277,382-412,685-693`** — window, pump, `PostMessageW`/`WM_APP` wakeup, the "read HWND atomically at notify-time" bug-fix. |
| `win_natives.cpp` | `cocoa_natives.mm` | Resolver table + concrete ops: create/destroy/place widgets (view-server materializer), set text/state, open/save dialogs (`IFileOpenDialog`). | Keep the resolver+wire pattern; bodies are Win32. |
| `win_callbacks.cpp` | `cocoa_callbacks.mm` | `WNDPROC` + subclass procs → ticket → Dart dispatcher; table data-source; syntax-span apply on the editor; **key-state bitset** via `GetAsyncKeyState`/raw input, read once per frame (matches the existing poll-not-event model). | ticket-table design preserved. |
| `d2d_canvas.cpp` | the `NSImage`/`NSBezierPath` canvas | Direct2D render target + geometry (`ID2D1*`), DirectWrite text, WIC for PNG/BMP. Serves the demo canvas AND the IDE chrome's custom-drawn widgets. | — |
| `gamepane/gp_engine_d3d.cpp` | `gp_engine.mm` | Direct3D 11 engine (§6). | MSL→HLSL translation. |
| `gamepane/gp_audio.cpp` | AVFoundation in `gp_engine.mm` | XAudio2 SFX playback; `midiOut*` or a bundled soft-synth for tunes. | — |
| `gp_synth.cc`, `BuildSmf`, `sqlite_natives.cc`, `workspace_natives.cc` | — | **Reused as-is** (pure C++/embedder-API). | bundle SQLite amalgamation. |
| `win.dart` | `cocoa.dart` | The Dart-facing API: the view-server widget vocabulary + canvas + gamepane. A thin `noSuchMethod` sugar over *named ops* is optional; the dynamic-selector version is gone. | — |

**The editor is the hard widget.** `workspace.dart` uses `NSTextView` +
`NSTextStorage` for the code editor with syntax-colored spans
(`Cocoa_applySpans`). Windows options, in preference order: (a) a **Direct2D/
DirectWrite custom text view** (full control, matches the "pure DirectX" intent,
most work); (b) **RichEdit** (`CF_UNICODETEXT`, `EM_SETCHARFORMAT` for spans —
fastest to working, least control); (c) embed **Scintilla** (a real code-editor
control, BSD — pragmatic middle). Recommend (b) for bring-up, (a) as the
finished form. This is the single biggest Layer-2 line item.

---

## 6. Layer 2 — The game pane on Direct3D 11

The one subsystem with real GPU coupling. The **wire is unchanged**: a whole
frame crosses in one `gpApply(cmds)` call (~40 verbs), applied atomically;
offscreen-render-then-present; a zero-copy direct framebuffer via
`ExternalTypedData`. Only the engine beneath changes. Metal → D3D11 mapping:

| Metal (today) | Direct3D 11 (port) |
|---|---|
| `MTLDevice`, `MTLCommandQueue` | `ID3D11Device`, `ID3D11DeviceContext` |
| `CAMetalLayer` + `nextDrawable`/`presentDrawable` | `IDXGISwapChain` + `Present` (or `DComp` for a composed layer) |
| Offscreen `MTLTexture` (BGRA) + `MTLBlitCommandEncoder` copy | offscreen `ID3D11Texture2D` RTV + `CopyResource`/full-screen blit |
| `R8Uint` index textures + per-scanline palette `MTLBuffer` | `DXGI_FORMAT_R8_UINT` textures + a constant/structured buffer palette |
| Palette-lookup fragment shader (MSL, runtime `newLibraryWithSource:`) | HLSL pixel shader (runtime `D3DCompile`) |
| 4 compute kernels (blitter copy/transparent/minterm/clear) | HLSL compute shaders (`CSSetShader`, `Dispatch`) |
| Sprite alpha-blended pipeline, seven-seg text overlay | same, D3D11 blend state + a text RTV |
| Runtime-compiled shader background (demo MSL) | runtime `D3DCompile` of the demo's HLSL body |
| `getBytes` readback for `gpsnap` (offscreen, honest pixels) | `CopyResource` to a `STAGING` texture + `Map` |
| `Dart_NewExternalTypedData` over 3 shared `MTLBuffer`s (direct FB) | `ExternalTypedData` over a `D3D11_USAGE_DYNAMIC`/staging ring, triple-buffered |

The MSL shaders are small and inlined (`gp_engine.mm:17-148`) — mechanical to
translate. `gp_synth.cc` (the synthesizer) and `BuildSmf` (MIDI) are pure C++,
reused. `MacGamePaneKeyView`/`HELD_KEYS` is **not ported** (input is the
per-frame key-state bitset, §5). Audio playback → XAudio2.

---

## 7. Reuse map — honest accounting

**From upstream Dart 1.24.3 (the biggest reuse, and free):** the entire VM, the
Windows OS layer, the Windows embedder, and the x64 JIT. This is a *shipped*
capability, not a reconstruction.

**From MACDART itself:** all portable Dart (every demo, both full games, the
apps, `language.dart`, the workspace *logic* to be re-hosted, the debugger,
the pull-pacer, the isolate graph); `gp_synth.cc`, `BuildSmf`, `sqlite_natives`,
`workspace_natives`; and the *patterns* — resolver table, wire contract,
ticket-dispatch, the view-server protocol (from `APP_PANE_PLAN`), the OS-neutral
half of the embedder patch. This is the bulk of the line count.

**From WINVM (`E:\WINVM`) — real but narrower than "much":** the WINVM inventory
corrected a premise. WINVM's Windows GUI is **WebView2, not Direct2D**, so for
the native path chosen here it has *no rendering code to lift* — only:
- **`gui/src/shell/win.rs`** — the single most valuable WINVM asset: a complete,
  loosely-coupled **Win32 host reference** (window creation, `GetMessage` pump,
  the `PostMessageW`/`WM_APP` worker→UI wakeup, `HMENU`, clipboard, DPI
  `PER_MONITOR_AWARE_V2`). Transliterate to C++ for `win_host.cpp`. The WebView2
  half is dropped.
- **`src/runtime/winkb.rs` + `E:\windows_api\windows_api.db`** — a Win32/COM
  signature knowledge base; useful *later* if WINDART adds a `dart:ffi`→Win32
  bridge. Not needed for the view-server.
- **Patterns**, not code: the VM↔UI threading bridge (`vm_host.rs` — single-
  outstanding backpressure, respawn-on-hang, batch-drain), and the retained-mode
  `GameCommand` IR design.
- **Superseded:** WINVM's JIT executable-memory (`native_windows.rs`) and
  lazy-commit heap (`reservation.rs`) are *not* reused — the Dart VM brings its
  own, better-suited Windows `virtual_memory_win.cc`/`code_patcher`/heap from
  upstream. Reusing WINVM's here would be a step backward.

Net: WINVM contributes the **Win32 host shell reference** and **architectural
patterns**; upstream Dart contributes the **VM + JIT + platform**; MACDART
contributes **everything above the platform line**. State this plainly so the
"reuse MacVM" expectation is calibrated: it's the host-shell + patterns, not a
liftable Windows renderer.

---

## 8. Phased plan

**W0 — Owned tree builds; `dart.exe` runs JIT code.** Port `extract` +
`gen_sources` filters + CMakeLists for clang-cl; compile the VM core +
`gen_snapshot` + `dart`. First: build *pristine* upstream 1.24.3 for Windows to
validate the toolchain, then layer the OS-neutral embedder patch. **Exit:**
`dart.exe hello.dart` executes JIT-compiled x64 and prints. (Low risk — upstream
capability; the milestone the mac port had to *fight* for is nearly free here.)

**W1 — `dart:io` + embedder integration.** Bring up file/socket/process/stdio
(`*_win.cc`, TLS deferred); apply the eval/reload/control-plane primitives.
**Exit:** real V1 programs run; `Dart_EvaluateExpr` + `ReloadSources` work
(hot-reload primitive live).

**W2 — Host + view-server + first control.** `win_host.cpp` (pump, reusing
WINVM's `win.rs`), `win_natives.cpp` + `win_callbacks.cpp`, a minimal widget set
(window, button, label). **Exit:** `dartui.exe` opens a window with a button
whose action is a Dart closure — the app-pane milestone.

**W3 — Direct2D canvas.** Port the demo 2D drawing (`d2d_canvas.cpp`). **Exit:**
the non-game demos (mandelbrot, life, boids, plasma, globe, attractor…) render
over the unchanged draw-command protocol.

**W4 — Direct3D game pane.** Translate the 5 layers to D3D11/HLSL, the compute
blitter, the direct framebuffer, XAudio2 (`gp_synth.cc` reused). **Exit:** Pong,
then Sprite Invaders and Brickout; `gpsnap` PNG readback for headless verify.

**W5 — The IDE chrome as a view-server.** Re-host `workspace.dart` (4,853 LOC)
onto the widget protocol: class browser, editor (RichEdit→Direct2D), tabs,
tables, menus, the debugger tab. The largest Dart-side effort; unifies with the
app pane. **Exit:** the full live workspace IDE.

**W6 — Polish & packaging.** File dialogs (`IFileOpenDialog`), borderless
fullscreen, the Accept-time static check (replace the objc-runtime selector lint
with a native-op-catalog check, or retire it — the dynamic bridge it guarded is
gone), a relocatable `windart` bundle.

---

## 9. Risks (ranked for Windows)

1. **The IDE-chrome rewrite (W5).** 4,853 LOC of Dart re-hosted onto a
   view-server, *plus* a syntax-highlighting code editor on Windows. The editor
   is the crux — mitigated by staging RichEdit → Direct2D. Largest single
   effort, but bounded and non-blocking for the JIT/demo/game milestones.
2. **The D3D11 game pane (W4).** MSL→HLSL translation, the compute blitter, the
   per-scanline "copper" palette trick. GPU code, but the engine is ~1,500 lines
   and the wire is unchanged.
3. **Toolchain / 2017 C++ (W0).** 435k lines under a 2026 compiler. clang-cl
   reuses the mac port's clang fixes; MSVC would add friction. Front-loaded;
   mechanical burn-down.
4. **The dynamic-bridge deletion.** Choosing the exact native-op catalog.
   *Mitigated by the view-server decision* — the catalog is the widget protocol,
   not all of Win32. The `objc_msgSend`/`@encode` machinery simply goes away.
5. **Snapshot format on Windows x64.** Lower than the mac port's arm64 unknown —
   x64 snapshots were the *original* 2017 target; the format is well-trodden.

---

## 10. Immediate next action

Port `extract` + `gen_sources.py` + a first clang-cl `CMakeLists`, and build
**pristine upstream 1.24.3 for Windows x64** — no MACDART code yet — to prove the
toolchain and turn the 2017-C++-under-2026-clang unknowns into a concrete error
list. Everything after W0 is burn-down against a VM that already knows how to JIT
on Windows.
