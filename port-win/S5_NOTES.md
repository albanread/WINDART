# WINDART Sprint S5 — Direct2D canvas + demos (first visual output)

Goal: a `canvas` widget backed by Direct2D, the demo draw-command protocol
replayed to it, and 2-3 demos rendering — proven headlessly by WIC PNG readback
(one pixel-path, one vector-path). Built on S4's live GUI framework; additive as
predicted (a widget kind + a native).

## MILESTONE — HIT (proven by saved PNGs in e:\windart\build\)
Three demos rendered, snapshotted to PNG, all non-blank + correct:
| demo | path | commands exercised | PNG |
|---|---|---|---|
| `03_lissajous` | vector | clear, **line**, text | `demo_lissajous.png` (46 KB) — hue-cycled 3:4 curve + label |
| `01_bounce` | vector | clear, **oval** (filled), text | `demo_bounce.png` (12 KB) — 14 balls settled under gravity |
| `07_plasma` | pixel | clear, **blit** (base64 BMP) | `demo_plasma.png` (163 KB) — full smooth plasma field |

Runner output (headless): `RUNNER: SNAP demo=03_lissajous frames=40 -> ...png OK`
(likewise bounce/plasma). Coordinates are **top-left origin, no flip** — the
Cocoa Y-flip (`workspace.dart:3204` `kDemoH - y`) simply goes away on Direct2D,
and the images confirm correct orientation (bounce balls rest at the *bottom*).

## What was created (owned)
- **`dart_win32/win_canvas.{h,cc}`** (NEW) — the Direct2D canvas:
  - Shared lazy factories: `ID2D1Factory` (single-threaded), `IDWriteFactory`,
    `IWICImagingFactory` — created on the UI thread where the host did
    `CoInitializeEx(APARTMENTTHREADED)`.
  - Per-canvas: an offscreen `IWICBitmap` (32bppPBGRA) + an
    `ID2D1RenderTarget` (WicBitmapRenderTarget) + a solid brush.
  - `CanvasDraw(ticket, cmds)` — replays the protocol atomically
    (`BeginDraw`..`EndDraw`): `clear`→`Clear`, `rect`/`oval`→`Fill/DrawRectangle`
    /`Fill/DrawEllipse`, `line`→`DrawLine`, `text`→DirectWrite `DrawText`
    (Consolas), `blit`→ a tiny base64 decoder → WIC BMP decode → `CreateBitmap
    FromWicBitmap` → `DrawBitmap`.
  - `CanvasSnapPng(ticket, path)` — WIC PNG encoder (the gpsnap "honest pixels"
    offscreen readback).
- **`test/demo_runner.dart`** — the standalone driver: `Ui.pane` + a `canvas`,
  `Isolate.spawnUri('demos/<name>.dart', [w,h], port)`, replay `['draw',cmds]`
  for N frames, `canvasSnap` to PNG, `hostQuit`. **spawnUri works under the
  DART_UI_HOST pump** (the load-port messages wake the UI isolate via the same
  NotifyUi→OnWake drain — S3 isolates confirmed).
- **`test/demos/`** — copies of `01_bounce`, `03_lissajous`, `04_mandelbrot`,
  `07_plasma`, `pixmap.dart` (from the MACDART reference; unchanged).
- Wiring: `win_view.cpp` — a `canvas` widget kind (child host + `CanvasCreate`,
  destroyed on clear). `win_natives.cpp` — `Win_canvasDraw`/`Win_canvasSnap`/
  `Win_hostQuit` natives. `win.dart` — `Ui.canvas`/`canvasDraw`/`canvasSnap` +
  `hostQuit`. CMake — `win_canvas.cpp` in `dart_win32` (libs already linked:
  `d2d1 dwrite windowscodecs ole32`).

## Draw-command protocol (workspace.dart:3069-3073, ported top-left)
```
['clear', r,g,b]                        0..1 doubles
['rect',  x,y,w,h, r,g,b, fill]         fill bool (default false)
['oval',  x,y,w,h, r,g,b, fill]         bounding box, top-left
['line',  x1,y1,x2,y2, r,g,b, width]    width optional (default 1)
['text',  x,y, string, size, r,g,b]
['blit',  x,y, dw,dh, base64-bmp]       a demos/pixmap.dart 24-bit BMP
```
One list per frame, applied atomically — the same whole-frame discipline as the
game pane. An unknown verb is ignored (never an illegal call).

## Compile burn-down
Clean on the first canvas build (0 errors). The d2d1.h inline C++ overloads
(value/reference args to `Clear`/`FillEllipse`/`DrawLine`/`DrawText`/…) compiled
as-is under `/std:c++14 /permissive`. Added `<objbase.h>` to win_canvas.cpp
defensively (IID_PPV_ARGS under WIN32_LEAN_AND_MEAN — matched win_host.cpp).

## Deferred / notes
- **Live on-screen present** — the milestone renders **offscreen** (WIC) and
  snapshots; drawing the WIC bitmap into a live `ID2D1HwndRenderTarget` for the
  visible canvas window is a small follow-up (not needed for the PNG proof).
- **Zero-copy `Win_canvasBlit`(ExternalTypedData)** — the demos use pixmap.dart's
  base64-BMP `blit` *command* (crosses as one message), which is what I
  implemented; the zero-copy typed-data native stays a future optimization.
- **Pull-pacer** — the runner consumes N frames then snapshots; strict
  one-frame-at-a-time invite pacing is the workspace's job (S7). Demos here are
  Timer-driven push (lissajous/bounce/plasma); starfield's control-port pull
  model was avoided to keep the driver minimal.
- No tree edits this sprint (windart-port.patch unchanged); dart.exe + dartui
  button regressions stay clean.

## S6/S7 hand-off
- Canvas foundation is live. S6 (game pane) is **D3D11**, separate from this D2D
  path (Win_gp* still stubbed; design signed off in gamepane-design/).
- S7 re-hosts workspace.dart onto this protocol; the demo canvas + the app pane
  now share the one materializer + the one draw protocol.
