// WINDART S5 — Direct2D canvas implementation. Renders the demo draw-command
// protocol to an offscreen WIC bitmap via an ID2D1RenderTarget, and encodes it
// to PNG for headless verification. All calls run on the UI (pump) thread, where
// the host already did CoInitializeEx(APARTMENTTHREADED).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>     // CoCreateInstance / IID_PPV_ARGS (WIN32_LEAN_AND_MEAN omits)
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "include/dart_api.h"
#include "win_canvas.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace dart {
namespace bin {

// ── shared factories (lazy, UI-thread) ──────────────────────────────────────
static ID2D1Factory* g_d2d = nullptr;
static IDWriteFactory* g_dwrite = nullptr;
static IWICImagingFactory* g_wic = nullptr;

static bool EnsureFactories() {
  if (g_d2d == nullptr) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory),
                                 reinterpret_cast<void**>(&g_d2d)))) {
      return false;
    }
  }
  if (g_dwrite == nullptr) {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g_dwrite)))) {
      return false;
    }
  }
  if (g_wic == nullptr) {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) {
      return false;
    }
  }
  return true;
}

// ── one canvas ──────────────────────────────────────────────────────────────
struct Canvas {
  int w = 0, h = 0;
  HWND hwnd = nullptr;                 // the live child window (unused for the
                                       // headless PNG path; kept for S5+ present)
  IWICBitmap* wic = nullptr;           // the offscreen pixels
  ID2D1RenderTarget* rt = nullptr;     // D2D target over `wic`
  ID2D1SolidColorBrush* brush = nullptr;
};

static std::unordered_map<int64_t, Canvas*> g_canvases;

static Canvas* Find(int64_t ticket) {
  auto it = g_canvases.find(ticket);
  return it == g_canvases.end() ? nullptr : it->second;
}

// ── small decode helpers ────────────────────────────────────────────────────
static double DNum(Dart_Handle h) {
  if (h == nullptr) return 0.0;
  if (Dart_IsDouble(h)) { double d = 0; Dart_DoubleValue(h, &d); return d; }
  if (Dart_IsInteger(h)) { int64_t v = 0; Dart_IntegerToInt64(h, &v); return (double)v; }
  return 0.0;
}
static double LNum(Dart_Handle list, intptr_t i) { return DNum(Dart_ListGetAt(list, i)); }
static bool LBool(Dart_Handle list, intptr_t i) {
  Dart_Handle h = Dart_ListGetAt(list, i);
  bool b = false;
  if (h != nullptr && Dart_IsBoolean(h)) Dart_BooleanValue(h, &b);
  return b;
}
static std::string LStr(Dart_Handle list, intptr_t i) {
  Dart_Handle h = Dart_ListGetAt(list, i);
  if (h == nullptr || !Dart_IsString(h)) return "";
  const char* c = nullptr;
  if (Dart_IsError(Dart_StringToCString(h, &c)) || c == nullptr) return "";
  return std::string(c);
}
static std::wstring Widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

// Standard base64 decode (the pixmap.dart BMP crosses as base64).
static std::vector<unsigned char> Base64Decode(const std::string& in) {
  static signed char T[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; i++) T[i] = -1;
    const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) T[(unsigned char)a[i]] = (signed char)i;
    init = true;
  }
  std::vector<unsigned char> out;
  int val = 0, bits = -8;
  for (unsigned char c : in) {
    if (c == '=' || T[c] == -1) { if (c == '=') break; else continue; }
    val = (val << 6) + T[c];
    bits += 6;
    if (bits >= 0) { out.push_back((unsigned char)((val >> bits) & 0xFF)); bits -= 8; }
  }
  return out;
}

// ── create / destroy ────────────────────────────────────────────────────────
bool CanvasCreate(int64_t ticket, int w, int h, HWND hwnd) {
  if (Find(ticket) != nullptr) return true;
  if (!EnsureFactories()) return false;
  if (w <= 0) w = 1;
  if (h <= 0) h = 1;

  Canvas* c = new Canvas();
  c->w = w; c->h = h; c->hwnd = hwnd;

  if (FAILED(g_wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnLoad, &c->wic))) {
    delete c; return false;
  }
  D2D1_RENDER_TARGET_PROPERTIES props = {
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      { DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN },
      0.0f, 0.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT };
  if (FAILED(g_d2d->CreateWicBitmapRenderTarget(c->wic, &props, &c->rt))) {
    c->wic->Release(); delete c; return false;
  }
  D2D1_COLOR_F white = { 1.0f, 1.0f, 1.0f, 1.0f };
  c->rt->CreateSolidColorBrush(white, &c->brush);
  g_canvases[ticket] = c;
  return true;
}

void CanvasDestroy(int64_t ticket) {
  Canvas* c = Find(ticket);
  if (!c) return;
  if (c->brush) c->brush->Release();
  if (c->rt) c->rt->Release();
  if (c->wic) c->wic->Release();
  g_canvases.erase(ticket);
  delete c;
}

// ── the replay ──────────────────────────────────────────────────────────────
static void SetColor(Canvas* c, double r, double g, double b) {
  D2D1_COLOR_F col = { (float)r, (float)g, (float)b, 1.0f };
  c->brush->SetColor(col);
}

static void DrawBlit(Canvas* c, Dart_Handle cmd) {
  double x = LNum(cmd, 1), y = LNum(cmd, 2), dw = LNum(cmd, 3), dh = LNum(cmd, 4);
  std::vector<unsigned char> bmp = Base64Decode(LStr(cmd, 5));
  if (bmp.empty()) return;

  IWICStream* stream = nullptr;
  if (FAILED(g_wic->CreateStream(&stream))) return;
  if (FAILED(stream->InitializeFromMemory(bmp.data(), (DWORD)bmp.size()))) {
    stream->Release(); return;
  }
  IWICBitmapDecoder* dec = nullptr;
  if (FAILED(g_wic->CreateDecoderFromStream(stream, nullptr,
                                            WICDecodeMetadataCacheOnLoad, &dec))) {
    stream->Release(); return;
  }
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  ID2D1Bitmap* d2dbmp = nullptr;
  if (SUCCEEDED(dec->GetFrame(0, &frame)) &&
      SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
      SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom)) &&
      SUCCEEDED(c->rt->CreateBitmapFromWicBitmap(conv, nullptr, &d2dbmp))) {
    D2D1_RECT_F dst = { (float)x, (float)y, (float)(x + dw), (float)(y + dh) };
    c->rt->DrawBitmap(d2dbmp, dst, 1.0f,
                      D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
  }
  if (d2dbmp) d2dbmp->Release();
  if (conv) conv->Release();
  if (frame) frame->Release();
  dec->Release();
  stream->Release();
}

void CanvasDraw(int64_t ticket, Dart_Handle cmds) {
  Canvas* c = Find(ticket);
  if (!c || c->rt == nullptr) return;
  intptr_t n = 0;
  if (cmds == nullptr || Dart_IsError(Dart_ListLength(cmds, &n))) return;

  c->rt->BeginDraw();
  for (intptr_t i = 0; i < n; i++) {
    Dart_Handle cmd = Dart_ListGetAt(cmds, i);
    if (cmd == nullptr || !Dart_IsList(cmd)) continue;
    intptr_t clen = 0;
    if (Dart_IsError(Dart_ListLength(cmd, &clen)) || clen == 0) continue;
    std::string op = LStr(cmd, 0);

    if (op == "clear") {
      D2D1_COLOR_F col = { (float)LNum(cmd, 1), (float)LNum(cmd, 2),
                           (float)LNum(cmd, 3), 1.0f };
      c->rt->Clear(col);
    } else if (op == "rect" || op == "oval") {
      double x = LNum(cmd, 1), y = LNum(cmd, 2), w = LNum(cmd, 3), h = LNum(cmd, 4);
      SetColor(c, LNum(cmd, 5), LNum(cmd, 6), LNum(cmd, 7));
      bool fill = clen > 8 && LBool(cmd, 8);
      if (op == "rect") {
        D2D1_RECT_F r = { (float)x, (float)y, (float)(x + w), (float)(y + h) };
        if (fill) c->rt->FillRectangle(r, c->brush);
        else c->rt->DrawRectangle(r, c->brush, 1.0f, nullptr);
      } else {
        D2D1_ELLIPSE e = { { (float)(x + w / 2), (float)(y + h / 2) },
                           (float)(w / 2), (float)(h / 2) };
        if (fill) c->rt->FillEllipse(e, c->brush);
        else c->rt->DrawEllipse(e, c->brush, 1.0f, nullptr);
      }
    } else if (op == "line") {
      SetColor(c, LNum(cmd, 5), LNum(cmd, 6), LNum(cmd, 7));
      float width = clen > 8 ? (float)LNum(cmd, 8) : 1.0f;
      D2D1_POINT_2F p0 = { (float)LNum(cmd, 1), (float)LNum(cmd, 2) };
      D2D1_POINT_2F p1 = { (float)LNum(cmd, 3), (float)LNum(cmd, 4) };
      c->rt->DrawLine(p0, p1, c->brush, width, nullptr);
    } else if (op == "text") {
      double x = LNum(cmd, 1), y = LNum(cmd, 2);
      std::wstring s = Widen(LStr(cmd, 3));
      float sz = (float)LNum(cmd, 4);
      if (sz <= 0) sz = 11.0f;
      SetColor(c, LNum(cmd, 5), LNum(cmd, 6), LNum(cmd, 7));
      IDWriteTextFormat* fmt = nullptr;
      if (SUCCEEDED(g_dwrite->CreateTextFormat(
              L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sz, L"en-us",
              &fmt))) {
        D2D1_RECT_F layout = { (float)x, (float)y, (float)(x + 2000),
                               (float)(y + sz * 2.0) };
        c->rt->DrawText(s.c_str(), (UINT32)s.length(), fmt, layout, c->brush,
                        D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
        fmt->Release();
      }
    } else if (op == "blit") {
      DrawBlit(c, cmd);
    }
  }
  c->rt->EndDraw();
}

// ── PNG snapshot ────────────────────────────────────────────────────────────
// Shared WIC PNG encode of any bitmap source (the offscreen canvas, or a window DIB).
static const char* EncodeToPng(IWICBitmapSource* src, UINT w, UINT h,
                               WICPixelFormatGUID srcFmt, const wchar_t* path) {
  if (!EnsureFactories()) return "WIC factory unavailable";
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* bag = nullptr;
  const char* err = nullptr;
  if (FAILED(g_wic->CreateStream(&stream)) ||
      FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) {
    err = "cannot open PNG file for write";
  } else if (FAILED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
             FAILED(enc->Initialize(stream, WICBitmapEncoderNoCache)) ||
             FAILED(enc->CreateNewFrame(&frame, &bag)) ||
             FAILED(frame->Initialize(bag))) {
    err = "PNG encoder init failed";
  } else {
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = srcFmt;   // match the source -> no conversion
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WriteSource(src, nullptr)) ||
        FAILED(frame->Commit()) || FAILED(enc->Commit())) {
      err = "PNG encode failed";
    }
  }
  if (bag) bag->Release();
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  return err == nullptr ? "" : err;
}

const char* CanvasSnapPng(int64_t ticket, const wchar_t* path) {
  Canvas* c = Find(ticket);
  if (!c || c->wic == nullptr) return "canvas not found";
  return EncodeToPng(c->wic, (UINT)c->w, (UINT)c->h,
                     GUID_WICPixelFormat32bppPBGRA, path);
}

// Snapshot a live window (incl. its child controls) to PNG via PrintWindow — the
// headless drive-and-see regression capture for the workspace shell (S7 §6.6;
// the mac bitmapImageRepForCachingDisplayInRect analogue).
const char* SnapshotHwndToPng(HWND hwnd, const wchar_t* path) {
  if (!EnsureFactories()) return "WIC factory unavailable";
  RECT rc;
  GetClientRect(hwnd, &rc);
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return "window has no client area";
  // Force children (e.g. virtual list rows) to paint before capture.
  RedrawWindow(hwnd, nullptr, nullptr,
               RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  if (!PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT)) {
    PrintWindow(hwnd, mem, PW_CLIENTONLY);   // fallback (older content path)
  }

  BITMAPINFO bi = {0};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;   // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  std::vector<unsigned char> px((size_t)w * h * 4);
  GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
  for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;   // PrintWindow gives BGRX -> opaque

  const char* err = nullptr;
  IWICBitmap* wicbmp = nullptr;
  if (SUCCEEDED(g_wic->CreateBitmapFromMemory((UINT)w, (UINT)h,
          GUID_WICPixelFormat32bppBGRA, (UINT)(w * 4), (UINT)px.size(),
          px.data(), &wicbmp))) {
    err = EncodeToPng(wicbmp, (UINT)w, (UINT)h,
                      GUID_WICPixelFormat32bppBGRA, path);
    wicbmp->Release();
  } else {
    err = "WIC bitmap from DIB failed";
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return err;
}

// Full-window capture (non-client frame + menu bar included). Same DIB->WIC->PNG
// path as SnapshotHwndToPng, but sized to the WINDOW rect and WITHOUT PW_CLIENTONLY
// so PrintWindow renders the menu bar and frame too.
const char* SnapshotWindowFullToPng(HWND hwnd, const wchar_t* path) {
  if (!EnsureFactories()) return "WIC factory unavailable";
  RECT wr;
  GetWindowRect(hwnd, &wr);
  int w = wr.right - wr.left, h = wr.bottom - wr.top;
  if (w <= 0 || h <= 0) return "window has no size";
  RedrawWindow(hwnd, nullptr, nullptr,
               RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME);

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  if (!PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)) {   // whole window incl. menu
    PrintWindow(hwnd, mem, 0);
  }

  BITMAPINFO bi = {0};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;   // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  std::vector<unsigned char> px((size_t)w * h * 4);
  GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
  for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;

  const char* err = nullptr;
  IWICBitmap* wicbmp = nullptr;
  if (SUCCEEDED(g_wic->CreateBitmapFromMemory((UINT)w, (UINT)h,
          GUID_WICPixelFormat32bppBGRA, (UINT)(w * 4), (UINT)px.size(),
          px.data(), &wicbmp))) {
    err = EncodeToPng(wicbmp, (UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGRA, path);
    wicbmp->Release();
  } else {
    err = "WIC bitmap from DIB failed";
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return err;
}

}  // namespace bin
}  // namespace dart
