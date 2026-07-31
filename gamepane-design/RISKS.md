# RISKS — game pane MSL→D3D11, ranked for the S6 architect

The hardest translation points, most-uncertain first. Each names the mechanism,
the concrete gp_engine.mm reference, and the recommended resolution. Cross-refs:
`shaders.hlsl`, `GP_ENGINE_D3D_DESIGN.md` (§ numbers), `GP_AUDIO_DESIGN.md`.

---

## 1. The compute blitter's min-term read of R8_UINT (typed UAV load)   [HARDEST]

**Where:** `cs_blit_minterm` reads the destination (`d = blitDst[dpos]`) for
AND/OR/XOR — a **typed UAV load of R8_UINT** (MSL: gp_engine.mm:95-109, the
`access::read_write` dst). copy/transparent/clear only STORE (broadly supported);
only min-term LOADS.

**The unknown:** D3D11 does not guarantee typed UAV *loads* of R8_UINT. They are
gated behind `D3D11_FEATURE_D3D11_OPTIONS2::TypedUAVLoadAdditionalFormats` (+
per-format `CheckFormatSupport2`). Modern Win11 GPUs (the 2026 target) almost
always report it, but it is not universal, and getting it wrong is a silent
wrong-pixels bug, not a compile error.

**Resolution (recommended):** don't depend on it. The engine already computes the
exact AND/OR/XOR into the CPU mirror (gp_engine.mm:743-760) on every blit. Ship
the **CPU-mirror-then-reupload fallback** as the baseline for min-term: skip the
`Dispatch`, leave `dst` dirty, let the next `upload()` push the correct bytes via
`UpdateSubresource`. Enable the GPU min-term dispatch only when the cap probe
(`ensure_device`, §3) says the format is supported. copy/transparent/clear stay
on the GPU (store-only, safe). Min-term blits are rare in the shipping games
(`GAMEPANE_PLAN.md` §5b: they drive the indexed pane + copy/transparent). Net
risk after this: low. **Decide the policy explicitly** (§5.3, §12.1).

---

## 2. The per-scanline "copper" palette in D3D11

**Where:** the whole point of the indexed pane — a single flat `float4` palette
buffer where entries 1..15 are **per SCREEN scanline** (raster-locked, do NOT
scroll) and 16..255 are global. MSL keys `k = screenY*16 + ci` off the screen
row (gp_engine.mm:41); host layout is `viewport_h*16` per-line entries then 240
globals (gp_engine.mm:230, 268, 279).

**The unknown:** the buffer is indexed arbitrarily by a computed `k` up to
`viewport_h*16 + 240` (viewport_h up to 2048 → ~33 k float4 = 515 KB), which
exceeds the 64 KB cbuffer limit — a cbuffer array cannot hold it. Getting the
index arithmetic or the buffer layout subtly wrong yields plausible-but-wrong
colours per row (hard to eyeball).

**Resolution:** bind the palette as a **`StructuredBuffer<float4>` SRV at t1**
(no size cap), `DYNAMIC`+`Map(WRITE_DISCARD)` on dirty. The index math ports
byte-identical (`shaders.hlsl` §1). This is well-understood once StructuredBuffer
is chosen — the risk is purely in matching the host layout exactly. **Verify:**
raster-bars demo, gpsnap, confirm distinct per-line colours and that they stay
put when the pane scrolls (copper is screen-locked, not world-locked). Low risk
after the StructuredBuffer decision; flagged because it is the engine's signature
trick and its layout is fiddly. (§5.1.)

---

## 3. The direct framebuffer under D3D11's memory model

**Where:** `GpDirectPane` (gp_engine.mm:1117-1212, `GAMEPANE_PLAN.md` §6b) —
3 shared `MTLBuffer`s the game writes as zero-copy external typed data, each with
a linear R8_UINT texture view the GPU samples.

**The unknown:** the design is built on **Apple unified memory** —
`MTLStorageModeShared.contents()` is CPU-writable memory the GPU samples with
zero copy (gp_engine.mm:1156-1163). **Discrete Windows GPUs have no shared
memory**, and D3D11 has no "persistently CPU-written buffer viewed as a
GPU-sampled texture" (a `DYNAMIC` resource's Map pointer is only valid between
Map/Unmap; the game writes at arbitrary times, even off-thread across worker
isolates, §6b).

**Resolution:** keep the Dart-facing contract identical (external typed data over
a **stable host pointer**, length-bounded, no finalizer — gp_natives.mm:440-451
unchanged), and turn the zero-copy into **host-write-then-upload-on-present**: 3
host ring buffers exposed to Dart; at `present_render`, `UpdateSubresource` the
just-written buffer into one DEFAULT R8_UINT SRV texture, then sample. The triple
ring still decouples the off-thread writers from the present-time read. **This is
the one place the port is not byte-identical to Metal** (one `stride*h` upload per
present — 75 KB at 424×176, negligible). Flag it to the architect as an accepted
semantic delta, not a bug. Optional UMA fast path exists but is not the portable
baseline. (§5.6, §12.2.)

---

## 4. Clip-space Y — the trap is OVER-correcting (not a real flip)

**Where:** every vertex shader's `o.uv = (…, 1.0 - (pos.y+1)*0.5)` and the sprite
quad's y-down screen mapping (gp_engine.mm:620), plus the offscreen→backbuffer
copy.

**The unknown / trap:** the porting folklore "Metal and D3D need a Y flip" is a
**myth for this port** — it is true for OpenGL/Vulkan, not Metal. Metal's NDC was
designed to match Direct3D: **both are Y-up NDC (+1 = top), Z∈[0,1], top-left
texel origin.** The verbatim UV math is convention-neutral and ports as-is; the
CPU buffers upload row-0-first (top), sample top-first, and the present copy is
top-left aligned. **The real risk is a well-meaning engineer ADDING a flip** to
"correct for D3D," which vertically mirrors the entire game.

**Resolution:** add no flip. Port the vertex UV math and the sprite transform
byte-identical (`shaders.hlsl` header note). **Verify:** define a copper bar for
screen line 0 and a sprite at a known top-left position; gpsnap; confirm they
render at the TOP. If the image is upside-down, the fix is to REMOVE a flip, not
add one. Low risk if the team trusts this note; medium if they "fix" it blind.

---

## 5. Resource-usage collisions (DYNAMIC vs UAV) and the UAV↔SRV hazard

**Where:** index-pane textures are both CPU-uploaded (`replaceRegion`,
gp_engine.mm:411) **and** blit UAV targets **and** render SRVs.

**The unknown:** D3D11 forbids `USAGE_DYNAMIC` + `BIND_UNORDERED_ACCESS` on the
same texture, and forbids a resource bound simultaneously as UAV (u0) and SRV
(t0) — it silently nulls the SRV and warns. A naive port that reaches for a
DYNAMIC texture (the obvious "CPU-writable" choice) hits a create-time failure;
one that forgets to unbind the blit UAV before rendering the same slot gets a
black pane + a debug-layer warning.

**Resolution:** index textures are `USAGE_DEFAULT` +
`SHADER_RESOURCE|UNORDERED_ACCESS`, uploaded via `UpdateSubresource` (not Map).
After the last `gpblit` and before the render pass, unbind the compute UAV
(`CSSetUnorderedAccessViews(0,1,&null,NULL)`). D3D11's automatic hazard tracking
handles the write→read barrier itself. Mechanical once known; a sharp edge if
not. (§5.1, §5.3.)

---

## 6. Lesser, mechanical points (documented so they aren't rediscovered)

- **cbuffer array packing** — `float p[8]` in the shader-background cbuffer takes
  128 bytes (16 B per element), not 32; the host must re-lay-out the tight MSL
  upload (gp_engine.mm:907-911) into the 144-byte layout in §5.5. Wrong-looking
  shader-background params if missed.
- **SRV/UAV namespace split** — Metal's src=texture(0)/dst=texture(1) becomes src
  SRV t0 / dst UAV **u0** (not u1). Host binding call changes (`shaders.hlsl` §6).
- **Inline sampler** — MSL's `constexpr sampler` (gp_engine.mm:131) has no SM5.0
  equivalent; host creates a POINT/CLAMP `SamplerState` at s0 (§5.4).
- **Runtime shader errors** — `D3DCompile`'s `ID3DBlob` error text must be
  returned as a string (never abort), preserving the `gpshader` contract
  (gp_engine.mm:880-890, gp_natives.mm:309-312).
- **Crisp upscale** — CAMetalLayer's `kCAFilterNearest` (gp_engine.mm:1241) has
  no DXGI equivalent; do the nearest letterbox upscale in a present-blit shader
  (`shaders.hlsl` §7), not via DWM stretch. (§7.)
- **Audio polyphony** — `AVAudioPlayerNode` sums overlapping schedules; XAudio2
  needs an explicit source-voice pool for the same effect (GP_AUDIO_DESIGN §1.3).
- **XAudio2 buffer lifetime** — XAudio2 does not copy PCM; a `define` that
  replaces a still-playing slot risks use-after-free — retire the old buffer
  (GP_AUDIO_DESIGN §1.4).
- **No matrices** — the "column- vs row-major" concern is N/A: this engine has
  zero matrix uniforms (all transforms are CPU-precomputed). Noted so nobody adds
  a `float4x4` and trips over HLSL's column-major default.

---

## Nothing in the MSL lacks a clean HLSL equivalent

Every construct has a direct SM5.0 counterpart (the deltas above are bindings and
packing, not missing features). The genuine *design* deltas are all on the D3D11
host side, not the shaders: the min-term typed-UAV-load gate (#1), and the direct
framebuffer's loss of unified-memory zero-copy (#3). Those two are the decisions
that need architect sign-off before S6 codes.
