# WINDART Sprint S6b — the Direct3D 11 game pane (gp_engine_d3d)

Goal: the last hard graphics piece — the D3D11 retro engine that powers the
Demos-tab arcade games (the `gp*` protocol), proven headlessly by saved gpsnap
PNGs. Built on the signed-off design (`gamepane-design/`, arch sign-off
`arch-notes/S6_design_signoff.md`).

## MILESTONE — HIT: copper AND invaders render (saved PNGs the architect can view)

| game | PNG | what it proves |
|---|---|---|
| `12_copper` | `e:\windart\build\game_copper.png` | indexed pane + **per-scanline "copper" palette** (green/blue/cyan bands at distinct scanlines) + a rotating **orb sprite** + the seven-seg **"60" HUD** |
| `13_invaders`| `e:\windart\build\game_invaders.png` | **sprites** (3×8 invader rank + scale-2 ship, per-def 16-colour palettes, animation), **indexed-pane terrain** (four bevelled bunkers via fill_rect), the score/lives **HUD**, and the runtime-shader path (graceful MSL decline, §deferred) |

Headless runner output:
```
GAME: SNAP 12_copper   frames=60 -> ...game_copper.png OK
GAME: SNAP 13_invaders frames=60 -> ...game_invaders.png OK
```

**Clip-space Y guard CONFIRMED (RISKS #4):** in both PNGs the HUD (drawn at y=6-8)
is at the TOP row and copper's screen-line-0 wash is at the top. No flip was
added; the UV math ported verbatim. Screen line 0 is at the top, as required.

## What was built (owned)
- **`dart_win32/gp_engine_d3d.{h,cpp}`** (NEW, ~950 lines) — the D3D11 engine, the
  port of `cocoa/gamepane/gp_engine.{h,mm}`:
  - **Device:** `D3D11CreateDevice` (HARDWARE, `BGRA_SUPPORT`) with a **WARP
    software fallback** so headless hosts render deterministically. Shared state
    created once: `CULL_NONE` rasterizer (the fullscreen triangle is CCW — Metal
    defaults to no-cull, D3D11 to back-cull; **this was the one make-or-break
    state**), an alpha-over blend state, a POINT/CLAMP sampler.
  - **Offscreen-first:** a persistent `B8G8R8A8_UNORM` render target; every layer
    renders into it; **gpsnap** does `CopyResource` → `USAGE_STAGING` → `Map(READ)`
    → BGRA→RGBA → **WIC** PNG (`GUID_ContainerFormatPng`, `WritePixels`). Independent
    of any swapchain — the honest-pixels readback.
  - **`GpIndexedPane`** — 8× `R8_UINT` textures (`USAGE_DEFAULT` +
    `UpdateSubresource`), the per-scanline palette as a **`StructuredBuffer<float4>`**
    SRV at t1 (`DYNAMIC`+`Map(WRITE_DISCARD)`; no 64 KB cbuffer cap — RISKS #2), the
    16-byte `IndexedUniforms` cbuffer, `ps_indexed`. All CPU drawing
    (cls/pset/line/circle/disc/fill/load, HSV default palette, buffer swap) ported
    verbatim.
  - **`GpSprites`** — `IMMUTABLE` R8_UINT frame textures, per-def 16-elem palette
    StructuredBuffer, a `DYNAMIC` vertex buffer + `{POSITION,TEXCOORD}` input layout,
    alpha-over blend, `Draw(4)` TRIANGLESTRIP per instance. Parse/tick/hit/place and
    the rotation/scale/scroll vertex math verbatim.
  - **`GpTextOverlay`** — `DYNAMIC` RGBA8 texture (`Map(WRITE_DISCARD)`, row-pitch
    honoured), the seven-seg raster verbatim, POINT sampler, `ps_text`.
  - **`GpShaderPane`** — runtime `D3DCompile(header + body)`; compile errors returned
    as a string, never an abort (RISKS #6); the **144-byte** cbuffer layout (each
    `p[i]` in its own 16-byte slot — RISKS #6).
  - **`GpBlitter`** — CPU-mirror + reupload for **all** modes (see burn-down).
  - HLSL: `shaders.hlsl` split into one source string per pipeline (each `register(b0)`
    compiles in isolation).
- **`dart_win32/gp_natives_win.cpp`** (NEW) — the 7 `Win_gp*` natives, the port of
  `gp_natives.mm`: the whole-frame `gpApply` dispatch (~40 verbs, best-effort,
  first-error-as-string), `gpOpen/Close/Snap/Stat/Fullscreen/Backbuffer`, the wire
  validators (`ElInt/ElStr/ClampByte/DecodeB64`). The wire is byte-for-byte the mac
  contract; only the engine beneath changed.
- **Wiring:** `win_natives.cpp` — the 7 stubs became forward decls (resolver table
  unchanged). `win.dart` — `gpOpen/gpApply/gpSnap/gpStat/gpFullscreen` client + the
  5 `native "Win_gp*"` bindings. `CMakeLists.txt` — the 2 new TUs in `dart_win32`;
  `d3d11 dxgi d3dcompiler` on the link line.
- **`test/gp_runner.dart`** (NEW) — the pull-paced gp game runner: turns the first
  `['draw',[['gpopen',…]]]` into `gpOpen()`, every later frame into `gpApply()`,
  then `gpSnap()`; self-drives with no keys (untyped `[[],0]` ticks — the S6
  DEBUG cross-isolate type-args workaround). `test/demos/` gained `12_copper`,
  `13_invaders`, `15_brickout`, `gamepane.dart`, `abc.dart` (game content unchanged).

## Compile burn-down
**Clean on the first build — 0 errors, 0 gp-warnings.** `gp_engine_d3d.cpp` and
`gp_natives_win.cpp` compiled first try under `/std:c++14 /permissive`; the full
`dart_win32` + `gen_snapshot` + snapshot regen + `dartui`/`dart` relink was 37 ninja
steps, all green. `Microsoft::WRL::ComPtr` for all D3D/DXGI/WIC lifetimes (RAII;
`close()` = reset the ComPtrs). The Windows SDK's `d3d11.h`/`d3dcompiler.h`/`wincodec.h`
compiled as-is beside the existing d2d1/dwrite path.

## The 3 signed-off decisions — applied
1. **Blitter min-term** — shipped the CPU-mirror baseline, extended to **all** modes
   (copy/transparent/and/or/xor/clear compute into the destination's CPU buffer, then
   the slot is marked dirty and `upload()` re-pushes it via `UpdateSubresource`).
   Always correct, needs **no** `R8_UINT` UAV support at all (sidesteps RISKS #1 and
   the DYNAMIC-vs-UAV collision, RISKS #5). The GPU compute-blitter (`cs_blit_*`) is
   the cap-gated optimisation — deferred (invaders' bunker chips are tiny CPU clears).
2. **Direct framebuffer** — the Dart external-typed-data contract is preserved in the
   wire; the pane itself is **deferred** (no shipping game uses direct mode — copper/
   invaders/brickout all open the retained mode-0 stack).
3. **Clip-space Y** — ported verbatim, no flip. Guard passes (above).

## Deferred / stubbed (honest scope — none blocks the milestone)
- **Live on-screen present** — the engine renders offscreen and snapshots (the PNG
  proof). A DXGI flip-model swapchain on a child HWND + the `vs_present`/`ps_present`
  letterbox blit + a `game` widget kind in `win_view.cpp` (modelled on `kCanvas`) is
  the follow-up for the visible Demos tab. `gpsnap` already reads the offscreen, so
  the S7 regression loop needs nothing more.
- **GPU compute blitter** (`cs_blit_*`) — CPU baseline used instead (decision 1).
- **Direct framebuffer mode** (§6b) — `gpOpen(mode=1)` fails cleanly; `gpBackbuffer`
  returns null.
- **Audio** — `gpsound/gpplay/gptune/gpmusic` are accepted no-ops. XAudio2 (SFX
  source-voice pool, `gp_synth.cc` reused verbatim) + winmm `midiStream` (music) per
  `GP_AUDIO_DESIGN.md` is the remaining audio sprint. Headless PNG proof needs none.
- **Game shader bodies are MSL** — the stock demos ship `fragment float4 fmain(...)`
  (MSL), which `D3DCompile` rightly rejects (`X3000: unrecognized identifier
  'fragment'`). The `gpshader` contract handles it exactly as designed: the error is
  logged, the pane keeps its black background, the game plays on. To get invaders'
  starfield the game's shader string must be rewritten to HLSL (a game-content change,
  out of scope for "games unchanged"). Everything else in invaders renders.

## Regressions + hygiene (all clean)
- `dart.exe` hello → `WINDART dart.exe OK: 7`.
- Pong (prior game milestone, canvas) → `SNAP 11_pong frames=60 OK`.
- Canvas demo `03_lissajous` → `SNAP OK`, 46 KB (== S5).
- Quarry `e:\dart_origins\sdk-1.24.3` **git-clean** (empty `git status`); no `tree/`
  edits, `windart-port.patch` untouched (the game pane lives entirely in
  `port-win/dart_win32/`, like S4/S5's host + canvas).

## Hand-off
The retained gp stack (indexed + copper + sprites + text + runtime-shader + CPU
blitter) is live and proven. Remaining game-pane work, all additive and
non-blocking: live present + `game` widget, XAudio2/winmm audio, the direct pane,
and the GPU compute-blitter fast path.
