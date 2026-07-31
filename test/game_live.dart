// WINDART T3 — the LIVE on-screen game runner. Opens a real window containing a
// `game` widget (a DXGI swapchain bound to a child HWND by win_view's kGame), runs
// a gp* game (default 13_invaders) through the pull-pacer, and PRESENTS every frame
// so the game is VISIBLE and animates in the window — not just rendered offscreen.
//
// Self-plays with no keys held: the invader rank marches, bombs drop, and SFX fire
// on collisions (proving live animation + audio without input). When focused, the
// arrow keys / A,D drive the ship and space fires (GetAsyncKeyState via keyState).
//
// With the `selftest` arg it captures, after a warm-up:
//   * a baseline frame-1 offscreen PNG (start positions),
//   * the offscreen PNG at frame ~48 (aliens/ship MOVED -> proves animation), and
//   * the on-screen swapchain backbuffer PNG (gpSnapPresent -> what the window shows),
// then quits. Without `selftest` it stays open and interactive.
//
//   dartui.exe game_live.dart 13_invaders e:/windart/build/game_invaders_live.png selftest
import 'dart:win';
import 'dart:isolate';
import 'dart:async';

main(List<String> args) {
  var game = args.length > 0 ? args[0] : '13_invaders';
  var outPng = args.length > 1
      ? args[1]
      : 'e:/windart/build/game_${game}_live.png';
  var selftest = args.contains('selftest');
  const int W = 424, H = 240;                 // logical game size
  const int SCALE = 2;                        // on-screen scale (aspect preserved)
  final int VW = W * SCALE, VH = H * SCALE;   // 848 x 480

  var ui = new Ui.window('WINDART Game (live) - $game', VW + 40, VH + 92);
  keyWatch();
  ui.game('gp', frame: <int>[8, 8, VW, VH]);
  ui.commit();
  uiReady();

  var rp = new ReceivePort();
  SendPort ctl;
  var frames = 0;
  var opened = false;
  var snapped = false;
  var done = false;

  // Self-play: hold no keys. Untyped List<dynamic> tick payload sidesteps the
  // DEBUG cross-isolate type-args canonicalization assert (S6 finding).
  void tick() { if (ctl != null && !done) ctl.send(<dynamic>[<dynamic>[], 0]); }
  // Pace the next frame off the event loop (Present is vsync-capped; a small
  // Timer keeps the message pump responsive to window + key input).
  void schedule() { if (!done) new Timer(new Duration(milliseconds: 12), tick); }

  rp.listen((msg) {
    if (done || msg is! List || msg.isEmpty) return;
    if (msg[0] == 'port') { ctl = msg[1]; tick(); return; }
    if (msg[0] != 'draw') return;

    var cmds = msg[1];
    if (!opened) {
      if (cmds is List && cmds.isNotEmpty && cmds[0] is List &&
          cmds[0].isNotEmpty && cmds[0][0] == 'gpopen') {
        var o = cmds[0];
        gpOpen(o[1], o[2], o[3], o[4], 0);
        opened = true;
        schedule();                    // invite the first real frame
        return;
      }
      gpOpen(W, H, W, H, 0);
      opened = true;
    }

    var err = gpApply(cmds);           // apply + render_present + swapchain Present
    if (err != null && err is String && err.isNotEmpty && frames == 0) {
      print('GAME: apply note: $err');
    }
    frames++;

    if (selftest && frames == 1) {
      var e = gpSnap('e:/windart/build/${game}_frame1.png');
      print('GAME: baseline frame 1 ${e.isEmpty ? "OK" : "ERR:$e"}');
    }
    if (selftest && frames == 3) {
      // Deterministic audio smoke-test: synth a coin into a spare slot and play
      // it, so the SFX path is exercised even if no in-game collision fires in
      // the short self-test window. (The native logs GP_SFX define/play lines.)
      gpApply(<dynamic>[<dynamic>['gpsound', 63, 'coin'], <dynamic>['gpplay', 63]]);
      print('GAME: audio smoke-test fired (coin)');
    }
    if (selftest && !snapped && frames >= 48) {
      snapped = true;
      var e1 = gpSnap(outPng);                                        // offscreen (moved)
      var e2 = gpSnapPresent('e:/windart/build/${game}_present.png'); // on-screen letterbox
      print('GAME: LIVE snap $game frames=$frames -> $outPng '
          '${e1.isEmpty ? "OK" : "ERR:$e1"}');
      print('GAME: PRESENT snap -> ${game}_present.png '
          '${e2.isEmpty ? "OK" : "ERR:$e2"}');
      var st = gpStat();
      print('GAME: stat open=${st[0]} framesPresented=${st[1]} '
          'w=${st[2]} h=${st[3]}');
      done = true;
      rp.close();
      gpClose();
      hostQuit();
      return;
    }
    schedule();                        // pull-pace: invite the next frame
  });

  Isolate
      .spawnUri(Uri.parse('demos/$game.dart'), <String>['$W', '$H'], rp.sendPort)
      .catchError((e) {
        print('GAME: spawn error: $e');
        hostQuit();
      });
}
