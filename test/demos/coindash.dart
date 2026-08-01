// COIN DASH — a parallax auto-runner showing off the whole WINDART game pane:
//   * an fx SHADER sky (layer 0, a runtime HLSL gradient behind everything)
//   * TWO tile layers scrolling at different speeds (far starfield, near ground)
//   * SPRITES with per-sprite palettes (the runner, coins, spikes)
//   * real font-atlas TEXT for the HUD
//   * a deep-colour RGBA overlay for the hit-flash (per-pixel alpha)
//   * SFX presets (jump / coin / hurt)
// It self-plays (an AI hops the spikes and the world keeps rolling); live in the
// Game tab, SPACE / UP make the runner jump.
//   dartui.exe gp_runner.dart coindash e:/windart/build/coindash.png 200
library coindash;

import 'dart:isolate';
import 'dart:math';

import 'gamepane.dart';

const int VW = 424, VH = 240;
const int kGroundTop = 184;                 // screen y of the ground surface
const int kRunX = 66;                        // the runner's fixed screen x

// A dawn sky gradient as a runtime HLSL fragment (fx layer 0, behind everything).
const String kSky = '''
float4 fmain(GVOut input) : SV_Target {
  float t = saturate(input.uv.y * 1.12);
  float3 top = float3(0.04, 0.06, 0.20);
  float3 hor = float3(0.52, 0.42, 0.42);
  float3 col = lerp(top, hor, t);
  return float4(col, 1.0);
}
''';

// ---- procedural tile atlases (built once) ----------------------------------
List<int> _starTile(int seed) {                // far: sparse stars on transparent
  var t = new List<int>.filled(16 * 16, 0);
  var s = seed | 1;
  for (var k = 0; k < 3; k++) {
    s = (s * 1103515245 + 12345) & 0x3fffffff;
    var px = s % 16;
    s = (s * 1103515245 + 12345) & 0x3fffffff;
    var py = s % 16;
    t[py * 16 + px] = (k == 0) ? 16 : 17;
  }
  return t;
}

List<int> _groundTile(bool top) {              // near: grass cap over speckled dirt
  var t = new List<int>.filled(16 * 16, 0);
  for (var y = 0; y < 16; y++) {
    for (var x = 0; x < 16; x++) {
      var idx = 19;                            // dirt
      if (top && y < 4) idx = 18;              // grass cap
      if (top && y == 4 && (x % 3) == 0) idx = 18;   // ragged grass edge
      if (!top && ((x + y) % 7) == 0) idx = 20;      // dirt speckle
      t[y * 16 + x] = idx;
    }
  }
  return t;
}

main(List args, SendPort ui) {
  var W = args.length > 0 ? int.parse(args[0]) : VW;
  var H = args.length > 1 ? int.parse(args[1]) : VH;
  var gp = new GamePane(ui, W, H);
  var rng = new Random(1234);

  Sprite runnerDef, coinDef, spikeDef;
  SpriteRef runner;
  var coins = <SpriteRef>[], coinX = <double>[], coinLive = <bool>[];
  var spikes = <SpriteRef>[], spikeX = <double>[], spikeLive = <bool>[];
  Sound sJump, sCoin, sHurt;

  var first = true;
  var worldX = 0.0;
  var speed = 2.4;
  var py = kGroundTop.toDouble(), vy = 0.0;
  var onGround = true;
  var score = 0, dist = 0, flash = 0, frame = 0;

  gp.onFrame((g) {
    if (first) {
      first = false;
      g.shader(kSky);                          // fx layer 0: the gradient sky

      // shared global palette for the tiles
      g.pal(16, 255, 255, 235);  // bright star
      g.pal(17, 150, 160, 200);  // dim star
      g.pal(18, 90, 200, 90);    // grass
      g.pal(19, 120, 82, 54);    // dirt
      g.pal(20, 96, 64, 42);     // dirt speckle

      // far tile layer 0: starfield (4 variants), slow parallax
      var atlas0 = <int>[]..addAll(new List<int>.filled(256, 0));
      for (var k = 0; k < 4; k++) atlas0.addAll(_starTile(31 + k * 97));
      g.tileset(0, 16, 16, 5, atlas0);
      var map0 = <int>[];
      for (var r = 0; r < 15; r++) {
        for (var c = 0; c < 30; c++) {
          map0.add(((r * 5 + c * 3) % 4 == 0) ? (1 + ((r + c) % 4)) : 0);
        }
      }
      g.tilemap(0, 30, 15, map0);

      // near tile layer 1: the ground band, fast parallax
      var atlas1 = <int>[]
        ..addAll(new List<int>.filled(256, 0))
        ..addAll(_groundTile(true))
        ..addAll(_groundTile(false));
      g.tileset(1, 16, 16, 3, atlas1);
      var map1 = <int>[];
      var rows1 = 15, cols1 = 30, topRow = kGroundTop ~/ 16;
      for (var r = 0; r < rows1; r++) {
        for (var c = 0; c < cols1; c++) {
          map1.add(r < topRow ? 0 : (r == topRow ? 1 : 2));
        }
      }
      g.tilemap(1, cols1, rows1, map1);

      // sprites
      runnerDef = g.sprite('.1111./111111/113111/111111/.1111./.1..1.');
      runnerDef.rgb(1, 90, 200, 240);
      runnerDef.rgb(3, 20, 22, 34);
      runner = runnerDef.place(kRunX.toDouble(), py - 6);

      coinDef = g.sprite('.22./2232/2232/.22.');
      coinDef.rgb(2, 245, 205, 50);
      coinDef.rgb(3, 255, 255, 205);
      spikeDef = g.sprite('...44.../..4444../.444444./44444444');
      spikeDef.rgb(4, 220, 84, 70);

      for (var i = 0; i < 5; i++) {
        var cx = (W + 40 + i * 96).toDouble();
        coinX.add(cx);
        coinLive.add(true);
        coins.add(coinDef.place(cx, kGroundTop - 16.0));
      }
      for (var i = 0; i < 3; i++) {
        var sx = (W + 130 + i * 150).toDouble();
        spikeX.add(sx);
        spikeLive.add(true);
        spikes.add(spikeDef.place(sx, kGroundTop - 4.0));
      }

      sJump = g.sound('jump');
      sCoin = g.sound('coin');
      sHurt = g.sound('hurt');
    }

    frame++;
    worldX += speed;
    dist = worldX ~/ 8;

    // parallax scroll: far slow, near fast
    g.tileScroll(0, worldX ~/ 3, 0);
    g.tileScroll(1, worldX.toInt(), 0);

    // ---- runner physics + AI/keys ----
    var wantHop = g.key(Keys.space) || g.key(Keys.up);
    if (!wantHop) {
      for (var i = 0; i < spikes.length; i++) {
        if (!spikeLive[i]) continue;
        var sx = spikeX[i] - worldX;
        if (sx > kRunX + 6 && sx < kRunX + 40) { wantHop = true; break; }
      }
    }
    if (onGround && wantHop) { vy = -7.6; onGround = false; sJump.play(); }
    vy += 0.62;
    py += vy;
    if (py >= kGroundTop) { py = kGroundTop.toDouble(); vy = 0.0; onGround = true; }
    runner.y = py - 6;
    runner.update();

    // ---- coins: scroll, recycle, collect ----
    for (var i = 0; i < coins.length; i++) {
      var sx = coinX[i] - worldX;
      if (sx < -20) {
        coinX[i] += coins.length * 96.0 + rng.nextInt(48);
        coinLive[i] = true;
        sx = coinX[i] - worldX;
      }
      var cy = kGroundTop - 16.0 - (((coinX[i].toInt() ~/ 96) % 2) * 26);  // low / high
      if (coinLive[i] && sx > kRunX - 14 && sx < kRunX + 14 &&
          (runner.y - cy).abs() < 18) {
        coinLive[i] = false;
        score += 10;
        flash = 3;
        sCoin.play();
      }
      var c = coins[i];
      if (coinLive[i]) { c.x = sx; c.y = cy; c.update(); } else { c.hide(); }
    }

    // ---- spikes: scroll, recycle, hit ----
    for (var i = 0; i < spikes.length; i++) {
      var sx = spikeX[i] - worldX;
      if (sx < -24) {
        spikeX[i] += spikes.length * 150.0 + 60 + rng.nextInt(90);
        spikeLive[i] = true;
        sx = spikeX[i] - worldX;
      }
      if (spikeLive[i] && sx > kRunX - 12 && sx < kRunX + 12 &&
          py > kGroundTop - 12) {
        spikeLive[i] = false;
        flash = 9;
        score = score > 5 ? score - 5 : 0;
        sHurt.play();
      }
      spikes[i].x = sx;
      spikes[i].y = kGroundTop - 4.0;
      spikes[i].update();
    }

    // ---- RGBA hit/collect flash (transient; the surface is cleared between) ----
    if (flash > 0) {
      g.rgbaClear(0, 0, 0, 0);
      g.rgbaFill(0, 0, W, H, 255, 255, 255, (flash * 20).clamp(0, 210));
      flash--;
      if (flash == 0) g.rgbaClear(0, 0, 0, 0);
    }

    // ---- HUD ----
    g.textClear();
    g.text(8, 8, 'COIN DASH', 255, 255, 255);
    g.text(8, 22, 'COINS ' + score.toString(), 245, 215, 70);
    g.text(300, 8, 'DIST ' + dist.toString(), 150, 220, 255);
  });
}
