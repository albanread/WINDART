# S6 game-pane D3D11 design — architect sign-off

Reviewed `e:\windart\gamepane-design\` (shaders.hlsl 401 lines, GP_ENGINE_D3D_DESIGN.md
468, GP_AUDIO_DESIGN.md, RISKS.md). **Verdict: APPROVED.** Deep, correct D3D11
work — the agent caught the non-obvious gotchas (typed-UAV-load format gating,
cbuffer 16-byte packing vs MSL's tight 40, the UAV↔SRV unbind hazard) and, best,
debunked the clip-space-Y flip myth. Execute S6 AFTER S5 (canvas), as a
compile-iterate sprint. gp_synth.cc reused verbatim.

## Sign-off on the 3 hardest unknowns (all resolutions ACCEPTED)
1. **Compute-blitter min-term typed-UAV-load of R8_UINT** (D3D11 gates it behind
   `TypedUAVLoadAdditionalFormats`, not universal). → **CPU-mirror baseline** (the
   AND/OR/XOR already runs at gp_engine.mm:743-760 for pget parity — VERIFIED it
   exists, so the fallback is free), GPU min-term enabled only when the device
   cap probe passes. copy/transparent/clear are store-only, safe on GPU
   unconditionally. Correct progressive-enhancement.
2. **Direct-framebuffer loss of Apple unified-memory zero-copy.** → Keep the
   identical Dart external-typed-data contract over a stable host buffer + one
   `stride*h` CPU→GPU upload per present (~75 KB @424×176 ≈ 4.5 MB/s at 60fps —
   negligible). The ONE accepted non-byte-identical delta vs Metal; the Dart-facing
   contract (the game writes a Uint8List that IS the framebuffer) is preserved.
3. **Clip-space Y is a trap, not a flip.** → Metal and D3D11 AGREE (Y-up NDC,
   top-left texel, Z∈[0,1]); the flip myth is OpenGL/Vulkan. Port the UV math
   VERBATIM; the real hazard is a dev ADDING a correction and mirroring the game.
   Guard: gpsnap-verify (copper bar for screen line 0 must render at top); "if
   upside-down, REMOVE a flip, don't add one." Excellent catch.

Mechanical-but-sharp (documented for the S6 coder): USAGE_DEFAULT+UpdateSubresource
(a texture can't be both DYNAMIC and a UAV), SRV/UAV namespace split (dst=u0), the
144-byte cbuffer upload. Audio: AVAudioEngine→XAudio2 (source-voice pool for
polyphony), AVMIDIPlayer→winmm midiStream + GS synth (BuildSmf reusable).

Both GUI-sprint designs (S4 host, S6 game pane) now approved with decisions.
S5 (Direct2D canvas) has no separate design yet — it shares the D2D setup with
S4's host, so design it after S4's Direct2D foundation exists.
