// WINDART RGBA-surface test — the deep-colour, true-alpha overlay layer.
// Base scene (indexed pane): four bold palette colour bars. Overlay (RGBA
// surface, composited above sprites, below text): a TRANSLUCENT rainbow gradient
// (the bars show through, tinted), an OPAQUE smooth gradient (proves >palette
// colour count), two translucent bars, and a soft edge vignette (per-pixel
// alpha). Built pixel-exact in Dart and blitted once; both layers retain.
//   dartui.exe gp_runner.dart rgbatest e:/windart/build/rgba_test.png 4
library rgbatest;

import 'dart:isolate';

import 'gamepane.dart';

main(List args, SendPort ui) {
  var W = args.length > 0 ? int.parse(args[0]) : 424;
  var H = args.length > 1 ? int.parse(args[1]) : 240;
  var gp = new GamePane(ui, W, H);
  var done = false;
  gp.onFrame((g) {
    if (done) return;
    done = true;

    // base scene (indexed pane): four bold colour bars behind the overlay
    g.pal(16, 220, 40, 40);   // red
    g.pal(17, 40, 200, 60);   // green
    g.pal(18, 50, 90, 230);   // blue
    g.pal(19, 235, 215, 40);  // yellow
    g.cls(0);
    var bw = (W / 4).ceil();
    for (var i = 0; i < 4; i++) g.fill(i * bw, 0, bw, H, 16 + i);

    // overlay (RGBA surface): built pixel-exact, blitted once, retained
    var img = new List<int>.filled(W * H * 4, 0);
    for (var y = 0; y < H; y++) {
      for (var x = 0; x < W; x++) {
        var i = (y * W + x) * 4;
        var t = x / (W - 1);
        int r = 0, gg = 0, b = 0, a = 0;
        if (y >= 24 && y < 88) {                    // translucent rainbow gradient
          r = (t * 255).toInt();
          gg = ((0.5 - (t - 0.5).abs()) * 2 * 255).toInt();
          b = ((1 - t) * 255).toInt();
          a = 120;                                  // ~47% -> bars tint through
        } else if (y >= 100 && y < 132) {           // OPAQUE deep-colour gradient
          r = (t * 255).toInt(); gg = 70; b = 255 - r; a = 255;
        } else if (y >= 150 && y < 176) {           // translucent white bar
          r = 255; gg = 255; b = 255; a = 96;
        } else if (y >= 186 && y < 212) {           // translucent orange bar
          r = 255; gg = 130; b = 0; a = 150;
        }
        // soft vignette: darken toward the edges (per-pixel alpha wins where stronger)
        var ex = ((x / (W - 1)) - 0.5).abs() * 2;
        var ey = ((y / (H - 1)) - 0.5).abs() * 2;
        var edge = ex > ey ? ex : ey;
        var va = (edge * edge * edge * 205).toInt();
        if (va > a) { r = 0; gg = 0; b = 0; a = va; }
        img[i] = r; img[i + 1] = gg; img[i + 2] = b; img[i + 3] = a;
      }
    }
    g.rgbaImage(img);

    g.text(10, 6, 'DEEP-COLOUR RGBA SURFACE + ALPHA', 255, 255, 255);
  });
}
