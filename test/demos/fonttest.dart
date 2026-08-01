// WINDART font test — draws the full printable ASCII set and sample HUD strings
// in several colours, proving the font-atlas text layer renders real glyphs.
// (Letters used to draw as hollow boxes; digits as 7-segment.) Headless:
//   dartui.exe gp_runner.dart fonttest e:/windart/build/font_test.png 6
library fonttest;

import 'dart:isolate';

import 'gamepane.dart';

main(List args, SendPort ui) {
  var W = args.length > 0 ? int.parse(args[0]) : 424;
  var H = args.length > 1 ? int.parse(args[1]) : 240;
  var gp = new GamePane(ui, W, H);
  gp.onFrame((g) {
    g.pal(16, 14, 16, 34);              // dark navy backdrop (global index 16)
    g.pal(17, 34, 40, 66);              // panel fill
    g.cls(16);
    g.fill(4, 4, W - 8, H - 8, 17);
    g.text(12, 8,   'WINDART  FONT  TEST', 255, 255, 255);
    g.text(12, 26,  'ABCDEFGHIJKLMNOPQRSTUVWXYZ', 120, 220, 255);
    g.text(12, 42,  'abcdefghijklmnopqrstuvwxyz', 130, 255, 160);
    g.text(12, 58,  '0123456789  Score: 128450', 255, 220, 120);
    g.text(12, 74,  r'''!"#$%&'()*+,-./:;<=>?@''', 255, 170, 170);
    g.text(12, 90,  r'''[\]^_`{|}~''', 255, 170, 210);
    g.text(12, 112, 'Hello, World!   GAME OVER', 255, 255, 255);
    g.text(12, 128, 'Level 7   Lives x3   1UP', 255, 120, 120);
    g.text(12, 144, 'The quick brown fox jumps', 180, 200, 255);
    g.text(12, 160, 'over the lazy dog.  0O 1lI', 200, 255, 200);
    g.text(12, 182, 'punctuation + digits + caps', 150, 230, 255);
  });
}
