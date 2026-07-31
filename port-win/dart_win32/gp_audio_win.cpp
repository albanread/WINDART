// WINDART game pane — SFX playback via XAudio2 (port of GpSfx). See gp_audio_win.h
// and GP_AUDIO_DESIGN.md §1. All calls run on the UI (pump) thread.
#define WIN32_LEAN_AND_MEAN
#include "gp_audio_win.h"

#include <objbase.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace windart_gamepane {

using macdart_gamepane::Sound;

// The shared stream format: 44.1 kHz stereo 32-bit float, interleaved.
static const WAVEFORMATEX& SharedFormat() {
  static WAVEFORMATEX f = [] {
    WAVEFORMATEX w = {};
    w.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    w.nChannels = 2;
    w.nSamplesPerSec = 44100;
    w.wBitsPerSample = 32;
    w.nBlockAlign = (WORD)((w.nChannels * w.wBitsPerSample) / 8);   // 8
    w.nAvgBytesPerSec = w.nSamplesPerSec * w.nBlockAlign;
    w.cbSize = 0;
    return w;
  }();
  return f;
}

GpSfx::GpSfx()
    : xaudio_(nullptr), master_(nullptr), next_voice_(0), started_(false) {
  for (int i = 0; i < kVoicePool; i++) voices_[i] = nullptr;
  for (int i = 0; i < kMaxSfxSlots; i++) slots_[i].defined = false;
}

GpSfx::~GpSfx() {
  for (int i = 0; i < kVoicePool; i++) {
    if (voices_[i]) { voices_[i]->Stop(0); voices_[i]->DestroyVoice(); }
  }
  if (master_) master_->DestroyVoice();
  if (xaudio_) xaudio_->Release();
}

bool GpSfx::start() {
  if (started_) return true;
  // The UI thread already inits COM for D3D/WIC; init anyway (S_FALSE / a changed
  // apartment are both fine to ignore — XAudio2.9 does not strictly require it).
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
    xaudio_ = nullptr;
    return false;
  }
  if (FAILED(xaudio_->CreateMasteringVoice(&master_, 2, 44100))) {
    xaudio_->Release(); xaudio_ = nullptr;
    return false;
  }
  const WAVEFORMATEX& fmt = SharedFormat();
  for (int i = 0; i < kVoicePool; i++) {
    if (FAILED(xaudio_->CreateSourceVoice(&voices_[i], &fmt))) voices_[i] = nullptr;
  }
  started_ = true;
  return true;
}

// True if any pool voice still has a queued buffer (a retired sample may be in use).
static bool AnyVoiceBusy(IXAudio2SourceVoice* const* voices, int n) {
  for (int i = 0; i < n; i++) {
    if (!voices[i]) continue;
    XAUDIO2_VOICE_STATE st;
    voices[i]->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (st.BuffersQueued > 0) return true;
  }
  return false;
}

void GpSfx::define(int slot, const Sound& sound) {
  if (slot < 0 || slot >= kMaxSfxSlots) return;
  if (!start()) return;
  std::vector<float> pcm(sound.samples.size());
  for (size_t i = 0; i < sound.samples.size(); i++) pcm[i] = (float)sound.samples[i];
  // Retire (don't free) the old buffer — a pool voice may still be reading it.
  if (slots_[slot].defined && !slots_[slot].pcm.empty()) {
    retired_.push_back(std::move(slots_[slot].pcm));
  }
  slots_[slot].pcm = std::move(pcm);
  slots_[slot].defined = true;
}

bool GpSfx::play(int slot) {
  if (slot < 0 || slot >= kMaxSfxSlots || !slots_[slot].defined) return false;
  if (!start()) return false;
  if (slots_[slot].pcm.empty()) return false;

  // Safe to reclaim retired buffers only when every voice is idle.
  if (!retired_.empty() && !AnyVoiceBusy(voices_, kVoicePool)) retired_.clear();

  // Prefer an idle voice; else round-robin steal (flush the oldest).
  IXAudio2SourceVoice* v = nullptr;
  for (int k = 0; k < kVoicePool; k++) {
    int idx = (next_voice_ + k) % kVoicePool;
    if (!voices_[idx]) continue;
    XAUDIO2_VOICE_STATE st;
    voices_[idx]->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (st.BuffersQueued == 0) { v = voices_[idx]; next_voice_ = (idx + 1) % kVoicePool; break; }
  }
  if (!v) {
    v = voices_[next_voice_];
    next_voice_ = (next_voice_ + 1) % kVoicePool;
    if (v) { v->Stop(0); v->FlushSourceBuffers(); }
  }
  if (!v) return false;

  XAUDIO2_BUFFER buf = {};
  buf.AudioBytes = (UINT32)(slots_[slot].pcm.size() * sizeof(float));
  buf.pAudioData = reinterpret_cast<const BYTE*>(slots_[slot].pcm.data());
  buf.Flags = XAUDIO2_END_OF_STREAM;
  if (FAILED(v->SubmitSourceBuffer(&buf))) return false;
  return SUCCEEDED(v->Start(0));
}

}  // namespace windart_gamepane
