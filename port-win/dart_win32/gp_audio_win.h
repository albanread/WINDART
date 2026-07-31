// WINDART game pane — SFX playback (the Windows/XAudio2 port of GpSfx, the mac's
// AVAudioEngine path). Per gamepane-design/GP_AUDIO_DESIGN.md §1: one IXAudio2 +
// mastering voice per process, a pool of source voices for summed polyphony, and
// 64 fixed sample slots. The synthesizer itself (gp_synth.{h,cc}) is REUSED
// VERBATIM — it renders 44.1 kHz stereo INTERLEAVED f64, which XAudio2 wants
// interleaved (simpler than the mac's deinterleave): f64 -> f32, no channel split.
//
// Tunes (GpMusic / winmm midiStream) are DEFERRED this landing (see gp_natives_win
// gptune/gpmusic) — SFX is the "gpsound/gpplay produce actual sound" milestone.
#ifndef WINDART_GP_AUDIO_WIN_H_
#define WINDART_GP_AUDIO_WIN_H_

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xaudio2.h>

#include <vector>

#include "gp_synth.h"   // macdart_gamepane::Sound

namespace windart_gamepane {

const int kMaxSfxSlots = 64;
const int kVoicePool = 24;      // overlapping SFX land on distinct voices; master sums

class GpSfx {
 public:
  GpSfx();
  ~GpSfx();

  // Lazily create IXAudio2 + mastering voice + the source-voice pool. Returns
  // started_; a failed init (e.g. no audio device) degrades gracefully — define/
  // play become no-ops, the game plays on (matches the mac's graceful-degrade).
  bool start();
  bool started() const { return started_; }

  // Render-time: store the synth's Sound (f64 interleaved) as interleaved f32 PCM
  // in the slot. XAudio2 does not copy — the vector must outlive playback, so a
  // replaced buffer is RETIRED (kept alive) rather than freed while a voice may
  // still be reading it (GP_AUDIO_DESIGN.md §1.4 lifetime hazard).
  void define(int slot, const macdart_gamepane::Sound& sound);

  // Play the slot on the next idle pool voice (round-robin steal if all busy).
  // Returns true if a buffer was actually submitted (for the caller's log).
  bool play(int slot);

 private:
  IXAudio2* xaudio_;
  IXAudio2MasteringVoice* master_;
  IXAudio2SourceVoice* voices_[kVoicePool];
  int next_voice_;

  struct SfxSlot {
    std::vector<float> pcm;    // interleaved f32; the XAUDIO2_BUFFER points in here
    bool defined;
  };
  SfxSlot slots_[kMaxSfxSlots];
  std::vector<std::vector<float>> retired_;   // replaced buffers, freed when idle
  bool started_;
};

}  // namespace windart_gamepane

#endif  // WINDART_GP_AUDIO_WIN_H_
