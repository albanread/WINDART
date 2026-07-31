// WINDART T4 — the debug TARGET (debuggee). Runs in its own isolate under the
// workspace debugger. Self-contained (dart:core + dart:developer + dart:isolate):
// no mirrors, no source-loaded libs, so it debugs cleanly. `debugger()` is the
// attach handshake — at that first pause the host sets the real line breakpoint
// (on `factorial`) using the paused script URL, then resumes; the breakpoint is
// then hit inside factorial, where the host captures the stack, evaluates an
// expression in the frame, steps once, and resumes to completion.
//
// Line numbers matter (the host breaks on a specific line): keep `var acc = 1;`
// as the marked breakpoint line.
import 'dart:developer';
import 'dart:isolate';

int factorial(int n) {
  var acc = 1;                       // <-- BREAKPOINT line
  for (var i = 2; i <= n; i++) {
    acc = acc * i;                   // step-over lands on/after this
  }
  return acc;
}

main(List<String> args, SendPort ui) {
  debugger();                        // attach handshake -> host sets the breakpoint
  var n = 5;
  var r = factorial(n);              // hits the breakpoint inside factorial
  print('DBGTGT: factorial($n) = $r');
  ui.send('done:$r');                // String message (no typed-list cross-isolate assert)
}
