// WINDART S5 — the Direct2D canvas: an offscreen WIC-bitmap render target that
// replays the demo draw-command protocol and can snapshot itself to a PNG (the
// gpsnap "honest pixels" pattern). This is the D2D foundation; S6's game pane is
// D3D11 and does NOT share it.
//
// The draw protocol (workspace.dart:3069-3073), TOP-LEFT origin (no Cocoa flip):
//   ['clear', r,g,b]
//   ['rect', x,y,w,h, r,g,b, fill]        fill = true/false
//   ['oval', x,y,w,h, r,g,b, fill]        (bounding box, top-left)
//   ['line', x1,y1,x2,y2, r,g,b, width]   width optional (default 1)
//   ['text', x,y, string, size, r,g,b]
//   ['blit', x,y, dw,dh, base64-bmp]      a demos/pixmap.dart 24-bit BMP
#ifndef WINDART_WIN_CANVAS_H_
#define WINDART_WIN_CANVAS_H_

#include <windows.h>
#include <cstdint>
#include "include/dart_api.h"

namespace dart {
namespace bin {

// Create an offscreen w x h canvas keyed by widget ticket (idempotent per ticket).
bool CanvasCreate(int64_t ticket, int w, int h, HWND hwnd);
// Replay a Dart List of draw commands atomically (BeginDraw..EndDraw).
void CanvasDraw(int64_t ticket, Dart_Handle cmds);
// Encode the canvas' current pixels to a PNG file. Returns "" or an error.
const char* CanvasSnapPng(int64_t ticket, const wchar_t* path);
void CanvasDestroy(int64_t ticket);

// Snapshot a live window (+ its child controls) to PNG via PrintWindow — the
// workspace-shell headless capture (S7). Returns "" or an error.
const char* SnapshotHwndToPng(HWND hwnd, const wchar_t* path);

// Snapshot the WHOLE window incl. the non-client frame + MENU BAR (not just the
// client area) — for the polish overview showing the menu + toolbar. "" or error.
const char* SnapshotWindowFullToPng(HWND hwnd, const wchar_t* path);

}  // namespace bin
}  // namespace dart

#endif  // WINDART_WIN_CANVAS_H_
