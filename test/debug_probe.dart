// WINDART T4 — standalone de-risking harness for the in-process debugger. Arms
// the debugger, spawns debug_target.dart in its own isolate, and polls the
// captured session (breakpoint -> pause -> stack -> frame-eval -> step -> resume
// -> complete). Proves the core loop headlessly before it is wired into the
// Debug tab.
//
//   dartui.exe debug_probe.dart
import 'dart:win';
import 'dart:isolate';
import 'dart:async';

main(List<String> args) {
  uiReady();
  const int breakLine = 15;          // `var acc = 1;` in debug_target.dart
  const String evalExpr = 'n * n';   // evaluate in the paused factorial frame -> 25

  wsDebugArm(breakLine, evalExpr);
  print('PROBE: armed (break line $breakLine, eval "$evalExpr")');

  var rp = new ReceivePort();
  rp.listen((msg) {
    print('PROBE: target message: $msg');
    wsDebugDone();
  });

  Isolate
      .spawnUri(Uri.parse('debug_target.dart'), <String>[], rp.sendPort)
      .catchError((e) { print('PROBE: spawn error: $e'); hostQuit(); });

  var ticks = 0;
  new Timer.periodic(new Duration(milliseconds: 100), (t) {
    ticks++;
    var st = wsDebugPoll();
    var done = st[0] == true;
    if (done || ticks > 60) {
      t.cancel();
      print('PROBE: ===== debug session transcript =====');
      print(st[1]);
      print('PROBE: curLine=${st[2]} hit=${st[5]} evalResult=${st[4]} done=${st[0]}');
      print('PROBE: done');
      rp.close();
      hostQuit();
    }
  });
}
