// WINDART view-server — the materializer's internal model. Net-new C++ (WINVM
// materialized nothing; it was WebView2). Owns the surface + widget registry that
// Win_surfaceApply drives, and the (surface,id) <-> (hwnd,ticket) mapping the
// callback dispatcher routes against. See S4_GUI_HOST_DESIGN.md §2.2, §3, §4.
#ifndef WINDART_WIN_VIEW_H_
#define WINDART_WIN_VIEW_H_

#include <windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dart {
namespace bin {

// The widget kinds the protocol admits (S4_GUI_HOST_DESIGN.md §4.4). A command
// with an unknown kind is REJECTED and reported — it can never reach an illegal
// Win32 call, which is the whole safety win over the deleted dynamic bridge
// (APP_PANE_PLAN.md §5, "bitten twice").
enum class WidgetKind {
  kLabel, kField, kButton, kCheckbox, kRadio, kPopup, kList, kText /*editor*/,
  kBox, kImage, kCanvas, kGame /*D3D11 game pane, T3*/, kTabs, kSlider, kProgress,
  kUnknown
};

// One materialized control. The control-id assigned at CreateWindowExW time IS
// the ticket the callback dispatcher and Dart share (S4_GUI_HOST_DESIGN.md §3.1).
struct Widget {
  int64_t     ticket = 0;      // == Win32 child control-id == Dart-side handle
  std::string id;              // app-assigned string id ('d', 'k7', …)
  WidgetKind  kind = WidgetKind::kUnknown;
  HWND        hwnd = nullptr;  // the control (or a D2D-drawn widget's host child)
  bool        subclassed = false;
};

// A place widgets live. 'pane' (WS_CHILD in the workspace window) and 'win'
// (WS_OVERLAPPEDWINDOW) are indistinguishable to the app (APP_PANE_PLAN.md §2).
struct Surface {
  int64_t ticket = 0;          // surface handle returned to Dart
  HWND    host = nullptr;      // the parent HWND children are created under
  bool    isWindow = false;    // 'win' vs 'pane'
  int64_t gen = 0;             // generation, for discarding stale applies
  std::unordered_map<std::string, Widget> widgets;   // id -> widget
};

// The global registry. UI-thread only (single-threaded, no lock needed — unlike
// cocoa_callbacks.mm's g_mu, because AppKit finalizers could run off-thread but
// our materializer never does).
class ViewServer {
 public:
  static ViewServer& Instance();

  Surface* OpenPane(int w, int h);                 // Win_surfaceOpenPane
  Surface* OpenWindow(const std::wstring& title, int w, int h);  // Win_surfaceOpenWindow
  void     Close(int64_t surface);                 // Win_surfaceClose

  // The batched materialize (Win_surfaceApply). Applies a whole build's command
  // list atomically: ['clear'|'add'|'set'|'remove'|'place'|'title'|'focus'].
  // Returns "" or the first error (best-effort, like gpApply).
  std::string Apply(int64_t surface, int64_t gen, /*decoded*/ const void* cmds);

  // Routing lookups for win_callbacks.cpp.
  Widget*  WidgetByTicket(int64_t ticket);         // ticket -> widget (or nullptr)
  Surface* SurfaceByHost(HWND host);               // parent HWND -> surface
  Surface* SurfaceByTicket(int64_t ticket);        // surface handle -> surface
  void     ForgetTicket(int64_t ticket);           // drop a widget from by_ticket_
  int64_t  CanvasTicketInSurface(int64_t surface); // first canvas widget's ticket (0 if none)
  HWND     SurfaceHost(int64_t surface);           // a surface's host HWND (nullptr if none)

  // disposeCallbacks (S4_GUI_HOST_DESIGN.md §3.2): destroys owned HWNDs AND forgets
  // tickets — Win32 controls are OWNED, unlike AppKit's weakly-held targets, so a
  // bare map-clear (cocoa.dart:156-158) would leak windows.
  void ClearSurface(Surface* s);

 private:
  int64_t next_ticket_ = 0x1000'0000;   // high range: keeps widget ids disjoint
                                        // from the host's menu-command id block
  std::unordered_map<int64_t, Surface> surfaces_;   // surface ticket -> surface
  std::unordered_map<int64_t, Widget*> by_ticket_;  // widget ticket -> widget
};

// kind string ("button"/"field"/…) -> WidgetKind (kUnknown on a typo).
WidgetKind ParseWidgetKind(const std::string& kind);

// True while ViewServer::Apply is materializing. The callback dispatcher
// (win_callbacks) checks this and SKIPS re-entrant dispatch: a control created/
// set with text can fire EN_CHANGE synchronously during Apply, and re-entering
// Dart (Dart_EnterScope) while Apply holds Dart handles corrupts the scope stack
// (allocation.cc:37). Notifications during Apply are dropped — the app just
// described the surface, it does not need them echoed back.
bool WinViewInApply();

}  // namespace bin
}  // namespace dart

#endif  // WINDART_WIN_VIEW_H_
