// WINDART — custom graphic toolbar (the "image bar"). A fully owner-drawn band
// (Direct2D) replacing the Win32 ToolbarWindow32, modeled on WINVM/MACVM/MACDART:
// a textured/shaded background, large icons on the left, and a live VM-metrics GRAPH
// on the right — a sparkline over a rotating buffer of recent samples, with the
// current value as a label. Icon clicks post the SHARED WinHostCommand ids (a
// toolbar click and the matching menu item run the same action). Samples are pushed
// from Dart (wsVmStats -> WinToolbarPushMetric), so the host never re-enters the VM.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <d2d1.h>
#include <string>

#include "win_host.h"     // WinHostCommand ids
#include "win_toolbar.h"

#pragma comment(lib, "d2d1.lib")

namespace dart {
namespace bin {

static const wchar_t* kTbClass = L"WindartToolbar";
static const int kBandH    = 52;   // band height
static const int kIconBox  = 20;   // DrawGlyph's native coordinate box
static const int kIconDraw = 30;   // drawn icon size (glyph scaled up)
static const int kCellW    = 42;   // icon cell width
static const int kSepW     = 14;   // separator width
static const int kMetricsW = 360;  // right-side metrics/graph area
static const int kRing     = 96;   // rotating buffer of recent samples

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
    case IC_NEW:
      rt->DrawRectangle(D2D1::RectF(4, 2, 15, 18), ink, 1.6f);
      ln(11, 2, 11, 6, 1.4f); ln(11, 6, 15, 6, 1.4f); ln(11, 2, 15, 6, 1.4f);
      break;
    case IC_OPEN:
      rt->DrawRectangle(D2D1::RectF(3, 8, 17, 16), ink, 1.6f);
      ln(3, 8, 8, 8, 1.4f); ln(8, 8, 10, 6, 1.4f); ln(10, 6, 14, 6, 1.4f); ln(14, 6, 14, 8, 1.4f);
      break;
    case IC_SAVE:
      rt->DrawRectangle(D2D1::RectF(3, 3, 17, 17), ink, 1.6f);
      rt->DrawRectangle(D2D1::RectF(6, 3, 14, 8), ink, 1.2f);
      rt->DrawRectangle(D2D1::RectF(6, 11, 14, 17), ink, 1.0f);
      break;
    case IC_DOIT: {
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
    case IC_HOME:
      ln(3, 10, 10, 4, 1.6f); ln(10, 4, 17, 10, 1.6f);
      rt->DrawRectangle(D2D1::RectF(5, 9, 15, 17), ink, 1.5f);
      rt->DrawRectangle(D2D1::RectF(8, 12, 12, 17), ink, 1.1f);
      break;
    case IC_BACK:    { ln(13, 4, 6, 10, 2.0f); ln(6, 10, 13, 16, 2.0f); break; }
    case IC_FWD:     { ln(7, 4, 14, 10, 2.0f); ln(14, 10, 7, 16, 2.0f); break; }
    case IC_REFRESH: {
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
      ln(15, 6, 11, 6, 1.6f); ln(15, 6, 15, 10, 1.6f);
      break;
    }
    case IC_FIND:
      rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(9, 9), 5, 5), ink, 1.6f);
      ln(13, 13, 17, 17, 1.9f);
      break;
    case IC_BROWSE:
      rt->DrawRectangle(D2D1::RectF(7, 3, 13, 7), ink, 1.4f);
      rt->DrawRectangle(D2D1::RectF(2, 13, 8, 17), ink, 1.4f);
      rt->DrawRectangle(D2D1::RectF(12, 13, 18, 17), ink, 1.4f);
      ln(10, 7, 10, 10, 1.2f); ln(5, 10, 15, 10, 1.2f);
      ln(5, 10, 5, 13, 1.2f); ln(15, 10, 15, 13, 1.2f);
      break;
  }
}

// ── the band ─────────────────────────────────────────────────────────────────
struct TbItem { int icon; int cmd; int x; int w; };  // icon < 0 => separator

static struct {
  HWND hwnd = nullptr;
  ID2D1Factory* fac = nullptr;
  int hover = -1;
  float ring[kRing];             // recent samples (the sparkline)
  int ringCount = 0;             // valid samples (<= kRing)
  int ringHead = 0;              // next write slot
  std::wstring label;            // metric name + current value, under the graph
  TbItem items[16];
  int nItems = 0;
} g_tb;

// sample i in draw order: 0 = oldest ... ringCount-1 = newest.
static float RingAt(int i) {
  int idx = (g_tb.ringCount < kRing) ? i : (g_tb.ringHead + i) % kRing;
  return g_tb.ring[idx];
}

static void LayoutItems() {
  struct { int icon; int cmd; } defs[] = {
    {IC_NEW, CMD_NEW}, {IC_OPEN, CMD_OPEN}, {IC_SAVE, CMD_SAVE}, {-1, 0},
    {IC_DOIT, CMD_DOIT}, {-1, 0},
    {IC_HOME, CMD_HOME}, {IC_BACK, CMD_BACK}, {IC_FWD, CMD_FORWARD},
    {IC_REFRESH, CMD_REFRESH}, {-1, 0},
    {IC_FIND, CMD_FIND}, {IC_BROWSE, CMD_BROWSE},
  };
  int x = 6, n = 0;
  for (auto& d : defs) {
    int w = (d.icon < 0) ? kSepW : kCellW;
    g_tb.items[n++] = { d.icon, d.cmd, x, w };
    x += w;
  }
  g_tb.nItems = n;
}

static int HitItem(int px) {
  for (int i = 0; i < g_tb.nItems; i++) {
    const TbItem& it = g_tb.items[i];
    if (it.icon >= 0 && px >= it.x && px < it.x + it.w) return i;
  }
  return -1;
}

static void DrawSparkline(ID2D1DCRenderTarget* rt, int W, int H) {
  if (g_tb.ringCount < 2) return;
  float gx0 = (float)(W - kMetricsW), gx1 = (float)(W - 14);
  float gy0 = 6.0f, gy1 = (float)(H - 20);
  float mn = 1e30f, mx = -1e30f;
  for (int i = 0; i < g_tb.ringCount; i++) {
    float v = RingAt(i);
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  if (mx <= mn) mx = mn + 1.0f;
  auto px = [&](int i) { return gx0 + (gx1 - gx0) * i / (float)(g_tb.ringCount - 1); };
  auto py = [&](int i) { return gy1 - (gy1 - gy0) * (RingAt(i) - mn) / (mx - mn); };

  ID2D1SolidColorBrush *line = nullptr, *fill = nullptr, *base = nullptr;
  rt->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.46f, 0.86f), &line);
  rt->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.46f, 0.86f, 0.13f), &fill);
  rt->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.57f, 0.62f, 0.5f), &base);
  if (base) rt->DrawLine(D2D1::Point2F(gx0, gy1), D2D1::Point2F(gx1, gy1), base, 0.6f);
  // filled area under the curve
  if (fill) {
    ID2D1PathGeometry* g = nullptr;
    if (SUCCEEDED(g_tb.fac->CreatePathGeometry(&g))) {
      ID2D1GeometrySink* s = nullptr;
      if (SUCCEEDED(g->Open(&s))) {
        s->BeginFigure(D2D1::Point2F(px(0), gy1), D2D1_FIGURE_BEGIN_FILLED);
        for (int i = 0; i < g_tb.ringCount; i++) s->AddLine(D2D1::Point2F(px(i), py(i)));
        s->AddLine(D2D1::Point2F(px(g_tb.ringCount - 1), gy1));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close(); s->Release();
        rt->FillGeometry(g, fill);
      }
      g->Release();
    }
  }
  if (line)
    for (int i = 0; i < g_tb.ringCount - 1; i++)
      rt->DrawLine(D2D1::Point2F(px(i), py(i)), D2D1::Point2F(px(i + 1), py(i + 1)), line, 1.5f);
  if (line) line->Release();
  if (fill) fill->Release();
  if (base) base->Release();
}

static void PaintBand(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc; GetClientRect(hwnd, &rc);
  int W = rc.right, H = rc.bottom;

  if (g_tb.fac != nullptr) {
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ID2D1DCRenderTarget* rt = nullptr;
    if (SUCCEEDED(g_tb.fac->CreateDCRenderTarget(&props, &rt))) {
      RECT full = { 0, 0, W, H };
      rt->BindDC(hdc, &full);
      rt->BeginDraw();

      // Textured/shaded background: a vertical gradient + faint brushed-metal
      // hairlines (source-over blending honours the low alpha even on an opaque DC).
      D2D1_GRADIENT_STOP gs[3] = {
        { 0.00f, D2D1::ColorF(0.985f, 0.985f, 0.99f) },
        { 0.55f, D2D1::ColorF(0.945f, 0.95f, 0.96f) },
        { 1.00f, D2D1::ColorF(0.90f, 0.91f, 0.925f) },
      };
      ID2D1GradientStopCollection* stops = nullptr;
      ID2D1LinearGradientBrush* grad = nullptr;
      if (SUCCEEDED(rt->CreateGradientStopCollection(gs, 3, &stops)) && stops) {
        rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, (float)H)),
            stops, &grad);
      }
      if (grad) rt->FillRectangle(D2D1::RectF(0, 0, (float)W, (float)H), grad);
      else rt->Clear(D2D1::ColorF(0.95f, 0.95f, 0.96f));

      ID2D1SolidColorBrush *tex = nullptr, *border = nullptr, *ink = nullptr,
                           *accent = nullptr, *hov = nullptr;
      rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f), &tex);     // brushed lines
      rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.79f, 0.82f), &border);
      rt->CreateSolidColorBrush(D2D1::ColorF(0.16f, 0.17f, 0.20f), &ink);
      rt->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.60f, 0.24f), &accent);
      rt->CreateSolidColorBrush(D2D1::ColorF(0.80f, 0.87f, 0.98f), &hov);
      if (tex)
        for (int y = 1; y < H; y += 2)
          rt->DrawLine(D2D1::Point2F(0, (float)y), D2D1::Point2F((float)W, (float)y), tex, 0.5f);
      if (border)
        rt->DrawLine(D2D1::Point2F(0, H - 0.5f), D2D1::Point2F((float)W, H - 0.5f), border, 1.0f);

      for (int i = 0; i < g_tb.nItems; i++) {
        const TbItem& it = g_tb.items[i];
        if (it.icon < 0) {
          if (border)
            rt->DrawLine(D2D1::Point2F(it.x + it.w / 2.0f, 12),
                         D2D1::Point2F(it.x + it.w / 2.0f, (float)H - 12), border, 1.0f);
          continue;
        }
        if (i == g_tb.hover && hov) {
          D2D1_ROUNDED_RECT rr = { D2D1::RectF((float)it.x + 2, 6,
                                               (float)it.x + it.w - 2, (float)H - 8), 5, 5 };
          rt->FillRoundedRectangle(rr, hov);
        }
        float s = (float)kIconDraw / kIconBox;
        float ox = it.x + (it.w - kIconDraw) / 2.0f;
        float oy = (H - kIconDraw) / 2.0f;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s) *
                         D2D1::Matrix3x2F::Translation(ox, oy));
        if (ink && accent) DrawGlyph(rt, g_tb.fac, it.icon, ink, accent);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
      }

      DrawSparkline(rt, W, H);

      rt->EndDraw();
      if (grad) grad->Release();
      if (stops) stops->Release();
      if (tex) tex->Release();
      if (border) border->Release();
      if (ink) ink->Release();
      if (accent) accent->Release();
      if (hov) hov->Release();
      rt->Release();
    }
  }

  // The metric name + current value, under the graph (GDI text over the D2D band).
  if (!g_tb.label.empty()) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(64, 70, 82));
    static HFONT f = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Segoe UI");
    HGDIOBJ of = SelectObject(hdc, f);
    RECT mr = { W - kMetricsW, H - 18, W - 12, H - 4 };
    DrawTextW(hdc, g_tb.label.c_str(), -1, &mr, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(hdc, of);
  }
  EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK TbProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  switch (msg) {
    case WM_PAINT: PaintBand(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
      int h = HitItem(LOWORD(l));
      if (h != g_tb.hover) { g_tb.hover = h; InvalidateRect(hwnd, nullptr, FALSE); }
      TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
      TrackMouseEvent(&tme);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (g_tb.hover != -1) { g_tb.hover = -1; InvalidateRect(hwnd, nullptr, FALSE); }
      return 0;
    case WM_LBUTTONDOWN: {
      int h = HitItem(LOWORD(l));
      if (h >= 0 && g_tb.items[h].cmd != 0) {
        PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(g_tb.items[h].cmd, 0),
                     reinterpret_cast<LPARAM>(hwnd));
      }
      return 0;
    }
    default: return DefWindowProcW(hwnd, msg, w, l);
  }
}

HWND WinToolbarCreate(HWND parent) {
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                    reinterpret_cast<void**>(&g_tb.fac));
  LayoutItems();
  WNDCLASSW wc = {};
  wc.lpfnWndProc = TbProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kTbClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  g_tb.hwnd = CreateWindowExW(0, kTbClass, nullptr, WS_CHILD | WS_VISIBLE,
      0, 0, 0, kBandH, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
  return g_tb.hwnd;
}

int WinToolbarHeight(HWND /*tb*/) { return kBandH; }

// Push one metric sample into the rotating buffer (the graph) + set the value label.
void WinToolbarPushMetric(double value, const wchar_t* label) {
  g_tb.ring[g_tb.ringHead] = (float)value;
  g_tb.ringHead = (g_tb.ringHead + 1) % kRing;
  if (g_tb.ringCount < kRing) g_tb.ringCount++;
  g_tb.label = (label != nullptr) ? label : L"";
  if (g_tb.hwnd != nullptr) InvalidateRect(g_tb.hwnd, nullptr, FALSE);
}

}  // namespace bin
}  // namespace dart
