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
#include <commctrl.h>     // SysListView32 / SysTabControl32 (S7 widget kinds)
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
  return WidgetKind::kUnknown;
}

// ── singleton ───────────────────────────────────────────────────────────────
ViewServer& ViewServer::Instance() {
  static ViewServer instance;
  return instance;
}

Surface* ViewServer::OpenPane(int /*w*/, int /*h*/) {
  // S4 slice: a pane's host is the main workspace window itself, so a described
  // button becomes a direct child of it (WM_COMMAND routes to the shared WndProc,
  // and the headless self-click's EnumChildWindows(main) finds it). A dedicated
  // WS_CHILD host per pane is a later refinement (multi-pane layout).
  int64_t t = next_ticket_++;
  Surface s;
  s.ticket = t;
  s.host = WinHostMainHwnd();
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
    default:
      // Parsed, rejected cleanly (never an illegal Win32 call).
      return "unsupported widget kind: " + kind;
  }

  bool sized = (wk == WidgetKind::kCanvas || wk == WidgetKind::kGame);
  int cw = sized ? (int)MapInt(props, "w") : 10;
  int ch = sized ? (int)MapInt(props, "h") : 10;
  HWND h = CreateWindowExW(0, cls, text.c_str(), style,
                           0, 0, cw > 0 ? cw : 10, ch > 0 ? ch : 10, s->host,
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
  }

  Widget w;
  w.ticket = ticket;
  w.id = id;
  w.kind = wk;
  w.hwnd = h;
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
    SendMessageW(w->hwnd, TCM_SETCURSEL, (WPARAM)MapInt(props, "tab"), 0);
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
