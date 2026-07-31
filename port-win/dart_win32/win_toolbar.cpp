// WINDART polish — the icon toolbar (ToolbarWindow32, comctl32). A themed button
// band below the menu bar. Icons are drawn with Direct2D (borrowing the shapes of
// the mono SVGs in macdart/.../icons-mono, plus a few made up: Do It = a green
// play triangle) into a 32-bit premultiplied-alpha DIB per icon, assembled into an
// ILC_COLOR32 HIMAGELIST. Buttons' command ids are the SHARED WinHostCommand ids,
// so a toolbar click and the matching menu item run the same action.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <d2d1.h>

#include "win_host.h"     // WinHostCommand ids
#include "win_toolbar.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "comctl32.lib")

namespace dart {
namespace bin {

static const int kIconSize = 20;

enum IconId {
  IC_NEW, IC_OPEN, IC_SAVE, IC_DOIT, IC_HOME, IC_BACK, IC_FWD,
  IC_REFRESH, IC_FIND, IC_BROWSE, IC_COUNT
};

// Draw one glyph into the [0..20] icon box with the given ink (mono) + accent.
static void DrawGlyph(ID2D1RenderTarget* rt, ID2D1Factory* fac, int icon,
                      ID2D1SolidColorBrush* ink, ID2D1SolidColorBrush* accent) {
  auto ln = [&](float x0, float y0, float x1, float y1, float w) {
    rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), ink, w);
  };
  switch (icon) {
    case IC_NEW: {                              // a sheet with a folded corner
      rt->DrawRectangle(D2D1::RectF(4, 2, 15, 18), ink, 1.6f);
      ln(11, 2, 11, 6, 1.4f); ln(11, 6, 15, 6, 1.4f); ln(11, 2, 15, 6, 1.4f);
      break;
    }
    case IC_OPEN: {                             // an open folder
      rt->DrawRectangle(D2D1::RectF(3, 8, 17, 16), ink, 1.6f);
      ln(3, 8, 8, 8, 1.4f); ln(8, 8, 10, 6, 1.4f); ln(10, 6, 14, 6, 1.4f); ln(14, 6, 14, 8, 1.4f);
      break;
    }
    case IC_SAVE: {                             // a floppy disk
      rt->DrawRectangle(D2D1::RectF(3, 3, 17, 17), ink, 1.6f);
      rt->DrawRectangle(D2D1::RectF(6, 3, 14, 8), ink, 1.2f);   // shutter
      rt->DrawRectangle(D2D1::RectF(6, 11, 14, 17), ink, 1.0f); // label
      break;
    }
    case IC_DOIT: {                             // a green play triangle (filled)
      ID2D1PathGeometry* g = nullptr;
      if (SUCCEEDED(fac->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(g->Open(&s))) {
          s->BeginFigure(D2D1::Point2F(6, 4), D2D1_FIGURE_BEGIN_FILLED);
          s->AddLine(D2D1::Point2F(16, 10));
          s->AddLine(D2D1::Point2F(6, 16));
          s->EndFigure(D2D1_FIGURE_END_CLOSED);
          s->Close(); s->Release();
          rt->FillGeometry(g, accent);
        }
        g->Release();
      }
      break;
    }
    case IC_HOME: {                             // a house
      ln(3, 10, 10, 4, 1.6f); ln(10, 4, 17, 10, 1.6f);
      rt->DrawRectangle(D2D1::RectF(5, 9, 15, 17), ink, 1.5f);
      rt->DrawRectangle(D2D1::RectF(8, 12, 12, 17), ink, 1.1f);
      break;
    }
    case IC_BACK:    { ln(13, 4, 6, 10, 2.0f); ln(6, 10, 13, 16, 2.0f); break; }
    case IC_FWD:     { ln(7, 4, 14, 10, 2.0f); ln(14, 10, 7, 16, 2.0f); break; }
    case IC_REFRESH: {                          // a circular arrow
      ID2D1PathGeometry* g = nullptr;
      if (SUCCEEDED(fac->CreatePathGeometry(&g))) {
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(g->Open(&s))) {
          s->BeginFigure(D2D1::Point2F(15, 6), D2D1_FIGURE_BEGIN_HOLLOW);
          D2D1_ARC_SEGMENT arc = { D2D1::Point2F(14, 15), D2D1::SizeF(6, 6), 0.0f,
                                   D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE };
          s->AddArc(arc);
          s->EndFigure(D2D1_FIGURE_END_OPEN);
          s->Close(); s->Release();
          rt->DrawGeometry(g, ink, 1.7f);
        }
        g->Release();
      }
      ln(15, 6, 11, 6, 1.6f); ln(15, 6, 15, 10, 1.6f);   // arrowhead
      break;
    }
    case IC_FIND: {                             // a magnifier
      rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(9, 9), 5, 5), ink, 1.6f);
      ln(13, 13, 17, 17, 1.9f);
      break;
    }
    case IC_BROWSE: {                           // a class hierarchy
      rt->DrawRectangle(D2D1::RectF(7, 3, 13, 7), ink, 1.4f);
      rt->DrawRectangle(D2D1::RectF(2, 13, 8, 17), ink, 1.4f);
      rt->DrawRectangle(D2D1::RectF(12, 13, 18, 17), ink, 1.4f);
      ln(10, 7, 10, 10, 1.2f); ln(5, 10, 15, 10, 1.2f);
      ln(5, 10, 5, 13, 1.2f); ln(15, 10, 15, 13, 1.2f);
      break;
    }
  }
}

static HIMAGELIST BuildIconImageList() {
  ID2D1Factory* fac = nullptr;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               __uuidof(ID2D1Factory),
                               reinterpret_cast<void**>(&fac)))) {
    return nullptr;
  }
  HIMAGELIST himl = ImageList_Create(kIconSize, kIconSize, ILC_COLOR32, IC_COUNT, 0);
  if (!himl) { fac->Release(); return nullptr; }

  for (int i = 0; i < IC_COUNT; i++) {
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kIconSize;
    bi.bmiHeader.biHeight = -kIconSize;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) continue;
    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, dib);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1DCRenderTarget* rt = nullptr;
    if (SUCCEEDED(fac->CreateDCRenderTarget(&props, &rt))) {
      RECT rc = {0, 0, kIconSize, kIconSize};
      rt->BindDC(mem, &rc);
      ID2D1SolidColorBrush* ink = nullptr;
      ID2D1SolidColorBrush* accent = nullptr;
      rt->CreateSolidColorBrush(D2D1::ColorF(0.16f, 0.17f, 0.20f), &ink);      // slate
      rt->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.60f, 0.24f), &accent);   // green
      rt->BeginDraw();
      rt->Clear(D2D1::ColorF(0, 0, 0, 0));   // transparent
      if (ink && accent) DrawGlyph(rt, fac, i, ink, accent);
      rt->EndDraw();
      if (ink) ink->Release();
      if (accent) accent->Release();
      rt->Release();
    }

    SelectObject(mem, old);
    DeleteDC(mem);
    ImageList_Add(himl, dib, nullptr);   // ILC_COLOR32 -> uses the alpha channel
    DeleteObject(dib);
  }
  fac->Release();
  return himl;
}

HWND WinToolbarCreate(HWND parent) {
  HWND tb = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
      WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP | CCS_NODIVIDER,
      0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!tb) return nullptr;
  SendMessageW(tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  HIMAGELIST himl = BuildIconImageList();
  if (himl) SendMessageW(tb, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(himl));
  SendMessageW(tb, TB_SETBUTTONSIZE, 0, MAKELONG(30, 30));

  auto B = [](int bmp, int cmd) {
    TBBUTTON b = {0};
    b.iBitmap = bmp; b.idCommand = cmd; b.fsState = TBSTATE_ENABLED;
    b.fsStyle = BTNS_BUTTON; b.iString = -1;
    return b;
  };
  auto SEP = []() {
    TBBUTTON b = {0}; b.fsState = TBSTATE_ENABLED; b.fsStyle = BTNS_SEP; b.iString = -1;
    return b;
  };
  TBBUTTON btns[] = {
    B(IC_NEW, CMD_NEW), B(IC_OPEN, CMD_OPEN), B(IC_SAVE, CMD_SAVE),
    SEP(),
    B(IC_DOIT, CMD_DOIT),
    SEP(),
    B(IC_HOME, CMD_HOME), B(IC_BACK, CMD_BACK), B(IC_FWD, CMD_FORWARD), B(IC_REFRESH, CMD_REFRESH),
    SEP(),
    B(IC_FIND, CMD_FIND), B(IC_BROWSE, CMD_BROWSE),
  };
  SendMessageW(tb, TB_ADDBUTTONS, sizeof(btns) / sizeof(btns[0]),
               reinterpret_cast<LPARAM>(btns));
  SendMessageW(tb, TB_AUTOSIZE, 0, 0);
  ShowWindow(tb, SW_SHOW);
  return tb;
}

int WinToolbarHeight(HWND tb) {
  if (!tb) return 0;
  RECT r; GetWindowRect(tb, &r);
  int h = r.bottom - r.top;
  return h > 0 ? h : 34;
}

}  // namespace bin
}  // namespace dart
