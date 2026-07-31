# WINDART Sprint S6 — Pong (the game milestone) + snapshot unification

Goal: the game milestone — **Pong renders and is playable**, proven headlessly by
a saved PNG — plus unify the snapshot primitive. Built on S5's Direct2D canvas.

## MILESTONE — HIT: Pong renders + playable (`e:\windart\build\game_pong.png`)
```
> dartui.exe game_runner.dart 11_pong e:\windart\build\game_pong.png 60
GAME: SNAP 11_pong frames=60 -> ...game_pong.png OK
```
The PNG shows Pong live: the "score 0  best 0  balls 3" HUD + separator line, the
blue paddle at the bottom, and the **yellow ball in flight near the top** — the
runner injects a serve (space, frames 3-6) then plays with no keys, so the ball is
mid-bounce. Proves the **pull-paced control-port loop** end to end: the game draws
one frame per UI invitation, reading `[downKeycodes, mods]` off each tick.

## FINDING 1 — Pong is a *canvas* game, not a game-pane (`gpApply`) game
`11_pong.dart` uses the S5 canvas draw protocol (`clear/line/rect/oval/text`) + a
**control port** for pull-paced input — **zero `gp*` verbs**. So the milestone
renders on the S5 Direct2D canvas with **no new C++** (only the pull-paced Dart
runner). The Direct3D 11 **game pane** is used by the *other* games:
- `12_copper` (cls + **per-scanline palette** + sprite + text),
- `13_invaders` (blit + pal + shader + sound + **sprite** + tune + text),
- `15_brickout` (pal + shader + sound + **sprite** + tune + text),
all via the `GamePane` library (`demos/gamepane.dart`). Every gp game needs
sprites + the indexed pane + text at minimum — see FINDING 3.

## FINDING 2 — a DEBUG-only VM assert blocks interactive games (worked around)
Pong's `gs[0] is List` on a **cross-isolate-copied `List<int>`** trips a DEBUG
invariant: `object.cc:15903 ASSERT(type_arguments.IsNull() || IsCanonical())` in
`Object::IsInstanceOf` — the copied list's type-args aren't re-canonicalized, and
the DEBUG build asserts. The S5 demos never hit it (one-way, never *received* a
message). **Workaround:** the runner sends untyped `List<dynamic>` ticks
(`[keys, 0]`, not `<int>[...]`) — type-args `[dynamic]` canonicalize fine. This
affects **every** interactive game in the DEBUG build (they all read `[downKeys,
mods]` ticks). Options for the real games: keep tick payloads untyped, run a
**Release (NDEBUG) dartui** (assert compiled out), or fix the cross-isolate
type-args canonicalization in the VM. Recommend a Release game-run config +
untyped tick payloads; flag the canonicalization gap for a VM look.

## DELIVERED — `Win_surfaceSnapshot` (architect §4: unify the snapshot)
One reusable PNG-readback native: `Win_surfaceSnapshot(surface, path)` finds the
surface's canvas (via `ViewServer::CanvasTicketInSurface`) and encodes it through
the same `CanvasSnapPng` (WIC). `win.dart` `Ui.snapshot(path)`. The game runner
now snapshots via `ui.snapshot()` (verified). When the D3D11 game pane lands, its
`gpsnap` routes through this same native — the single primitive the S7 rehost
regression loop calls per surface.

## What was created (owned)
- `test/game_runner.dart` — pull-paced control-port game runner (kick on `['port']`,
  tick after each `['draw']`, self-serve, snapshot, quit).
- `test/demos/11_pong.dart` — copied from the reference (unchanged).
- `dart_win32/win_view.{h,cpp}` — `CanvasTicketInSurface`; `win_natives.cpp` —
  `Win_surfaceSnapshot`; `win.dart` — `Ui.snapshot`.
- No tree edits (windart-port.patch unchanged); regressions clean (canvas demos,
  dartui button, dart.exe hello); quarry git-clean.

## FINDING 3 — the D3D11 game pane is a full sprint (assessment + plan)
The signed-off design (`gamepane-design/`, ~870 lines) is thorough. The engine
(`gp_engine_d3d.cc`) is a large, multi-component build; the **minimal** gp game
(`12_copper`) already needs: device+swapchain+offscreen RT+present+gpsnap, the
**indexed pane** (8× R8_UINT textures + the per-scanline-palette StructuredBuffer +
`ps_indexed`), **sprites** (IMMUTABLE R8_UINT frames + palette + dynamic VB +
alpha-over blend), the **text overlay**, the fixed shaders via `D3DCompile`, the
**7 `Win_gp*` natives** + the ~40-verb `gpApply` dispatch, and the `GamePane` Dart
library. Invaders/brickout add the compute blitter, the runtime `gpshader`, and
XAudio2. This is comparable in size to S4 or S5 on its own.

**Recommendation:** give the D3D11 game pane its own focused turn (as each prior
sprint had). Suggested order for that turn: (1) device/swapchain/offscreen/present
/gpsnap on the canvas-style child HWND + the `game` widget kind; (2) the indexed
pane + per-scanline palette + `ps_indexed`/`vs_present` → render **copper** (no
sprites path first — cls to index 1, then linePal) and gpsnap-verify the copper
bar for screen line 0 is at the **TOP** (the design's Y-flip guard); (3) sprites +
text → copper's orb, then invaders' self-play; (4) blitter (CPU-mirror baseline) +
XAudio2. The unified `Win_surfaceSnapshot` is already in place as the gpsnap sink.

## S7 hand-off
Backend + GUI framework + canvas + one game (Pong) all live and verified. The
`Win_surfaceSnapshot` primitive is ready for the S7 rehost regression loop. The
D3D11 game pane (copper/invaders/brickout) is the remaining graphics piece,
scoped above.
