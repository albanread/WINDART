// WINDART view-server materializer — the ViewServer registry that Win_surfaceApply
// drives. This is the one large NET-NEW C++ surface in S4 (WINVM materialized
// nothing — it was WebView2). See S4_GUI_HOST_DESIGN.md §2.2, §4.
//
// S4 iteration-1 SCOPE (the vertical slice): OpenPane/OpenWindow, and the
// materialize verbs the button milestone needs — 'clear' | 'add' (button, label)
// | 'place' | 'set' | 'remove' | 'title' | 'focus'. Other widget kinds
// (field/list/editor/canvas/…) parse but report "unsupported (S4 slice)" rather
// than crash — the whole safety win of a validated batch over the deleted dynamic
// send (APP_PANE_PLAN.md §5). They become additive once the framework lives.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>     // GET_X_LPARAM / GET_Y_LPARAM (splitter drag, P2)
#include <commctrl.h>     // SysListView32 / SysTabControl32 (S7 widget kinds) + SetWindowSubclass
#include <richedit.h>     // MSFTEDIT_CLASS (the code editor, S7.2)

#include <cstdint>
#include <string>

#include "include/dart_api.h"
#include "win_view.h"
#include "win_host.h"     // WinHostMainHwnd, WinHostWindowClassName
#include "win_canvas.h"   // CanvasCreate/Destroy (S5)
#include "gp_engine_d3d.h" // GpEngine live-present target (T3 game widget)

#pragma comment(lib, "comctl32.lib")

namespace dart {
namespace bin {

// ── small decode helpers ────────────────────────────────────────────────────
static std::wstring Utf16(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

static std::string DartStr(Dart_Handle h) {
  if (h == nullptr || !Dart_IsString(h)) return "";
  const char* c = nullptr;
  if (Dart_IsError(Dart_StringToCString(h, &c)) || c == nullptr) return "";
  return std::string(c);
}

static int64_t DartInt(Dart_Handle h) {
  int64_t v = 0;
  if (h == nullptr) return v;
  if (Dart_IsInteger(h)) { Dart_IntegerToInt64(h, &v); return v; }
  // Frames from layout arithmetic often arrive as doubles (e.g. the app pane's
  // grid math). Round to the nearest pixel rather than decoding them as 0.
  if (Dart_IsDouble(h)) { double d = 0; Dart_DoubleValue(h, &d); return (int64_t)(d < 0 ? d - 0.5 : d + 0.5); }
  return v;
}

static std::string ListStr(Dart_Handle list, intptr_t i) {
  return DartStr(Dart_ListGetAt(list, i));
}
static int64_t ListInt(Dart_Handle list, intptr_t i) {
  return DartInt(Dart_ListGetAt(list, i));
}

// props is a Dart Map<String,dynamic>; pull one String value (or "").
static std::string MapStr(Dart_Handle map, const char* key) {
  if (map == nullptr || !Dart_IsMap(map)) return "";
  Dart_Handle v = Dart_MapGetAt(map, Dart_NewStringFromCString(key));
  return DartStr(v);
}
static int64_t MapInt(Dart_Handle map, const char* key) {
  if (map == nullptr || !Dart_IsMap(map)) return 0;
  return DartInt(Dart_MapGetAt(map, Dart_NewStringFromCString(key)));
}
static Dart_Handle MapVal(Dart_Handle map, const char* key) {
  if (map == nullptr || !Dart_IsMap(map)) return Dart_Null();
  return Dart_MapGetAt(map, Dart_NewStringFromCString(key));
}
static bool MapBool(Dart_Handle map, const char* key) {
  Dart_Handle v = MapVal(map, key);
  bool b = false;
  if (v != nullptr && Dart_IsBoolean(v)) Dart_BooleanValue(v, &b);
  return b;
}

// ── kind parsing ────────────────────────────────────────────────────────────
WidgetKind ParseWidgetKind(const std::string& k) {
  if (k == "label") return WidgetKind::kLabel;
  if (k == "field") return WidgetKind::kField;
  if (k == "button") return WidgetKind::kButton;
  if (k == "checkbox") return WidgetKind::kCheckbox;
  if (k == "radio") return WidgetKind::kRadio;
  if (k == "popup") return WidgetKind::kPopup;
  if (k == "list") return WidgetKind::kList;
  if (k == "text") return WidgetKind::kText;
  if (k == "box") return WidgetKind::kBox;
  if (k == "image") return WidgetKind::kImage;
  if (k == "canvas") return WidgetKind::kCanvas;
  if (k == "game") return WidgetKind::kGame;
  if (k == "tabs") return WidgetKind::kTabs;
  if (k == "slider") return WidgetKind::kSlider;
  if (k == "progress") return WidgetKind::kProgress;
  if (k == "splitter") return WidgetKind::kSplitter;
  return WidgetKind::kUnknown;
}

// ── singleton ───────────────────────────────────────────────────────────────
ViewServer& ViewServer::Instance() {
  static ViewServer instance;
  return instance;
}

Surface* ViewServer::OpenPane(int /*w*/, int /*h*/) {
  // The pane's host is the main workspace window — UNLESS the host reserved a top
  // band for the icon toolbar (polish), in which case we create a WS_CHILD
  // container filling the client BELOW the toolbar and parent the app's widgets
  // there, so they never overlap the toolbar. The container uses the shared window
  // class, so its children's WM_COMMAND/WM_NOTIFY route to the same WndProc.
  int64_t t = next_ticket_++;
  Surface s;
  s.ticket = t;
  HWND main = WinHostMainHwnd();
  HWND host = main;
  int top = WinHostToolbarHeight();
  int bot = WinHostStatusHeight();
  if ((top > 0 || bot > 0) && main != nullptr) {
    RECT rc; GetClientRect(main, &rc);
    HWND container = CreateWindowExW(0, WinHostWindowClassName(), L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, top, rc.right - rc.left, rc.bottom - rc.top - top - bot,
        main, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (container != nullptr) host = container;
  }
  s.host = host;
  s.isWindow = false;
  Surface& ref = (surfaces_[t] = s);
  return &ref;
}

Surface* ViewServer::OpenWindow(const std::wstring& title, int w, int h) {
  int64_t t = next_ticket_++;
  HWND hw = CreateWindowExW(0, WinHostWindowClassName(), title.c_str(),
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Surface s;
  s.ticket = t;
  s.host = hw;
  s.isWindow = true;
  Surface& ref = (surfaces_[t] = s);
  return &ref;
}

Surface* ViewServer::SurfaceByTicket(int64_t ticket) {
  auto it = surfaces_.find(ticket);
  return it == surfaces_.end() ? nullptr : &it->second;
}

Surface* ViewServer::SurfaceByHost(HWND host) {
  for (auto& kv : surfaces_) {
    if (kv.second.host == host) return &kv.second;
  }
  return nullptr;
}

Widget* ViewServer::WidgetByTicket(int64_t ticket) {
  auto it = by_ticket_.find(ticket);
  return it == by_ticket_.end() ? nullptr : it->second;
}

static Widget* WidgetInSurface(Surface* s, const std::string& id) {
  auto it = s->widgets.find(id);
  return it == s->widgets.end() ? nullptr : &it->second;
}

// ── Draggable splitter (P2) ──────────────────────────────────────────────────
// A thin divider between two neighbor panes. Self-contained: the drag resizes the
// neighbors ENTIRELY in C++ (MoveWindow), never round-tripping to Dart per mouse
// move (the design note). It resolves its neighbors by id at drag time (from the
// surface), so creation order and post-rebuild HWND churn don't matter. The
// neighbors are addressed by id via the SURFACE TICKET (a Surface* would dangle if
// surfaces_ rehashes; the ticket is stable).
struct SplitterCtx {
  int64_t     surfaceTicket = 0;
  std::string leftId, rightId;   // left/top neighbor, right/bottom neighbor
  bool        vertical = true;    // true = vertical bar, moves horizontally (IDC_SIZEWE)
  bool        dragging = false;
  int         grab = 0;           // cursor offset within the bar at grab time
};

static RECT ChildRectInParent(HWND child, HWND parent) {
  RECT r; GetWindowRect(child, &r);
  POINT tl = {r.left, r.top}, br = {r.right, r.bottom};
  ScreenToClient(parent, &tl); ScreenToClient(parent, &br);
  RECT out = {tl.x, tl.y, br.x, br.y};
  return out;
}

static LRESULT CALLBACK SplitterProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR /*uid*/, DWORD_PTR ref) {
  SplitterCtx* ctx = reinterpret_cast<SplitterCtx*>(ref);
  switch (msg) {
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, ctx->vertical ? IDC_SIZEWE : IDC_SIZENS));
      return TRUE;
    case WM_LBUTTONDOWN:
      SetCapture(hwnd);
      ctx->dragging = true;
      ctx->grab = ctx->vertical ? GET_X_LPARAM(lp) : GET_Y_LPARAM(lp);
      return 0;
    case WM_LBUTTONUP:
      if (ctx->dragging) { ctx->dragging = false; ReleaseCapture(); }
      return 0;
    case WM_CAPTURECHANGED:
      // Capture stolen mid-drag (Alt+Tab, a dialog/menu, screen lock). End the
      // drag so subsequent buttonless hover-moves don't keep resizing the panes.
      ctx->dragging = false;
      return 0;
    case WM_MOUSEMOVE: {
      if (!ctx->dragging) break;
      if (!(wp & MK_LBUTTON)) {          // button no longer down (missed up / lost capture)
        ctx->dragging = false;
        if (GetCapture() == hwnd) ReleaseCapture();
        break;
      }
      HWND parent = GetParent(hwnd);
      RECT sr = ChildRectInParent(hwnd, parent);
      int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
      Surface* s = ViewServer::Instance().SurfaceByTicket(ctx->surfaceTicket);
      Widget* lw = s ? WidgetInSurface(s, ctx->leftId) : nullptr;
      Widget* rw = s ? WidgetInSurface(s, ctx->rightId) : nullptr;
      RECT lr = (lw && lw->hwnd) ? ChildRectInParent(lw->hwnd, parent) : sr;
      RECT rr = (rw && rw->hwnd) ? ChildRectInParent(rw->hwnd, parent) : sr;
      if (ctx->vertical) {
        int newLeft = sr.left + GET_X_LPARAM(lp) - ctx->grab;
        int minLeft = ((lw && lw->hwnd) ? lr.left : 0) + 48;
        int maxLeft = ((rw && rw->hwnd) ? rr.right : newLeft + sw + 48) - sw - 48;
        if (maxLeft < minLeft) maxLeft = minLeft;  // extreme shrink: keep the left pane's min, don't invert
        if (newLeft < minLeft) newLeft = minLeft;
        if (newLeft > maxLeft) newLeft = maxLeft;
        if (lw && lw->hwnd)
          MoveWindow(lw->hwnd, lr.left, lr.top, newLeft - lr.left, lr.bottom - lr.top, TRUE);
        if (rw && rw->hwnd) { int rx = newLeft + sw;
          MoveWindow(rw->hwnd, rx, rr.top, rr.right - rx, rr.bottom - rr.top, TRUE); }
        MoveWindow(hwnd, newLeft, sr.top, sw, sh, TRUE);
      } else {
        int newTop = sr.top + GET_Y_LPARAM(lp) - ctx->grab;
        int minTop = ((lw && lw->hwnd) ? lr.top : 0) + 40;
        int maxTop = ((rw && rw->hwnd) ? rr.bottom : newTop + sh + 40) - sh - 40;
        if (maxTop < minTop) maxTop = minTop;      // extreme shrink: keep the top pane's min, don't invert
        if (newTop < minTop) newTop = minTop;
        if (newTop > maxTop) newTop = maxTop;
        if (lw && lw->hwnd)
          MoveWindow(lw->hwnd, lr.left, lr.top, lr.right - lr.left, newTop - lr.top, TRUE);
        if (rw && rw->hwnd) { int ry = newTop + sh;
          MoveWindow(rw->hwnd, rr.left, ry, rr.right - rr.left, rr.bottom - ry, TRUE); }
        MoveWindow(hwnd, sr.left, newTop, sw, sh, TRUE);
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
      RECT rc; GetClientRect(hwnd, &rc);
      FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
      DrawEdge(dc, &rc, EDGE_RAISED, ctx->vertical ? (BF_LEFT | BF_RIGHT) : (BF_TOP | BF_BOTTOM));
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCDESTROY:
      RemoveWindowSubclass(hwnd, SplitterProc, 1);
      delete ctx;
      break;
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

void ViewServer::Close(int64_t surface) {
  Surface* s = SurfaceByTicket(surface);
  if (!s) return;
  ClearSurface(s);
  if (s->isWindow && s->host) DestroyWindow(s->host);
  surfaces_.erase(surface);
}

// disposeCallbacks (§3.2): destroy the OWNED child controls AND forget tickets —
// a bare map-clear would leak windows (Win32 controls are owned, unlike AppKit's
// weakly-held targets).
void ViewServer::ClearSurface(Surface* s) {
  for (auto& kv : s->widgets) {
    if (kv.second.kind == WidgetKind::kCanvas) CanvasDestroy(kv.second.ticket);
    if (kv.second.kind == WidgetKind::kGame)
      windart_gamepane::GpEngine::instance()->detach_present();  // drop the swapchain first
    if (kv.second.hwnd) DestroyWindow(kv.second.hwnd);
    by_ticket_.erase(kv.second.ticket);
  }
  s->widgets.clear();
}

// ── the one materialize verb: 'add' ─────────────────────────────────────────
// The parent a content widget is created under: the ACTIVE tab page when the surface
// has tab pages, else the container itself. Frames stay container-relative because
// each page fills the container (origin 0,0).
static HWND ContentParent(Surface* s) {
  if (!s->tab_pages.empty() && s->active_tab >= 0 &&
      s->active_tab < static_cast<int>(s->tab_pages.size()))
    return s->tab_pages[s->active_tab];
  return s->host;
}

// Select tab page i: show it, hide the rest, keep the strip on top. The strip's own
// highlighted tab is set by the OS (a click) or TCM_SETCURSEL (programmatic) — this
// only drives page visibility, which is what stops inactive tabs bleeding.
static void SwitchTabPage(Surface* s, int i) {
  if (!s || i < 0 || i >= static_cast<int>(s->tab_pages.size())) return;
  for (int j = 0; j < static_cast<int>(s->tab_pages.size()); j++)
    ShowWindow(s->tab_pages[j], j == i ? SW_SHOW : SW_HIDE);
  s->active_tab = i;
  if (s->tab_ctrl)
    SetWindowPos(s->tab_ctrl, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

static std::string MaterializeAdd(ViewServer* vs, Surface* s,
                                  const std::string& kind, const std::string& id,
                                  int64_t ticket, Dart_Handle props,
                                  std::unordered_map<int64_t, Widget*>* by_ticket) {
  WidgetKind wk = ParseWidgetKind(kind);
  const wchar_t* cls = nullptr;
  DWORD style = WS_CHILD | WS_VISIBLE;
  std::wstring text;
  switch (wk) {
    case WidgetKind::kButton:
      cls = L"BUTTON"; style |= BS_PUSHBUTTON | WS_TABSTOP;
      text = Utf16(MapStr(props, "title"));
      break;
    case WidgetKind::kLabel:
      cls = L"STATIC"; style |= SS_LEFT;
      text = Utf16(MapStr(props, "text"));
      break;
    case WidgetKind::kCanvas:
      // A child host window; the pixels live in an offscreen D2D/WIC target (S5).
      cls = L"STATIC"; style |= SS_OWNERDRAW;
      break;
    case WidgetKind::kGame:
      // T3: a child host window the D3D11 game pane presents into via a DXGI
      // swapchain. SS_OWNERDRAW suppresses the STATIC's default background erase
      // (like kCanvas), so DXGI's presented frame is never overpainted. Add
      // WS_CLIPSIBLINGS so sibling controls don't bleed over the swapchain.
      cls = L"STATIC"; style |= SS_OWNERDRAW | WS_CLIPSIBLINGS;
      break;
    case WidgetKind::kField:                 // S7: editable text field
      cls = L"EDIT"; style |= ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP;
      if (MapStr(props, "align") == "right") style |= ES_RIGHT;
      else if (MapStr(props, "align") == "center") style |= ES_CENTER;
      if (MapBool(props, "readOnly")) style |= ES_READONLY;
      text = Utf16(MapStr(props, "text"));
      break;
    case WidgetKind::kBox:                    // S7: labeled group frame
      cls = L"BUTTON"; style |= BS_GROUPBOX;
      text = Utf16(MapStr(props, "title"));
      break;
    case WidgetKind::kPopup:                   // S7: drop-down (COMBOBOX)
      cls = L"COMBOBOX"; style |= CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
      break;
    case WidgetKind::kList:                    // S7: virtual table (owner-data)
      cls = WC_LISTVIEWW;
      style |= LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP;
      break;
    case WidgetKind::kTabs:                     // S7: tab strip
      cls = WC_TABCONTROLW; style |= WS_CLIPSIBLINGS;
      break;
    case WidgetKind::kText:                     // S7.2: the code editor (RichEdit)
      cls = MSFTEDIT_CLASS;                     // RICHEDIT50W (Msftedit.dll loaded at host init)
      style |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL |
               WS_BORDER | WS_TABSTOP;
      text = Utf16(MapStr(props, "text"));
      break;
    case WidgetKind::kSplitter:                 // P2: a draggable pane divider
      // SS_NOTIFY makes the STATIC return HTCLIENT so it receives the mouse
      // messages the subclass proc drags with (a bare STATIC is HTTRANSPARENT).
      cls = L"STATIC"; style |= SS_NOTIFY;
      break;
    default:
      // Parsed, rejected cleanly (never an illegal Win32 call).
      return "unsupported widget kind: " + kind;
  }

  bool sized = (wk == WidgetKind::kCanvas || wk == WidgetKind::kGame);
  int cw = sized ? (int)MapInt(props, "w") : 10;
  int ch = sized ? (int)MapInt(props, "h") : 10;
  // The tab strip lives on the container; every other widget lives on the ACTIVE tab
  // page, so page show/hide governs visibility (no bleed, resize for free).
  HWND parent = (wk == WidgetKind::kTabs) ? s->host : ContentParent(s);
  HWND h = CreateWindowExW(0, cls, text.c_str(), style,
                           0, 0, cw > 0 ? cw : 10, ch > 0 ? ch : 10, parent,
                           reinterpret_cast<HMENU>(static_cast<UINT_PTR>(ticket)),
                           GetModuleHandleW(nullptr), nullptr);
  if (!h) return "CreateWindowExW failed for id=" + id;
  SendMessageW(h, WM_SETFONT,
               reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);

  // ── kind-specific post-create setup ───────────────────────────────────────
  if (wk == WidgetKind::kList) {
    ListView_SetExtendedListViewStyle(h, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col = {0};
    col.mask = LVCF_WIDTH | LVCF_TEXT;
    col.cx = 600;                       // wide; the list clips to its own width
    wchar_t hdr[] = L"";
    col.pszText = hdr;
    SendMessageW(h, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
    SendMessageW(h, LVM_SETITEMCOUNT, (WPARAM)MapInt(props, "rows"), 0);  // virtual
  } else if (wk == WidgetKind::kText) {
    // A monospace code font for the editor (created once, reused).
    static HFONT mono = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (mono) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);
    // NB: do NOT set ENM_CHANGE here. A RichEdit created/set WITH text fires
    // EN_CHANGE synchronously during Win_surfaceApply, which would re-enter
    // OnSurfaceCommand (Dart_EnterScope) inside another native's execution and
    // corrupt the scope stack (allocation.cc:37 top==this). Live onText, when
    // needed, must arrive via a deferred/queued path, not a re-entrant dispatch.
  } else if (wk == WidgetKind::kPopup || wk == WidgetKind::kTabs) {
    Dart_Handle items = MapVal(props, "items");
    intptr_t n = 0;
    if (items != nullptr && Dart_IsList(items) &&
        !Dart_IsError(Dart_ListLength(items, &n))) {
      for (intptr_t i = 0; i < n; i++) {
        std::wstring item = Utf16(DartStr(Dart_ListGetAt(items, i)));
        if (wk == WidgetKind::kPopup) {
          SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        } else {
          TCITEMW ti = {0};
          ti.mask = TCIF_TEXT;
          ti.pszText = const_cast<wchar_t*>(item.c_str());
          SendMessageW(h, TCM_INSERTITEMW, (WPARAM)i, reinterpret_cast<LPARAM>(&ti));
        }
      }
    }
    if (wk == WidgetKind::kTabs) {
      // One native page window per tab item, filling the container; only page 0 is
      // shown. Content widgets parent to the active page (ContentParent), so an
      // inactive tab's controls are hidden and cannot bleed. Kept behind the strip
      // in z-order so the tab buttons stay visible.
      RECT rc; GetClientRect(s->host, &rc);
      for (intptr_t i = 0; i < n; i++) {
        HWND page = CreateWindowExW(
            0, WinHostWindowClassName(), L"",
            WS_CHILD | WS_CLIPCHILDREN | (i == 0 ? WS_VISIBLE : 0),
            0, 0, rc.right, rc.bottom, s->host, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        SetWindowPos(page, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        s->tab_pages.push_back(page);
      }
      s->tab_ctrl = h;
      s->active_tab = 0;
    }
  }

  bool w_subclassed_hint = false;
  if (wk == WidgetKind::kSplitter) {
    // Subclass the divider to own the drag; it resolves its neighbors by id at
    // drag time (via the surface ticket), so creation order does not matter.
    SplitterCtx* ctx = new SplitterCtx();
    ctx->surfaceTicket = s->ticket;
    ctx->leftId = MapStr(props, "left");
    ctx->rightId = MapStr(props, "right");
    ctx->vertical = (MapStr(props, "orientation") != "horizontal");
    SetWindowSubclass(h, SplitterProc, 1, reinterpret_cast<DWORD_PTR>(ctx));
    w_subclassed_hint = true;
  }

  Widget w;
  w.ticket = ticket;
  w.id = id;
  w.kind = wk;
  w.hwnd = h;
  w.subclassed = w_subclassed_hint;
  Widget& ref = (s->widgets[id] = w);
  (*by_ticket)[ticket] = &ref;   // unordered_map: refs/ptrs stable across inserts
  if (wk == WidgetKind::kCanvas) CanvasCreate(ticket, cw, ch, h);
  if (wk == WidgetKind::kGame) {
    // Bind the D3D11 game pane's live present to this child HWND. The swapchain
    // is created lazily on the first present (after gpOpen makes the device).
    windart_gamepane::GpEngine::instance()->set_present_target(h);
  }
  (void)vs;
  return "";
}

static void DoPlace(Surface* s, const std::string& id, Dart_Handle frame) {
  Widget* w = WidgetInSurface(s, id);
  if (!w || !w->hwnd) return;
  auto at = [&](int i) -> int {
    return (i < 4) ? (int)DartInt(Dart_ListGetAt(frame, i)) : 0;
  };
  MoveWindow(w->hwnd, at(0), at(1), at(2), at(3), TRUE);
}

static void DoSet(Surface* s, const std::string& id, Dart_Handle props) {
  Widget* w = WidgetInSurface(s, id);
  if (!w || !w->hwnd) return;
  // A virtual list's row count changed (the reloadData() -> set(rows:) mapping).
  if (w->kind == WidgetKind::kList && MapVal(props, "rows") != Dart_Null() &&
      Dart_IsInteger(MapVal(props, "rows"))) {
    SendMessageW(w->hwnd, LVM_SETITEMCOUNT, (WPARAM)MapInt(props, "rows"), 0);
    InvalidateRect(w->hwnd, nullptr, TRUE);
    return;
  }
  // Programmatic tab selection (T1: the self-test drives the strip; user clicks
  // set it themselves). TCM_SETCURSEL does not fire TCN_SELCHANGE, so the caller
  // rebuilds the content itself.
  if (w->kind == WidgetKind::kTabs && MapVal(props, "tab") != Dart_Null() &&
      Dart_IsInteger(MapVal(props, "tab"))) {
    int idx = (int)MapInt(props, "tab");
    SendMessageW(w->hwnd, TCM_SETCURSEL, (WPARAM)idx, 0);
    SwitchTabPage(s, idx);   // show that page + hide the rest (programmatic select)
    return;
  }
  // Programmatic combobox selection (the Editor tab's class selector).
  if (w->kind == WidgetKind::kPopup && MapVal(props, "index") != Dart_Null() &&
      Dart_IsInteger(MapVal(props, "index"))) {
    SendMessageW(w->hwnd, CB_SETCURSEL, (WPARAM)MapInt(props, "index"), 0);
    return;
  }
  std::string t = MapStr(props, "title");
  if (t.empty()) t = MapStr(props, "text");
  if (!t.empty()) SetWindowTextW(w->hwnd, Utf16(t).c_str());
}

static void DoRemove(ViewServer* vs, Surface* s, const std::string& id) {
  Widget* w = WidgetInSurface(s, id);
  if (!w) return;
  if (w->kind == WidgetKind::kCanvas) CanvasDestroy(w->ticket);
  if (w->kind == WidgetKind::kGame)
    windart_gamepane::GpEngine::instance()->detach_present();  // release the swapchain
  if (w->hwnd) DestroyWindow(w->hwnd);
  vs->ForgetTicket(w->ticket);
  s->widgets.erase(id);
}

static bool g_in_apply = false;
bool WinViewInApply() { return g_in_apply; }

// RAII: set g_in_apply for the duration of Apply (exception-safe).
struct ApplyGuard { ApplyGuard() { g_in_apply = true; } ~ApplyGuard() { g_in_apply = false; } };

std::string ViewServer::Apply(int64_t surface, int64_t gen, const void* cmdsPtr) {
  ApplyGuard guard;
  Surface* s = SurfaceByTicket(surface);
  if (!s) return "surface not found";
  s->gen = gen;
  Dart_Handle cmds = *reinterpret_cast<const Dart_Handle*>(cmdsPtr);
  intptr_t n = 0;
  if (cmds == nullptr || Dart_IsError(Dart_ListLength(cmds, &n))) {
    return "apply: cmds is not a list";
  }
  std::string firstErr;
  auto note = [&](const std::string& e) { if (!e.empty() && firstErr.empty()) firstErr = e; };

  for (intptr_t i = 0; i < n; i++) {
    Dart_Handle cmd = Dart_ListGetAt(cmds, i);
    std::string verb = ListStr(cmd, 0);
    if (verb == "add") {
      note(MaterializeAdd(this, s, ListStr(cmd, 1), ListStr(cmd, 2),
                          ListInt(cmd, 3), Dart_ListGetAt(cmd, 4), &by_ticket_));
    } else if (verb == "place") {
      DoPlace(s, ListStr(cmd, 1), Dart_ListGetAt(cmd, 2));
    } else if (verb == "set") {
      DoSet(s, ListStr(cmd, 1), Dart_ListGetAt(cmd, 2));
    } else if (verb == "remove") {
      DoRemove(this, s, ListStr(cmd, 1));
    } else if (verb == "title") {
      SetWindowTextW(s->host, Utf16(ListStr(cmd, 1)).c_str());
    } else if (verb == "focus") {
      Widget* w = WidgetInSurface(s, ListStr(cmd, 1));
      if (w && w->hwnd) SetFocus(w->hwnd);
    } else if (verb == "clear") {
      ClearSurface(s);
    } else {
      note("unknown command: " + verb);
    }
  }
  return firstErr;
}

void ViewServer::ForgetTicket(int64_t ticket) { by_ticket_.erase(ticket); }

HWND ViewServer::SurfaceHost(int64_t surface) {
  Surface* s = SurfaceByTicket(surface);
  return s ? s->host : nullptr;
}

// Tab pages — the callback layer (win_callbacks.cpp) drives these on a tab click /
// container resize; SwitchTabPage does the show/hide.
void ViewServer::ShowTabPage(HWND host, int i) {
  SwitchTabPage(SurfaceByHost(host), i);
}
void ViewServer::ResizeTabPages(HWND host, int w, int h) {
  Surface* s = SurfaceByHost(host);
  if (!s) return;
  for (HWND page : s->tab_pages) MoveWindow(page, 0, 0, w, h, TRUE);
}

// The first canvas widget's ticket in a surface (0 if none) — backs the unified
// Win_surfaceSnapshot (S6): one PNG-readback primitive for both the demo canvas
// and, once the D3D11 game pane lands, the game pane (S7's headless regression loop).
int64_t ViewServer::CanvasTicketInSurface(int64_t surface) {
  Surface* s = SurfaceByTicket(surface);
  if (!s) return 0;
  for (auto& kv : s->widgets) {
    if (kv.second.kind == WidgetKind::kCanvas) return kv.second.ticket;
  }
  return 0;
}

}  // namespace bin
}  // namespace dart
