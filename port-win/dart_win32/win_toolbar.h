// WINDART polish — the icon toolbar (win_toolbar.cpp).
#ifndef WINDART_WIN_TOOLBAR_H_
#define WINDART_WIN_TOOLBAR_H_

#include <windows.h>

namespace dart {
namespace bin {

// Create the icon toolbar as a child of `parent` (CCS_TOP band). Returns the
// toolbar HWND (or nullptr). Buttons carry the shared WinHostCommand ids.
HWND WinToolbarCreate(HWND parent);

// The toolbar's actual band height (for the pane container's top inset).
int WinToolbarHeight(HWND tb);

// Push one VM-metric sample into the toolbar's rotating buffer (drawn as a live
// sparkline) and set the value label under it. Called from Dart (wsPushToolbarMetric).
void WinToolbarPushMetric(double value, const wchar_t* label);

}  // namespace bin
}  // namespace dart

#endif  // WINDART_WIN_TOOLBAR_H_
