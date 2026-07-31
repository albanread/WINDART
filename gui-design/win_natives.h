// WINDART dart:win native resolver. Parallels macdart/cocoa/cocoa_natives.h and
// bin/io_natives.h — the Builtin native lookup chain falls through to
// WinNativeLookup for dart:win (patched into bin/builtin_natives.cc, see
// S4_GUI_HOST_DESIGN.md §6 hunk 2).
#ifndef WINDART_WIN_NATIVES_H_
#define WINDART_WIN_NATIVES_H_

#include "include/dart_api.h"

namespace dart {
namespace bin {

Dart_NativeFunction WinNativeLookup(Dart_Handle name,
                                    int argument_count,
                                    bool* auto_setup_scope);
const uint8_t* WinNativeSymbol(Dart_NativeFunction nf);

}  // namespace bin
}  // namespace dart

#endif  // WINDART_WIN_NATIVES_H_
