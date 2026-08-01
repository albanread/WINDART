// KEY ECHO — a diagnostic demo: each row lights green + reads DOWN while its key
// is held (g.key), proving live keyboard input reaches a gp game through the
// runner's per-frame keyState() payload.
//   dartui.exe game_live.dart keyecho e:/windart/build/keyecho.png selftest
library keyecho;

import 'dart:isolate';

import 'gamepane.dart';

main(List args, SendPort ui) {
  var W = args.length > 0 ? int.parse(args[0]) : 424;
  var H = args.length > 1 ? int.parse(args[1]) : 240;
  var gp = new GamePane(ui, W, H);
  gp.onFrame((g) {
    g.pal(16, 18, 22, 40);   // background
    g.pal(17, 60, 220, 90);  // lit indicator (green)
    g.pal(18, 50, 55, 70);   // dim indicator
    g.cls(16);
    g.text(16, 12, 'KEY ECHO   hold keys to light them', 255, 255, 255);
    var y = 44;
    void row(String label, int kc) {
      var on = g.key(kc);
      g.fill(16, y, 16, 12, on ? 17 : 18);
      g.text(44, y, label + '   ' + (on ? 'DOWN' : 'up'),
             on ? 120 : 130, on ? 255 : 140, on ? 140 : 150);
      y += 22;
    }
    row('SPACE', Keys.space);
    row('UP', Keys.up);
    row('LEFT', Keys.left);
    row('RIGHT', Keys.right);
    row('A', Keys.a);
    row('D', Keys.d);
    row('ESC', Keys.esc);
    row('F', Keys.f);
  });
}
