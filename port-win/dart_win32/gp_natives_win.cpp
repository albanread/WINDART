// WINDART game pane — the Dart boundary (port of gp_natives.mm). The wire is
// UNCHANGED from cocoa: a whole frame's command list crosses in one gpApply call,
// applied best-effort, presented once at the end; every argument validated here,
// a bad one costs an error string, never an abort. Only the engine beneath is
// D3D11 (gp_engine_d3d.*).
//
// Deferred this landing (S6b_NOTES.md): direct-framebuffer mode (§6b) and audio
// (gpsound/gpplay/gptune/gpmusic). The audio verbs are ACCEPTED and ignored so a
// game that uses them (invaders) reports no spurious "unknown verb".
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "include/dart_api.h"
#include "gp_engine_d3d.h"
#include "gp_audio_win.h"   // GpSfx (T3 SFX)
#include "gp_synth.h"       // the synth presets (macdart_gamepane)

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dart {
namespace bin {

using namespace windart_gamepane;

// --- small wire readers (== gp_natives.mm:23-91) ----------------------------
static int64_t ElInt(Dart_Handle list, intptr_t i) {
  Dart_Handle h = Dart_ListGetAt(list, i);
  if (Dart_IsInteger(h)) { int64_t v = 0; Dart_IntegerToInt64(h, &v); return v; }
  if (Dart_IsDouble(h)) { double d = 0; Dart_DoubleValue(h, &d); return (int64_t)d; }
  return 0;
}

static double ElDouble(Dart_Handle list, intptr_t i) {
  Dart_Handle h = Dart_ListGetAt(list, i);
  if (Dart_IsDouble(h)) { double d = 0; Dart_DoubleValue(h, &d); return d; }
  if (Dart_IsInteger(h)) { int64_t v = 0; Dart_IntegerToInt64(h, &v); return (double)v; }
  return 0.0;
}

static const char* ElStr(Dart_Handle list, intptr_t i) {
  Dart_Handle h = Dart_ListGetAt(list, i);
  if (!Dart_IsString(h)) return NULL;
  const char* c = NULL;
  Dart_StringToCString(h, &c);
  return c;
}

static uint8_t ClampByte(int64_t v) {
  return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static bool DecodeB64(const char* s, std::vector<uint8_t>* out) {
  static int8_t table[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; i++) table[i] = -1;
    const char* alpha =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) table[(uint8_t)alpha[i]] = (int8_t)i;
    init = true;
  }
  out->clear();
  uint32_t acc = 0; int bits = 0;
  for (const char* p = s; *p != '\0'; p++) {
    if (*p == '=' || *p == '\n' || *p == '\r') continue;
    int8_t v = table[(uint8_t)*p];
    if (v < 0) return false;
    acc = (acc << 6) | (uint32_t)v; bits += 6;
    if (bits >= 8) { bits -= 8; out->push_back((uint8_t)(acc >> bits)); }
  }
  return true;
}

// --- the 7 natives -----------------------------------------------------------

// _gpOpen(w, h, worldW, worldH, mode) -> handle (non-zero on success).
void Win_gpOpen(Dart_NativeArguments args) {
  int64_t w = 0, h = 0, ww = 0, wh = 0, mode = 0;
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 0), &w);
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 1), &h);
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 2), &ww);
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 3), &wh);
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 4), &mode);
  if (w < 32 || h < 32 || w > 2048 || h > 2048) {
    Dart_ThrowException(Dart_NewStringFromCString("gamepane: viewport must be 32..2048"));
    return;
  }
  if (ww > 8192 || wh > 8192) {
    Dart_ThrowException(Dart_NewStringFromCString("gamepane: world must be <= 8192"));
    return;
  }
  std::string err;
  int64_t handle = GpEngine::instance()->open((int)w, (int)h, (int)ww, (int)wh,
                                              mode != 0, &err);
  if (handle == 0) {
    Dart_ThrowException(Dart_NewStringFromCString(
        err.empty() ? "gamepane: open failed" : err.c_str()));
    return;
  }
  Dart_SetReturnValue(args, Dart_NewInteger(handle));
}

void Win_gpClose(Dart_NativeArguments args) {
  GpEngine::instance()->close();
  Dart_SetReturnValue(args, Dart_Null());
}

// _gpApply(cmds) -> null, or the FIRST error as a String (frame applied
// best-effort). Port of Cocoa_gpApply (gp_natives.mm:131-402); the direct-mode
// branch is dropped (deferred) and audio verbs are accepted no-ops.
void Win_gpApply(Dart_NativeArguments args) {
  GpEngine* eng = GpEngine::instance();
  if (!eng->is_open()) {
    Dart_SetReturnValue(args, Dart_NewStringFromCString("gamepane: not open"));
    return;
  }
  Dart_Handle cmds = Dart_GetNativeArgument(args, 0);
  intptr_t n = 0;
  if (!Dart_IsList(cmds) || Dart_IsError(Dart_ListLength(cmds, &n))) {
    Dart_SetReturnValue(args, Dart_NewStringFromCString("gamepane: apply needs a list"));
    return;
  }

  std::string first_err;
  eng->begin_frame();
  GpIndexedPane* pane = eng->pane();
  GpSprites* sprites = eng->sprites();

  for (intptr_t ci = 0; ci < n; ci++) {
    Dart_Handle c = Dart_ListGetAt(cmds, ci);
    intptr_t cn = 0;
    if (!Dart_IsList(c) || Dart_IsError(Dart_ListLength(c, &cn)) || cn < 1) continue;
    const char* op = ElStr(c, 0);
    if (op == NULL) continue;
    std::string verr;

    if (strcmp(op, "gppal") == 0 && cn >= 5) {
      int64_t i = ElInt(c, 1);
      if (i < 16 || i > 255) verr = "gppal: index must be 16..255";
      else pane->set_rgb((uint8_t)i, ClampByte(ElInt(c, 2)),
                         ClampByte(ElInt(c, 3)), ClampByte(ElInt(c, 4)));
    } else if (strcmp(op, "gplinepal") == 0 && cn >= 6) {
      int64_t line = ElInt(c, 1), i = ElInt(c, 2);
      if (i < 1 || i > 15) verr = "gplinepal: index must be 1..15";
      else if (line < 0 || line >= pane->viewport_h()) verr = "gplinepal: line out of range";
      else pane->set_line_rgb((int)line, (uint8_t)i, ClampByte(ElInt(c, 3)),
                              ClampByte(ElInt(c, 4)), ClampByte(ElInt(c, 5)));
    } else if (strcmp(op, "gpsprite") == 0 && cn >= 3) {
      int64_t want = ElInt(c, 1);
      const char* rows = ElStr(c, 2);
      int got = rows ? sprites->define(rows) : -1;
      if (got < 0) verr = "gpsprite: bad art rows";
      else if (got != want) verr = "gpsprite: id out of sequence";
    } else if (strcmp(op, "gpframe") == 0 && cn >= 3) {
      const char* rows = ElStr(c, 2);
      if (rows == NULL || !sprites->add_frame((int)ElInt(c, 1), rows))
        verr = "gpframe: bad def or mismatched art";
    } else if (strcmp(op, "gpspritepal") == 0 && cn >= 6) {
      sprites->set_rgb((int)ElInt(c, 1), (int)ElInt(c, 2), ClampByte(ElInt(c, 3)),
                       ClampByte(ElInt(c, 4)), ClampByte(ElInt(c, 5)));
    } else if (strcmp(op, "gpspawn") == 0 && cn >= 5) {
      int64_t want = ElInt(c, 1);
      int got = sprites->place((int)ElInt(c, 2), ElDouble(c, 3), ElDouble(c, 4));
      if (got < 0) verr = "gpspawn: bad def id";
      else if (got != want) verr = "gpspawn: id out of sequence";
    } else if (strcmp(op, "gpplace") == 0 && cn >= 8) {
      GpSpriteInstance* inst = sprites->instance((int)ElInt(c, 1));
      if (inst == NULL) verr = "gpplace: bad instance";
      else {
        inst->x = ElDouble(c, 2); inst->y = ElDouble(c, 3);
        inst->frame = (int)ElInt(c, 4);
        if (inst->frame < 0) inst->frame = 0;
        inst->scale = ElDouble(c, 5); inst->rot_deg = ElDouble(c, 6);
        double a = ElDouble(c, 7);
        inst->alpha = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
        inst->visible = true;
      }
    } else if (strcmp(op, "gphide") == 0 && cn >= 2) {
      GpSpriteInstance* inst = sprites->instance((int)ElInt(c, 1));
      if (inst != NULL) inst->visible = false;
    } else if (strcmp(op, "gpanim") == 0 && cn >= 3) {
      GpSpriteInstance* inst = sprites->instance((int)ElInt(c, 1));
      if (inst != NULL) inst->fps = ElDouble(c, 2);
    } else if (strcmp(op, "gpscroll") == 0 && cn >= 3) {
      pane->set_scroll(ElInt(c, 1), ElInt(c, 2));
    } else if (strcmp(op, "gpactive") == 0 && cn >= 2) {
      int64_t slot = ElInt(c, 1);
      if (slot < 0 || slot >= kNumBuffers) verr = "gpactive: slot 0..7";
      else pane->set_active((int)slot);
    } else if (strcmp(op, "gpswap") == 0) {
      pane->swap_buffers();
    } else if (strcmp(op, "gpcls") == 0 && cn >= 2) {
      pane->cls(ClampByte(ElInt(c, 1)));
    } else if (strcmp(op, "gppset") == 0 && cn >= 4) {
      pane->pset(ElInt(c, 1), ElInt(c, 2), ClampByte(ElInt(c, 3)));
    } else if (strcmp(op, "gpline") == 0 && cn >= 6) {
      pane->line(ElInt(c, 1), ElInt(c, 2), ElInt(c, 3), ElInt(c, 4),
                 ClampByte(ElInt(c, 5)));
    } else if (strcmp(op, "gpfill") == 0 && cn >= 6) {
      pane->fill_rect(ElInt(c, 1), ElInt(c, 2), ElInt(c, 3), ElInt(c, 4),
                      ClampByte(ElInt(c, 5)));
    } else if (strcmp(op, "gpcircle") == 0 && cn >= 5) {
      pane->circle(ElInt(c, 1), ElInt(c, 2), ElInt(c, 3), ClampByte(ElInt(c, 4)));
    } else if (strcmp(op, "gpdisc") == 0 && cn >= 5) {
      pane->disc(ElInt(c, 1), ElInt(c, 2), ElInt(c, 3), ClampByte(ElInt(c, 4)));
    } else if (strcmp(op, "gpload") == 0 && cn >= 3) {
      int64_t slot = ElInt(c, 1);
      const char* b64 = ElStr(c, 2);
      std::vector<uint8_t> bytes;
      if (slot < 0 || slot >= kNumBuffers) verr = "gpload: slot 0..7";
      else if (b64 == NULL || !DecodeB64(b64, &bytes)) verr = "gpload: bad base64";
      else {
        int prev = pane->active();
        pane->set_active((int)slot);
        pane->load(bytes.data(), bytes.size());
        pane->set_active(prev);
      }
    } else if (strcmp(op, "gpblit") == 0 && cn >= 10) {
      int64_t mode = ElInt(c, 1);
      int64_t src = ElInt(c, 2), dst = ElInt(c, 3);
      if (mode < 0 || mode > 5) verr = "gpblit: mode 0..5";
      else if (src < 0 || src >= kNumBuffers || dst < 0 || dst >= kNumBuffers)
        verr = "gpblit: slot 0..7";
      else eng->blitter()->blit(pane, (int)mode, (int)src, (int)dst,
                                ElInt(c, 4), ElInt(c, 5), ElInt(c, 6), ElInt(c, 7),
                                ElInt(c, 8), ElInt(c, 9),
                                cn > 10 ? ClampByte(ElInt(c, 10)) : 0);
    } else if (strcmp(op, "gptileset") == 0 && cn >= 6) {
      int64_t layer = ElInt(c, 1), tw = ElInt(c, 2), th = ElInt(c, 3),
              count = ElInt(c, 4);
      const char* b64 = ElStr(c, 5);
      std::vector<uint8_t> bytes;
      GpTileLayer* L = eng->bg((int)layer);
      if (L == NULL) verr = "gptileset: layer 0..1";
      else if (tw < 1 || th < 1 || count < 1 || tw > 256 || th > 256 || count > 1024)
        verr = "gptileset: bad tile dims";
      else if (b64 == NULL || !DecodeB64(b64, &bytes)) verr = "gptileset: bad base64";
      else if (bytes.size() < (size_t)(tw * th * count))
        verr = "gptileset: atlas smaller than tw*th*count";
      else L->set_tileset(bytes.data(), (int)tw, (int)th, (int)count);
    } else if (strcmp(op, "gptilemap") == 0 && cn >= 5) {
      int64_t layer = ElInt(c, 1), cols = ElInt(c, 2), rows = ElInt(c, 3);
      const char* b64 = ElStr(c, 4);
      std::vector<uint8_t> bytes;
      GpTileLayer* L = eng->bg((int)layer);
      if (L == NULL) verr = "gptilemap: layer 0..1";
      else if (cols < 1 || rows < 1 || cols > 4096 || rows > 4096)
        verr = "gptilemap: bad map dims";
      else if (b64 == NULL || !DecodeB64(b64, &bytes)) verr = "gptilemap: bad base64";
      else if (bytes.size() < (size_t)(cols * rows))
        verr = "gptilemap: map smaller than cols*rows";
      else L->set_map(bytes.data(), (int)cols, (int)rows);
    } else if (strcmp(op, "gptilescroll") == 0 && cn >= 4) {
      GpTileLayer* L = eng->bg((int)ElInt(c, 1));
      if (L == NULL) verr = "gptilescroll: layer 0..1";
      else L->set_scroll((double)ElInt(c, 2), (double)ElInt(c, 3));
    } else if (strcmp(op, "gptileclear") == 0 && cn >= 2) {
      GpTileLayer* L = eng->bg((int)ElInt(c, 1));
      if (L != NULL) L->clear();
    } else if (strcmp(op, "gprgbaclear") == 0 && cn >= 5) {
      eng->rgba()->clear(ClampByte(ElInt(c, 1)), ClampByte(ElInt(c, 2)),
                         ClampByte(ElInt(c, 3)), ClampByte(ElInt(c, 4)));
    } else if (strcmp(op, "gprgbapset") == 0 && cn >= 7) {
      eng->rgba()->pset(ElInt(c, 1), ElInt(c, 2), ClampByte(ElInt(c, 3)),
                        ClampByte(ElInt(c, 4)), ClampByte(ElInt(c, 5)),
                        ClampByte(ElInt(c, 6)));
    } else if (strcmp(op, "gprgbafill") == 0 && cn >= 9) {
      eng->rgba()->fill_rect(ElInt(c, 1), ElInt(c, 2), ElInt(c, 3), ElInt(c, 4),
                             ClampByte(ElInt(c, 5)), ClampByte(ElInt(c, 6)),
                             ClampByte(ElInt(c, 7)), ClampByte(ElInt(c, 8)));
    } else if (strcmp(op, "gprgbaline") == 0 && cn >= 9) {
      eng->rgba()->line(ElInt(c, 1), ElInt(c, 2), ElInt(c, 3), ElInt(c, 4),
                        ClampByte(ElInt(c, 5)), ClampByte(ElInt(c, 6)),
                        ClampByte(ElInt(c, 7)), ClampByte(ElInt(c, 8)));
    } else if (strcmp(op, "gprgbaload") == 0 && cn >= 2) {
      const char* b64 = ElStr(c, 1);
      std::vector<uint8_t> bytes;
      if (b64 == NULL || !DecodeB64(b64, &bytes)) verr = "gprgbaload: bad base64";
      else eng->rgba()->load(bytes.data(), bytes.size());
    } else if (strcmp(op, "gptextclear") == 0) {
      eng->text()->clear();
    } else if (strcmp(op, "gptext") == 0 && cn >= 7) {
      const char* s = ElStr(c, 3);
      if (s != NULL) eng->text()->draw_text(ElInt(c, 1), ElInt(c, 2), s,
          ClampByte(ElInt(c, 4)), ClampByte(ElInt(c, 5)), ClampByte(ElInt(c, 6)));
    } else if (strcmp(op, "gpshader") == 0 && cn >= 2) {
      const char* body = ElStr(c, 1);
      if (body == NULL) verr = "gpshader: body must be a string";
      else verr = eng->shader()->compile(body);
    } else if (strcmp(op, "gpparam") == 0 && cn >= 3) {
      eng->shader()->set_param((int)ElInt(c, 1), (float)ElDouble(c, 2));
    } else if (strcmp(op, "gpsound") == 0 && cn >= 3) {
      // ['gpsound', slot, preset, a1?, a2?, wf?] — synth a preset into a slot.
      using namespace macdart_gamepane;
      int64_t slot = ElInt(c, 1);
      const char* preset = ElStr(c, 2);
      if (slot < 0 || slot >= kMaxSfxSlots) verr = "gpsound: slot 0..63";
      else if (preset == NULL) verr = "gpsound: preset name needed";
      else {
        static Lcg rng(12345);            // deterministic across a session
        double a1 = cn > 3 ? ElDouble(c, 3) : 0.0;
        double a2 = cn > 4 ? ElDouble(c, 4) : 0.0;
        Sound snd;
        if (strcmp(preset, "beep") == 0)         snd = preset_beep(a1 > 0 ? a1 : 440.0, a2 > 0 ? a2 : 0.15);
        else if (strcmp(preset, "coin") == 0)    snd = preset_coin(a1 > 0 ? a1 : 0.3);
        else if (strcmp(preset, "jump") == 0)    snd = preset_jump(a1 > 0 ? a1 : 0.2);
        else if (strcmp(preset, "zap") == 0)     snd = preset_zap(a1 > 0 ? a1 : 0.2, rng);
        else if (strcmp(preset, "shoot") == 0)   snd = preset_shoot(a1 > 0 ? a1 : 0.15, rng);
        else if (strcmp(preset, "explode") == 0) snd = preset_explode(a1 > 0 ? a1 : 1.0, a2 > 0 ? a2 : 0.5, rng);
        else if (strcmp(preset, "powerup") == 0) snd = preset_powerup(a1 > 0 ? a1 : 0.4);
        else if (strcmp(preset, "hurt") == 0)    snd = preset_hurt(a1 > 0 ? a1 : 0.25, rng);
        else if (strcmp(preset, "click") == 0)   snd = preset_click(a1 > 0 ? a1 : 0.05, rng);
        else if (strcmp(preset, "bang") == 0)    snd = preset_bang(a1 > 0 ? a1 : 0.3, rng);
        else if (strcmp(preset, "blip") == 0)    snd = preset_blip(a1 > 0 ? a1 : 1.0, a2 > 0 ? a2 : 0.08);
        else if (strcmp(preset, "tone") == 0) {
          int64_t wf = cn > 5 ? ElInt(c, 5) : 0; if (wf < 0 || wf > 5) wf = 0;
          snd = preset_tone(a1 > 0 ? a1 : 440.0, a2 > 0 ? a2 : 0.2, (Waveform)wf);
        } else if (strcmp(preset, "noise") == 0) snd = noise_burst((int)a1, a2 > 0 ? a2 : 0.3, rng);
        else verr = std::string("gpsound: unknown preset ") + preset;
        if (verr.empty()) {
          eng->sfx()->define((int)slot, snd);
          fprintf(stdout, "GP_SFX: define slot=%d preset=%s (audio %s)\n",
                  (int)slot, preset, eng->sfx()->started() ? "on" : "unavailable");
          fflush(stdout);
        }
      }
    } else if (strcmp(op, "gpplay") == 0 && cn >= 2) {
      int64_t slot = ElInt(c, 1);
      bool fired = eng->sfx()->play((int)slot);
      fprintf(stdout, "GP_SFX: play slot=%d -> %s\n", (int)slot,
              fired ? "submitted" : "no-op");
      fflush(stdout);
    } else if (strcmp(op, "gptune") == 0 || strcmp(op, "gpmusic") == 0) {
      // Tunes (GpMusic / winmm midiStream) deferred this landing (see T3_NOTES);
      // accepted and ignored so a game that scores a theme reports no error.
    } else if (strcmp(op, "gpfull") == 0 && cn >= 2) {
      eng->set_fullscreen(ElInt(c, 1) != 0);
    } else if (strcmp(op, "gpopen") == 0) {
      // Consumed by the runner before apply; harmless here.
    } else {
      verr = std::string("gamepane: unknown verb ") + op;
    }

    if (!verr.empty() && first_err.empty()) first_err = verr;
  }

  eng->render_present();

  if (first_err.empty()) Dart_SetReturnValue(args, Dart_Null());
  else Dart_SetReturnValue(args, Dart_NewStringFromCString(first_err.c_str()));
}

// _gpSnap(path) -> "" on success, else the error.
void Win_gpSnap(Dart_NativeArguments args) {
  const char* path = NULL;
  Dart_StringToCString(Dart_GetNativeArgument(args, 0), &path);
  std::string err;
  if (path == NULL || !GpEngine::instance()->snap(path, &err)) {
    Dart_SetReturnValue(args, Dart_NewStringFromCString(
        err.empty() ? "gamepane: snap failed" : err.c_str()));
    return;
  }
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}

// _gpSnapPresent(path) -> "" on success, else the error. Reads back the live
// swapchain backbuffer (the letterboxed ON-SCREEN image), distinct from gpSnap's
// raw offscreen. Fails cleanly if no game widget/window is attached (T3).
void Win_gpSnapPresent(Dart_NativeArguments args) {
  const char* path = NULL;
  Dart_StringToCString(Dart_GetNativeArgument(args, 0), &path);
  std::string err;
  if (path == NULL || !GpEngine::instance()->snap_present(path, &err)) {
    Dart_SetReturnValue(args, Dart_NewStringFromCString(
        err.empty() ? "gamepane: present snap failed" : err.c_str()));
    return;
  }
  Dart_SetReturnValue(args, Dart_NewStringFromCString(""));
}

// _gpStat() -> [open, framesPresented, logicalW, logicalH, fullscreen, direct, stride]
void Win_gpStat(Dart_NativeArguments args) {
  GpEngine* eng = GpEngine::instance();
  Dart_Handle l = Dart_NewList(7);
  Dart_ListSetAt(l, 0, Dart_NewInteger(eng->is_open() ? 1 : 0));
  Dart_ListSetAt(l, 1, Dart_NewInteger(eng->frames_presented()));
  Dart_ListSetAt(l, 2, Dart_NewInteger(eng->logical_w()));
  Dart_ListSetAt(l, 3, Dart_NewInteger(eng->logical_h()));
  Dart_ListSetAt(l, 4, Dart_NewInteger(eng->fullscreen() ? 1 : 0));
  Dart_ListSetAt(l, 5, Dart_NewInteger(eng->is_direct() ? 1 : 0));
  Dart_ListSetAt(l, 6, Dart_NewInteger(eng->direct_stride()));
  Dart_SetReturnValue(args, l);
}

// _gpFullscreen(on).
void Win_gpFullscreen(Dart_NativeArguments args) {
  int64_t on = 0;
  Dart_IntegerToInt64(Dart_GetNativeArgument(args, 0), &on);
  GpEngine::instance()->set_fullscreen(on != 0);
  Dart_SetReturnValue(args, Dart_Null());
}

// _gpBackbuffer() -> null (direct framebuffer mode deferred, S6b).
void Win_gpBackbuffer(Dart_NativeArguments args) {
  Dart_SetReturnValue(args, Dart_Null());
}

}  // namespace bin
}  // namespace dart
