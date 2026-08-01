// WINDART tile test — two INDEPENDENT indexed tile layers with parallax scroll.
// Layer 0 (far): a scrolling checkerboard backdrop. Layer 1 (near): yellow discs
// on transparent, scrolling FASTER, so the checkerboard shows between them (the
// discs' alignment vs the checkers differs by frame => parallax). Foreground: the
// free-draw indexed pane's ground strip + a block + HUD text, all on top.
//   dartui.exe gp_runner.dart tiletest e:/windart/build/tile_test.png 30
library tiletest;

import 'dart:isolate';

import 'gamepane.dart';

List<int> solid(int c) => new List<int>.filled(16 * 16, c);

List<int> disc(int c) {
  var t = new List<int>.filled(16 * 16, 0);
  for (var y = 0; y < 16; y++) {
    for (var x = 0; x < 16; x++) {
      var dx = x - 8, dy = y - 8;
      if (dx * dx + dy * dy <= 42) t[y * 16 + x] = c;   // r ~ 6.5
    }
  }
  return t;
}

main(List args, SendPort ui) {
  var W = args.length > 0 ? int.parse(args[0]) : 424;
  var H = args.length > 1 ? int.parse(args[1]) : 240;
  var gp = new GamePane(ui, W, H);
  var first = true;
  var frame = 0;
  gp.onFrame((g) {
    if (first) {
      first = false;
      // shared global palette (indices 16..255)
      g.pal(16, 28, 30, 78);      // navy   (checker A)
      g.pal(17, 58, 92, 200);     // blue   (checker B)
      g.pal(20, 245, 220, 70);    // yellow (near discs)
      g.pal(22, 54, 176, 84);     // green  (ground)
      g.pal(23, 235, 96, 96);     // red    (block)

      // layer 0 (far): checkerboard of navy(16)/blue(17). Tile 0 is the always-
      // empty cell; tiles 1 & 2 are the two solid colours.
      var atlas0 = <int>[]
        ..addAll(solid(0))        // tile 0 (empty, never drawn)
        ..addAll(solid(16))       // tile 1 navy
        ..addAll(solid(17));      // tile 2 blue
      g.tileset(0, 16, 16, 3, atlas0);
      var map0 = <int>[];
      for (var r = 0; r < 8; r++) {
        for (var c = 0; c < 16; c++) map0.add(((r + c) & 1) == 0 ? 1 : 2);
      }
      g.tilemap(0, 16, 8, map0);  // 256x128 world, wraps

      // layer 1 (near): sparse yellow discs on transparent.
      var atlas1 = <int>[]
        ..addAll(solid(0))        // tile 0 empty
        ..addAll(disc(20));       // tile 1 yellow disc
      g.tileset(1, 16, 16, 2, atlas1);
      var map1 = <int>[];
      for (var r = 0; r < 8; r++) {
        for (var c = 0; c < 16; c++) map1.add(((r * 3 + c) % 5) == 0 ? 1 : 0);
      }
      g.tilemap(1, 16, 8, map1);
    }
    frame++;
    // parallax: far layer creeps, near layer races
    g.tileScroll(0, frame * 1, 0);
    g.tileScroll(1, frame * 3, 0);

    // foreground on the free-draw pane (index 0 = transparent -> tiles show)
    g.cls(0);
    g.fill(0, H - 26, W, 26, 22);                 // green ground
    g.fill(W ~/ 2 - 11, H - 44, 22, 18, 23);      // a red block riding the ground
    g.text(10, 8, 'PARALLAX TILE LAYERS', 240, 240, 255);
    g.text(10, 22, 'far checkerboard + near discs', 150, 205, 255);
    g.text(10, H - 18, 'frame $frame', 200, 255, 200);
  });
}
