# WINDART T3 — the LIVE game pane (on-screen present + audio + shader backgrounds)

S6b built the D3D11 engine but rendered games **offscreen only** (gpsnap PNGs).
T3 makes them **live and playable in a window**: a real DXGI swapchain presents
every frame, XAudio2 plays the SFX, and the runtime-shader background renders.
Next in the user's order (game pane before the debugger). No quarry edits; the
whole game pane still lives in `port-win/dart_win32/`.

## MILESTONE — HIT (PNGs the architect can view, in `e:\windart\build\`)

`dartui.exe game_live.dart 13_invaders_hlsl` opens a window and the game is
VISIBLE and animating (self-play: the rank marches, bombs drop, SFX fire).

| proof | PNG | what it shows |
|---|---|---|
| animation (offscreen, frame 1) | `13_invaders_frame1.png` | the rank at its START positions |
| animation (offscreen, frame 48) | `game_invaders_live.png` | the rank has **marched right + dropped** — live animation, not a static first frame |
| **on-screen present** | `13_invaders_present.png` | the actual **swapchain backbuffer** (848×480, letterboxed) — what the live window displays |
| **HLSL shader + full scene** | `13_invaders_hlsl_present.png` | the twinkling **starfield** (layer 0) behind bunkers (layer 1), invaders + ship (layer 2), HUD (layer 3) — all four engine layers presenting live |

Runner log (`selftest`): `framesPresented=48` (real `Present()` calls), both
`gpSnap` (offscreen) and `gpSnapPresent` (on-screen readback) return OK. Without
`selftest` the game stays open and interactive (verified: pid alive after 3s;
arrow/A-D drive the ship, space fires when the window is focused).

## 1. Live on-screen present (THE core) — DXGI swapchain on a `game` widget
- **`win_view.cpp` — a new `kGame` widget kind** (modelled on `kCanvas`): a
  `STATIC | SS_OWNERDRAW | WS_CLIPSIBLINGS` child HWND (owner-draw suppresses the
  default GDI erase, so DXGI's presented frame is never overpainted). On create it
  calls `GpEngine::set_present_target(hwnd)`; on teardown (`ClearSurface`/`remove`)
  it `detach_present()`s. `OpenWindow` gained `WS_CLIPCHILDREN`.
- **`gp_engine_d3d.{h,cpp}` — the present path.** `set_present_target()` records
  the HWND; the swapchain is created **lazily** on the first present after `gpOpen`
  makes the device (order-independent). Each `render_present()`, after rendering
  the offscreen RT, does `present_blit_to_backbuffer()` + `Present(1,0)`:
  - `ensure_swapchain()` — `IDXGIFactory2::CreateSwapChainForHwnd` (FLIP_DISCARD,
    2 buffers, B8G8R8A8, STRETCH), factory pulled from the device's adapter (no
    `CreateDXGIFactory` needed); `ResizeBuffers` on client-size change.
  - `present_blit_to_backbuffer()` — a fullscreen-triangle **letterbox blit**
    (`kPresentHlsl` `vs_present`/`ps_present`) sampling the offscreen SRV; the
    letterbox is done by the **D3D viewport** (clear black, then a viewport fitted
    to the logical aspect), no per-pixel math. Screen line 0 stays at the top.
  - **`snap_present()`** — reads the swapchain backbuffer back to PNG (the honest
    "what the window shows" proof), exposed as the `Win_gpSnapPresent` native /
    `gpSnapPresent()`.
- **Driving frames:** `test/game_live.dart` — the pull-pacer (one frame per UI
  invitation, untyped `[[],0]` ticks for the DEBUG cross-isolate assert) on a 12 ms
  `Timer`; `Present(1,0)` is vsync-capped. `Ui.game(id, frame:)` describes the widget.

## 2. Audio — XAudio2 SFX (gpsound/gpplay now play real sound)
- **`gp_synth.{h,cc}` vendored VERBATIM** from macdart (pure C++, the 11 presets +
  LCG). **`gp_audio_win.{h,cpp}` — `GpSfx`** per `GP_AUDIO_DESIGN.md §1`: one
  `IXAudio2` + mastering voice, a **24-voice pool** for summed polyphony, 64 sample
  slots; the synth's f64-interleaved → **f32-interleaved** (XAudio2 wants
  interleaved — simpler than the mac's deinterleave); replaced buffers are
  **retired** (kept alive) not freed, drained when all voices go idle
  (the §1.4 lifetime hazard). Graceful-degrade: a failed init makes define/play
  no-ops, the game plays on.
- **`gp_natives_win.cpp`** — `gpsound` (synth a preset into a slot) and `gpplay`
  (submit on the next idle pool voice) are now real; each logs a `GP_SFX:` line.
  Verified: `audio on`; invaders defined `zap`/`explode`/`hurt`; a `coin`
  smoke-test **`play … -> submitted`** (a buffer reached a voice end-to-end).
- Engine holds `sfx()` (lazy, **process-lifetime** — survives game close/re-open).

## 3. Shader backgrounds (stretch) — HLSL starfield
- `demos/13_invaders_hlsl.dart` — a Windows variant of invaders whose `kSky` is
  rewritten from **MSL → HLSL** (`fract`→`frac`, `in.uv`→`input.uv`, `u.time/aspect`
  → the `ShaderUniforms` cbuffer globals declared by `kShaderHeaderHlsl`, scalar
  `float3(x)` splat spelled out; stars tuned brighter/denser for a visible night
  sky). `D3DCompile` accepts it (no `X3000` decline) and the twinkling starfield
  renders live. The **stock `13_invaders.dart` is unchanged** (keeps its MSL sky
  for macdart portability; on Windows that body still declines gracefully to black
  — the `gpshader` contract). One game ported; the rest are the same per-game
  string edit (documented, not done).

## 4. GPU compute blitter + direct framebuffer (optional) — DEFERRED
The CPU-mirror blitter is already correct (invaders' bunker chips render), so the
cap-gated `cs_blit_*` GPU path is a no-visible-difference optimisation; and no
shipping game opens direct mode (`gpOpen(mode=1)` still fails cleanly). Left as
S6b left them — pure speed, no milestone value.

## Bugs / gotchas handled
- **Offscreen used as RTV then SRV in the same frame** — unbind the RTV before
  binding the offscreen as the present SRV, and null the SRV after the blit, so the
  next frame's `OMSetRenderTargets(offscreen_rtv)` has no read/write hazard (no
  D3D11 debug-layer WARNING).
- **FLIP_DISCARD readback** — `snap_present` copies the backbuffer to a staging
  texture **before** `Present` (flip discards after), then presents to stay in sync.
- **Swapchain child painting** — `SS_OWNERDRAW` + `WS_CLIPCHILDREN`/`WS_CLIPSIBLINGS`
  keep GDI from erasing over the DXGI surface.
- **DEBUG cross-isolate typed-List assert** — reused the untyped-tick workaround.

## Verification / regressions (all clean)
Build exit 0 (synth + `gp_audio_win` + the present engine + `win_view` + snapshot
regen, all green). Live invaders (present + audio + HLSL sky) verified by the 4
PNGs above. Regressions: `dart.exe` `hello, windart`; **offscreen** S6b `12_copper`
+ `13_invaders` still render (the engine change didn't touch the offscreen path);
`03_lissajous` 46687b; Pong 3247b; **all 8 workspace tabs** (T1/T2) snapshot OK;
the live game stays open interactively. Quarry `e:\dart_origins\sdk-1.24.3`
pristine; `windart-port.patch` untouched; all T3 code in `port-win/dart_win32/` +
`test/`.

## Deferred / open (honest scope)
- **Tunes** (`gptune`/`gpmusic`) — accepted no-ops. winmm `midiStream` + GS
  Wavetable (`GP_AUDIO_DESIGN.md §2.1`) is the remaining audio piece; SFX was the
  "actual sound" priority. (Invaders scores a looping theme; it is silent for now.)
- **Real fullscreen** — `gpFullscreen`/`gpfull` set a flag only; no exclusive/
  borderless swapchain transition yet (games call it on **F**, which self-play
  never presses).
- **GPU compute blitter + direct framebuffer** — Part 4 above.

## Next (user's order): T4 — the debugger (the vm-service client), the last item.
