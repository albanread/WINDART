// WINDART S6 — pull-paced game runner (for control-port games like Pong, which
// draw one frame per UI invitation and read [downKeys, mods] off each tick).
// Renders to the S5 Direct2D canvas; self-drives headlessly (injects a serve,
// then plays with no keys); snapshots to PNG.
//
//   dartui.exe game_runner.dart 11_pong e:/windart/build/game_pong.png 60
import 'dart:win';
import 'dart:isolate';
import 'dart:async';

main(List<String> args) {
  var game = args.length > 0 ? args[0] : '11_pong';
  var outPng = args.length > 1 ? args[1] : 'e:/windart/build/game_$game.png';
  var target = args.length > 2 ? int.parse(args[2]) : 60;
  const int W = 480, H = 360;

  var ui = new Ui.pane(W, H);
  ui.canvas('cv', frame: <int>[0, 0, W, H]);
  ui.commit();
  uiReady();

  var rp = new ReceivePort();
  SendPort ctl;
  var frames = 0;
  var done = false;

  void tick() {
    if (ctl == null || done) return;
    // Self-play: press space (49) around frames 3-6 to serve the ball, then
    // play with no keys held — the ball flies and bounces on its own. Untyped
    // lists (List<dynamic>) to sidestep a DEBUG-build cross-isolate type-args
    // canonicalization assert on List<int>.
    var keys = (frames >= 3 && frames < 7) ? [49] : [];
    ctl.send([keys, 0]);            // the tick payload = [downKeycodes, mods]
  }

  rp.listen((msg) {
    if (done || msg is! List || msg.isEmpty) return;
    if (msg[0] == 'port') {
      ctl = msg[1];
      tick();                        // kick the first frame
    } else if (msg[0] == 'draw') {
      ui.canvasDraw('cv', msg[1]);
      frames++;
      if (frames >= target) {
        done = true;
        var e = ui.snapshot(outPng);   // unified Win_surfaceSnapshot (S6 §4)
        print('GAME: SNAP $game frames=$frames -> $outPng '
            '${e.isEmpty ? "OK" : "ERR:$e"}');
        rp.close();
        hostQuit();
      } else {
        tick();                      // pull-pace: invite the next frame
      }
    }
    // ['status',..] / ['done',..] ignored
  });

  Isolate
      .spawnUri(Uri.parse('demos/$game.dart'), <String>['$W', '$H'], rp.sendPort)
      .catchError((e) {
        print('GAME: spawn error: $e');
        hostQuit();
      });
}
