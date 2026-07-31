// WINDART S6 — pull-paced game-pane runner for the D3D11 games (12_copper,
// 13_invaders, 15_brickout). Unlike game_runner.dart (canvas games like Pong),
// these games speak the retained gp* protocol via GamePane: each ['draw', cmds]
// is a whole frame of gp verbs. The first frame carries ['gpopen', w,h,ww,wh],
// which we turn into gpOpen(); every later frame goes to gpApply(); after N
// frames we gpSnap() the offscreen to a PNG and quit. Self-drives headlessly
// (no keys held — the games render their attract/idle scene).
//
//   dartui.exe gp_runner.dart 12_copper e:/windart/build/game_copper.png 60
import 'dart:win';
import 'dart:isolate';

main(List<String> args) {
  var game = args.length > 0 ? args[0] : '12_copper';
  var outPng = args.length > 1 ? args[1] : 'e:/windart/build/game_$game.png';
  var target = args.length > 2 ? int.parse(args[2]) : 60;
  const int W = 424, H = 240;

  // A UI surface isn't required (the engine renders offscreen), but marking the
  // UI ready keeps a later isolate error survivable rather than fatal.
  uiReady();

  var rp = new ReceivePort();
  SendPort ctl;
  var frames = 0;
  var opened = false;
  var done = false;

  // Self-play: hold no keys. Untyped List<dynamic> tick payload to sidestep the
  // DEBUG-build cross-isolate type-args canonicalization assert (S6 finding).
  void tick() { if (ctl != null && !done) ctl.send([[], 0]); }

  rp.listen((msg) {
    if (done || msg is! List || msg.isEmpty) return;
    if (msg[0] == 'port') {
      ctl = msg[1];
      tick();                          // kick the first frame (carries gpopen)
    } else if (msg[0] == 'draw') {
      var cmds = msg[1];
      if (!opened) {
        // The GamePane ctor's first draw is [['gpopen', w, h, ww, wh]].
        if (cmds is List && cmds.isNotEmpty && cmds[0] is List &&
            cmds[0].isNotEmpty && cmds[0][0] == 'gpopen') {
          var o = cmds[0];
          gpOpen(o[1], o[2], o[3], o[4], 0);
          opened = true;
          tick();                      // invite the first real frame
          return;
        }
        // A game that opened differently: open with defaults, fall through.
        gpOpen(W, H, W, H, 0);
        opened = true;
      }
      var err = gpApply(cmds);
      if (err != null && err is String && err.isNotEmpty) {
        // Best-effort wire: log the first per-frame verb error once, keep going.
        if (frames == 0) print('GAME: apply note: $err');
      }
      frames++;
      if (frames >= target) {
        done = true;
        var e = gpSnap(outPng);
        print('GAME: SNAP $game frames=$frames -> $outPng '
            '${e.isEmpty ? "OK" : "ERR:$e"}');
        rp.close();
        gpClose();
        hostQuit();
      } else {
        tick();                        // pull-pace: invite the next frame
      }
    }
  });

  Isolate
      .spawnUri(Uri.parse('demos/$game.dart'), <String>['$W', '$H'], rp.sendPort)
      .catchError((e) {
        print('GAME: spawn error: $e');
        hostQuit();
      });
}
