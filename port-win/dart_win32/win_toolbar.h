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

}  // namespace bin
}  // namespace dart

#endif  // WINDART_WIN_TOOLBAR_H_
