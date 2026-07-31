// WINDART T4 — the workspace debugger natives (the Debug tab's engine).
//
// Uses the classic in-process embedder debugger API (dart_tools_api.h), which is
// compiled + functional in this non-PRODUCT VM (debugger_api_impl.cc:
// Dart_SetPausedEventHandler installs a GLOBAL event bridge, so one handler covers
// every isolate — including a spawned debuggee). The debuggee runs in its OWN
// isolate on a VM pool thread (via Isolate.spawnUri from the Debug tab), so the UI
// pump thread stays responsive while it is paused.
//
// Model (scripted core loop — the T4 milestone): the target calls `debugger()` as
// an attach handshake; at that pause the handler sets the real LINE breakpoint on
// the target script (the paused location gives the URL). The breakpoint is then
// hit; the handler captures the call stack + top-frame locals, EVALUATES an
// expression in the paused frame (Dart_ActivationFrameEvaluate), steps once
// (Dart_SetStepOver), records the new line, and resumes to completion. Everything
// is logged to a mutex-guarded transcript the UI polls (Workspace_debugPoll) and
// also echoed to stdout (DBG:) for the headless proof.
#include <cstdio>
#include <mutex>
#include <string>

#include "include/dart_api.h"
#include "include/dart_tools_api.h"

namespace dart {
namespace bin {

static std::mutex  g_dbg_mu;
static int         g_dbg_break_line = 0;
static int         g_dbg_step_kind = 0;   // 0 over, 1 into, 2 out, 3 resume (no step)
static std::string g_dbg_eval_expr;
static bool        g_dbg_bp_set = false;     // has the attach handshake set the bp?
static bool        g_dbg_hit = false;        // has the breakpoint been hit + captured?
static std::string g_dbg_log;
static std::string g_dbg_stack;
static int         g_dbg_cur_line = -1;
static std::string g_dbg_cur_fn;
static std::string g_dbg_eval_result;
static bool        g_dbg_done = false;

// --- small helpers (run inside the paused handler's scope) -------------------
static std::string HStr(Dart_Handle h) {
  if (h == NULL || !Dart_IsString(h)) return "";
  const char* c = NULL;
  if (Dart_IsError(Dart_StringToCString(h, &c)) || c == NULL) return "";
  return std::string(c);
}

static void LogLine(const std::string& s) {   // caller holds g_dbg_mu
  g_dbg_log += s; g_dbg_log += "\n";
  fprintf(stdout, "DBG: %s\n", s.c_str()); fflush(stdout);
}

// Build "#i qualifiedFn (url:line)\n…"; records the top frame's fn + line.
static std::string CaptureStack() {
  Dart_StackTrace trace;
  if (Dart_IsError(Dart_GetStackTrace(&trace))) return "";
  intptr_t len = 0;
  if (Dart_IsError(Dart_StackTraceLength(trace, &len))) return "";
  std::string out;
  for (intptr_t i = 0; i < len && i < 32; i++) {
    Dart_ActivationFrame frame;
    if (Dart_IsError(Dart_GetActivationFrame(trace, (int)i, &frame))) break;
    Dart_Handle fn = NULL, url = NULL; intptr_t line = -1, col = -1;
    Dart_ActivationFrameInfo(frame, &fn, &url, &line, &col);
    char buf[600];
    snprintf(buf, sizeof(buf), "#%d %s (%s:%d)\n", (int)i, HStr(fn).c_str(),
             HStr(url).c_str(), (int)line);
    out += buf;
    if (i == 0) { g_dbg_cur_line = (int)line; g_dbg_cur_fn = HStr(fn); }
  }
  return out;
}

static std::string EvalInTopFrame(const std::string& expr) {
  if (expr.empty()) return "";
  Dart_StackTrace trace;
  if (Dart_IsError(Dart_GetStackTrace(&trace))) return "(no stack)";
  Dart_ActivationFrame frame;
  if (Dart_IsError(Dart_GetActivationFrame(trace, 0, &frame))) return "(no frame)";
  Dart_Handle r = Dart_ActivationFrameEvaluate(
      frame, Dart_NewStringFromCString(expr.c_str()));
  if (Dart_IsError(r)) { std::string e = "ERR: "; e += Dart_GetError(r); return e; }
  return HStr(Dart_ToString(r));
}

static std::string LocalsInTopFrame() {
  Dart_StackTrace trace;
  if (Dart_IsError(Dart_GetStackTrace(&trace))) return "";
  Dart_ActivationFrame frame;
  if (Dart_IsError(Dart_GetActivationFrame(trace, 0, &frame))) return "";
  Dart_Handle vars = Dart_GetLocalVariables(frame);
  if (vars == NULL || Dart_IsError(vars) || !Dart_IsList(vars)) return "";
  intptr_t n = 0;
  if (Dart_IsError(Dart_ListLength(vars, &n))) return "";
  std::string out;
  for (intptr_t i = 0; i + 1 < n; i += 2) {
    std::string name = HStr(Dart_ListGetAt(vars, i));
    std::string val = HStr(Dart_ToString(Dart_ListGetAt(vars, i + 1)));
    if (!name.empty() && name[0] != ':') out += name + "=" + val + "  ";
  }
  return out;
}

// The global paused handler. bp_id != 0 => a real breakpoint; bp_id == 0 => a
// debugger()/step pause. Runs on the debuggee's thread with the isolate paused;
// DebuggerEventHandler already entered a scope for us.
static void DbgPausedHandler(Dart_IsolateId /*iso*/, intptr_t bp_id,
                             const Dart_CodeLocation& location) {
  std::lock_guard<std::mutex> lk(g_dbg_mu);

  if (bp_id == 0 && !g_dbg_bp_set) {
    // Attach handshake (the debugger() call): the target script is now loaded;
    // set the real LINE breakpoint using the paused location's URL.
    std::string url = HStr(location.script_url);
    Dart_Handle bp = Dart_SetBreakpoint(
        Dart_NewStringFromCString(url.c_str()), g_dbg_break_line);
    if (Dart_IsError(bp)) {
      LogLine(std::string("breakpoint set FAILED: ") + Dart_GetError(bp));
    } else {
      int64_t id = 0; Dart_IntegerToInt64(bp, &id);
      char buf[700];
      snprintf(buf, sizeof(buf),
               "attached (debugger handshake); breakpoint set at %s:%d -> id %lld",
               url.c_str(), g_dbg_break_line, (long long)id);
      LogLine(buf);
    }
    g_dbg_bp_set = true;
    return;   // resume; run on to the breakpoint
  }

  if (bp_id != 0 && !g_dbg_hit) {
    // The breakpoint was hit — capture, evaluate, and step once.
    g_dbg_hit = true;
    g_dbg_stack = CaptureStack();
    char buf[256];
    snprintf(buf, sizeof(buf), "PAUSED at breakpoint: %s line %d",
             g_dbg_cur_fn.c_str(), g_dbg_cur_line);
    LogLine(buf);
    LogLine("call stack:\n" + g_dbg_stack);
    std::string locals = LocalsInTopFrame();
    if (!locals.empty()) LogLine("frame locals: " + locals);
    g_dbg_eval_result = EvalInTopFrame(g_dbg_eval_expr);
    LogLine("frame-eval  " + g_dbg_eval_expr + "  =>  " + g_dbg_eval_result);
    switch (g_dbg_step_kind) {
      case 1: LogLine("step into ->"); Dart_SetStepInto(); break;
      case 2: LogLine("step out ->");  Dart_SetStepOut();  break;
      case 3: LogLine("resume ->");    /* no step: run to completion */ break;
      default: LogLine("step over ->"); Dart_SetStepOver(); break;
    }
    return;
  }

  // A step landing (bp_id == 0, breakpoint already hit): record the new line.
  CaptureStack();   // refreshes g_dbg_cur_line / g_dbg_cur_fn
  char buf[128];
  snprintf(buf, sizeof(buf), "stepped to %s line %d; resuming to completion",
           g_dbg_cur_fn.c_str(), g_dbg_cur_line);
  LogLine(buf);
  // resume (no step set) -> run to completion
}

// --- the natives -------------------------------------------------------------

// Workspace_debugArm(int breakLine, String evalExpr) -> "" : reset state, install
// the global paused handler. Call BEFORE spawning the target isolate.
void Workspace_debugArm(Dart_NativeArguments args) {
  int64_t line = 0, stepKind = 0;
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 0), &line);
  const char* expr = NULL;
  Dart_StringToCString(Dart_GetNativeArgument(args, 1), &expr);
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 2), &stepKind);
  {
    std::lock_guard<std::mutex> lk(g_dbg_mu);
    g_dbg_break_line = (int)line;
    g_dbg_step_kind = (int)stepKind;
    g_dbg_eval_expr = expr ? expr : "";
    g_dbg_bp_set = false; g_dbg_hit = false; g_dbg_done = false;
    g_dbg_log.clear(); g_dbg_stack.clear();
    g_dbg_cur_line = -1; g_dbg_cur_fn.clear(); g_dbg_eval_result.clear();
  }
  Dart_SetPausedEventHandler(DbgPausedHandler);
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}

// Workspace_debugDone() -> null : the Debug tab calls this when the target's
// isolate signals completion (its onExit/'done' message), so poll reports done.
void Workspace_debugDone(Dart_NativeArguments args) {
  std::lock_guard<std::mutex> lk(g_dbg_mu);
  if (!g_dbg_done) { LogLine("target isolate finished — debug session complete"); g_dbg_done = true; }
  Dart_SetReturnValue(args, Dart_Null());
}

// Workspace_debugPoll() -> [done(bool), log(String), curLine(int), stack(String),
//                           evalResult(String), hit(bool)]
void Workspace_debugPoll(Dart_NativeArguments args) {
  std::lock_guard<std::mutex> lk(g_dbg_mu);
  Dart_Handle l = Dart_NewList(6);
  Dart_ListSetAt(l, 0, Dart_NewBoolean(g_dbg_done));
  Dart_ListSetAt(l, 1, Dart_NewStringFromCString(g_dbg_log.c_str()));
  Dart_ListSetAt(l, 2, Dart_NewInteger(g_dbg_cur_line));
  Dart_ListSetAt(l, 3, Dart_NewStringFromCString(g_dbg_stack.c_str()));
  Dart_ListSetAt(l, 4, Dart_NewStringFromCString(g_dbg_eval_result.c_str()));
  Dart_ListSetAt(l, 5, Dart_NewBoolean(g_dbg_hit));
  Dart_SetReturnValue(args, l);
}

}  // namespace bin
}  // namespace dart
