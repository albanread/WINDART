// WINDART S5 — standalone demo runner: spawn one demo isolate, render N frames
// to a Direct2D canvas, snapshot the canvas to a PNG, and exit. Run under
// dartui.exe (the GUI host). Proves the whole path: demo isolate -> ['draw',cmds]
// -> UI isolate -> Win_canvasDraw (Direct2D) -> WIC PNG readback.
//
//   dartui.exe demo_runner.dart <demoBaseName> <outPng> <frames>
// e.g. dartui.exe demo_runner.dart 03_lissajous e:/windart/build/lissajous.png 40
import 'dart:win';
import 'dart:isolate';
import 'dart:async';

main(List<String> args) {
  var demo = args.length > 0 ? args[0] : '03_lissajous';
  var outPng = args.length > 1 ? args[1] : 'e:/windart/build/demo_$demo.png';
  var target = args.length > 2 ? int.parse(args[2]) : 40;
  const int W = 480, H = 360;

  var ui = new Ui.pane(W, H);
  ui.canvas('cv', frame: <int>[0, 0, W, H]);
  var err = ui.commit();
  uiReady();
  if (err.isNotEmpty) {
    print('RUNNER: canvas apply error: $err');
    hostQuit();
    return;
  }

  var rp = new ReceivePort();
  var frames = 0;
  var done = false;
  rp.listen((msg) {
    if (done) return;
    if (msg is List && msg.isNotEmpty && msg[0] == 'draw') {
      ui.canvasDraw('cv', msg[1]);
      frames++;
      if (frames >= target) {
        done = true;
        var e = ui.canvasSnap('cv', outPng);
        print('RUNNER: SNAP demo=$demo frames=$frames -> $outPng '
            '${e.isEmpty ? "OK" : "ERR:$e"}');
        rp.close();
        hostQuit();
      }
    }
    // other envelopes (['status',..] / ['done',..] / ['port',..]) are ignored
  });

  Isolate
      .spawnUri(Uri.parse('demos/$demo.dart'), <String>['$W', '$H'], rp.sendPort)
      .then((_) => print('RUNNER: spawned demos/$demo.dart (${W}x$H)'))
      .catchError((e) {
        print('RUNNER: spawn error: $e');
        hostQuit();
      });
}
