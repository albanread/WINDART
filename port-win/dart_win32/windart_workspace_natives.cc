// WINDART workspace runtime natives — the live-workspace eval/reload primitives.
// Port of macdart/cocoa/workspace_natives.cc (cocoa->win): the host-callback names
// are windart_* (strong, defined in win_host.cpp and linked into dart_win32
// everywhere), so no weak-fallback trick is needed. See WORKSPACE_PLAN.md §5.
//   Workspace_eval   — Dart_EvaluateExpr: run one expression against live state (Do It).
//   Workspace_reload — Dart_WorkspaceReloadSources: hot-reload a library (S3 API).
//   Workspace_vmStats — Dart_WorkspaceVmStats: live VM counters.
#include <cstdio>

#include "include/dart_api.h"
#include "include/dart_tools_api.h"
#include "win_host.h"   // windart_ui_ready / _request_ui_reload / _take_ui_reload_status

namespace dart {
namespace bin {

// wsEval(String src) -> String. Evaluates `src` as an expression in the scope of
// the current root library, returning the result's toString(). On a compile or
// runtime error, returns an "ERR: <msg>" STRING rather than throwing.
// Dart_EvaluateExpr compiles a single expression; wrap multi-statement do-its as
// an immediately-invoked closure `(){ ... }()` on the Dart side.
void Workspace_eval(Dart_NativeArguments args) {
  Dart_Handle src = Dart_GetNativeArgument(args, 0);
  Dart_Handle lib = Dart_RootLibrary();

  Dart_Handle result = Dart_EvaluateExpr(lib, src);
  if (Dart_IsError(result)) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "ERR: %s", Dart_GetError(result));
    Dart_SetReturnValue(args, Dart_NewStringFromCString(buf));
    return;
  }
  Dart_Handle str = Dart_ToString(result);
  if (Dart_IsError(str)) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "ERR: toString: %s", Dart_GetError(str));
    Dart_SetReturnValue(args, Dart_NewStringFromCString(buf));
    return;
  }
  const char* c = NULL;
  Dart_StringToCString(str, &c);
  Dart_SetReturnValue(args, Dart_NewStringFromCString(c != NULL ? c : "null"));
}

// wsReload() -> "" | "ERR: ...". Hot-reload the isolate's sources (morphs live
// instances). Uses the S3 embedder primitive Dart_WorkspaceReloadSources.
void Workspace_reload(Dart_NativeArguments args) {
  Dart_Handle r = Dart_WorkspaceReloadSources(true /* force_reload */);
  if (Dart_IsError(r)) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "ERR: %s", Dart_GetError(r));
    Dart_SetReturnValue(args, Dart_NewStringFromCString(buf));
    return;
  }
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}

// wsVmStats() -> List<int>. This isolate's live VM counters (S3 primitive).
void Workspace_vmStats(Dart_NativeArguments args) {
  int64_t v[kDartWorkspaceVmStatCount];
  Dart_Handle err = Dart_WorkspaceVmStats(v, kDartWorkspaceVmStatCount);
  if (Dart_IsError(err)) { Dart_SetReturnValue(args, err); return; }
  Dart_Handle list = Dart_NewList(kDartWorkspaceVmStatCount);
  if (Dart_IsError(list)) { Dart_SetReturnValue(args, list); return; }
  for (intptr_t i = 0; i < kDartWorkspaceVmStatCount; i++) {
    Dart_ListSetAt(list, i, Dart_NewInteger(v[i]));
  }
  Dart_SetReturnValue(args, list);
}

// wsRequestUiReload()/wsUiReady()/wsUiReloadStatus() -> forward to the host.
void Workspace_requestUiReload(Dart_NativeArguments args) {
  windart_request_ui_reload();
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}
void Workspace_uiReady(Dart_NativeArguments args) {
  windart_ui_ready();
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}
void Workspace_uiReloadStatus(Dart_NativeArguments args) {
  const char* s = windart_take_ui_reload_status();
  Dart_SetReturnValue(args, Dart_NewStringFromCString(s != NULL ? s : ""));
}

}  // namespace bin
}  // namespace dart
