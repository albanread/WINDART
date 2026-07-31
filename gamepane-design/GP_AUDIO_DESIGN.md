# GP_AUDIO_DESIGN — SFX and tunes on Windows (XAudio2 + winmm)

The AVFoundation → Windows audio mapping for the game pane. Two subsystems:
SFX playback (`GpSfx`, gp_engine.mm:1040-1115) and tune/MIDI playback
(`GpMusic`, gp_engine.mm:916-1038). The synthesizer itself is portable.

- **`gp_synth.cc` / `gp_synth.h` — REUSED VERBATIM.** Pure portable C++ (no ObjC,
  no GPU, no platform API); the header says so (gp_synth.h:1-15). It renders
  44.1 kHz **stereo interleaved doubles** into `Sound.samples` (gp_synth.h:74-79).
  The 11 presets and the LCG are exact. Nothing to translate — just compile it
  into `dart_win32`.
- Audio is explicitly the last milestone / deferrable (`GAMEPANE_PLAN.md` §7 M3,
  M4). Keep the port light; correctness over fidelity for tunes.

---

## 1. SFX: AVAudioEngine → XAudio2

### 1.1 Object map

| AVFoundation (gp_engine.mm) | XAudio2 | Notes |
|---|---|---|
| `AVAudioEngine` (one per process) | `IXAudio2` + `IXAudio2MasteringVoice` | one per process, lazily started (gp_engine.mm:1303-1308) |
| `AVAudioPlayerNode` (:1063) | a **pool** of `IXAudio2SourceVoice` | polyphony — see §1.3 |
| `AVAudioFormat` 44100/2/float32 (:1065) | `WAVEFORMATEX` `WAVE_FORMAT_IEEE_FLOAT`, 2ch, 44100, 32-bit | shared, one struct |
| `AVAudioPCMBuffer` per slot (:1090) | `XAUDIO2_BUFFER` + owned `float` sample array per slot | 64 slots (`kMaxSfxSlots`) |
| `scheduleBuffer:completionHandler:nil` (:1112) | `SubmitSourceBuffer` on a pooled voice + `Start` | overlapping SFX → different voices |

### 1.2 Startup (`start()`, gp_engine.mm:1060-1083)

The AVFoundation wiring order is load-bearing (attach→format→connect→start→play).
The XAudio2 order:
1. (Ensure `CoInitializeEx` on the thread — the UI thread already inits COM for
   D3D/WIC. XAudio2.9 on Win10+ does not strictly require it, but init anyway.)
2. `XAudio2Create(&xaudio_, 0, XAUDIO2_DEFAULT_PROCESSOR)`.
3. `xaudio_->CreateMasteringVoice(&master_, 2, 44100)`.
4. Create the source-voice pool (§1.3).
On failure, return false and leave `started_=false` (matches the mac's
graceful-degrade — a failed audio init never kills the pane). **One `IXAudio2`
per process** (the mac's "two concurrent starts abort" hazard, gp_engine.h:
218-221, does not exist for XAudio2, but the single-instance shape is kept
anyway). Link `xaudio2.lib`; header `<xaudio2.h>`; use **XAudio2.9** (built into
Windows 10+, no redistributable).

### 1.3 The polyphony difference (the one real structural change)

`AVAudioPlayerNode` accepts overlapping `scheduleBuffer` calls and **sums** them
internally ("overlapping SFX just sum", `GAMEPANE_PLAN.md` §5). A single
`IXAudio2SourceVoice` plays its submitted buffers **sequentially**, not
overlapped. To reproduce summed polyphony, use a **pool of source voices**:

- Create N voices (e.g. `kVoicePool = 16..32`) up front, all with the shared
  `WAVEFORMATEX`, via `CreateSourceVoice`. They stay alive for the process.
- `play(slot)` (gp_engine.mm:1108-1115): pick the next pool voice
  (round-robin, or the first whose queued-buffer count is 0 via
  `GetState().BuffersQueued`), `SubmitSourceBuffer(&slotBuffer)`, `Start`. A
  voice already playing another SFX is left alone (round-robin moves on), so
  concurrent sounds land on different voices and the mastering voice sums them —
  same audible result. A voice finishing just becomes idle (BuffersQueued→0).
- Rate note: SFX are short (≤10 s cap, gp_synth.h:25); a 16-voice pool comfortably
  covers a retro game's simultaneous effects.

### 1.4 define(slot, Sound) (gp_engine.mm:1085-1106)

The synth's `Sound.samples` is **interleaved f64** (gp_synth.h:78). The mac
DE-interleaves into two float32 channel planes (AVAudio wants planar,
gp_engine.mm:1095-1102). **XAudio2 wants interleaved PCM — so the Windows
conversion is simpler: f64 interleaved → f32 interleaved (no deinterleave).**

- `frames = samples.size()/2`. Allocate/own `std::vector<float> pcm(samples.size())`
  per slot; `for i: pcm[i] = (float)samples[i]`.
- Fill an `XAUDIO2_BUFFER`: `AudioBytes = pcm.size()*4`, `pAudioData =
  (BYTE*)pcm.data()`, `Flags = XAUDIO2_END_OF_STREAM`, `PlayBegin/PlayLength =
  0`. Store both the vector and the buffer per slot (the buffer points INTO the
  vector). define replaces the old slot (release/free the old vector).
- **Lifetime hazard (flag):** XAudio2 does NOT copy — `pAudioData` must stay
  valid until the voice finishes with it. If `define(slot)` replaces a buffer
  that a pool voice is still playing, freeing the old vector is a use-after-free.
  Mitigation: keep the *previous* vector alive one extra generation (a small
  retire list), or `for each pool voice: if playing this slot's buffer, Flush
  SourceBuffers/Stop` before freeing. Simplest robust: retire the old vector to a
  list drained when all voices report idle. (The mac side-stepped this because
  `AVAudioPCMBuffer` owns/retains its samples.)

### 1.5 Teardown (`~GpSfx`, gp_engine.mm:1046-1057)

`Stop`+`DestroyVoice` each pool voice; `master_->DestroyVoice()`;
`xaudio_->Release()`. Free all slot vectors. (The mac's "release the format
object, stop before release" fixes are N/A — WAVEFORMATEX is a plain struct.)

---

## 2. Tunes: AVMIDIPlayer → winmm

`GpMusic` (gp_engine.mm:916-1038) builds an in-memory Standard MIDI File
(`BuildSmf`, gp_engine.mm:936-976) and plays it through `AVMIDIPlayer` — which
carries its own GM soundbank and its own timing, with looping done by a per-frame
poll (`poll()`, gp_engine.mm:1022-1031). Windows has no "play this SMF blob"
call with a built-in synth in one API; three routes, recommended in order:

### 2.1 Primary — winmm `midiStream` + the GS Wavetable Synth

`midiStreamOpen`/`midiStreamOut` accept a buffer of **timestamped short MIDI
events** (`MIDIEVENT` records: delta-time in ticks + a packed short message) and
play them with correct timing through the **Microsoft GS Wavetable Synth** (the
built-in GM soft-synth on every Windows install) — the closest analogue to "hand
a device an event list and it plays." This maps `GpMusic` cleanly:

- **define(slot, bpm, events)** (gp_engine.mm:989-1006): the wire is the flat
  `[timeMs, status, d1, d2]*n` list (gp_natives.mm:361-379). Convert directly to
  `MIDIEVENT[]`:
  - Reuse **the tick math from `BuildSmf`** (gp_engine.mm:958-961): `tick = ms*bpm/125`
    (a quarter = 480 ppq), `delta = tick - last_tick`. Set the stream's time
    division via `midiStreamProperty(MIDIPROP_TIMEDIV)` = 480 and tempo via
    `MIDIPROP_TEMPO` = `60000000/bpm` (mirrors the SMF Set-Tempo meta,
    gp_engine.mm:949-953).
  - Each event → `MIDIEVENT{ dwDeltaTime=delta, dwStreamID=0,
    dwEvent=MEVT_SHORTMSG | (status | d1<<8 | d2<<16) }`. Program-change (0xC0)
    has no d2 (gp_engine.mm:966), pack 2 bytes.
  - Wrap in a `MIDIHDR` (`lpData`, `dwBufferLength`, `dwBytesRecorded`), prepare
    with `midiOutPrepareHeader`.
  - **Note:** the MThd/MTrk SMF *framing* in `BuildSmf` (gp_engine.mm:940-975) is
    NOT needed for midiStream — only its ms→tick conversion is reused. (If the
    architect prefers to keep `BuildSmf` whole, use route 2.2.)
- **control(slot, mode)** (gp_engine.mm:1008-1020): mode 0 stop → `midiStreamStop`
  + `midiStreamRestart`-reset; mode 1 play once → `midiStreamOut` + `midiStream
  Restart`; mode 2 loop → same, set `looping_[slot]=true`.
- **poll()** (gp_engine.mm:1022-1031): looping restart on completion. midiStream
  signals done via the `MOM_DONE` callback (a `MIDIHDR` returns). Keep the mac's
  per-frame poll shape: a flag set by the callback (or `midiOutGetVolume`-style
  state check), and `poll()` (called from `render_present`) re-`midiStreamOut`s
  the buffer for looping slots. `MIDIHDR` must be re-prepared/reset per replay.
- **stop_all()** / dtor: `midiStreamStop` + `midiOutUnprepareHeader` +
  `midiStreamClose` per slot. 8 slots (`kMaxTunes`).

### 2.2 Alternative — MCI + a temp `.mid` (reuses `BuildSmf` verbatim)

If keeping `BuildSmf` intact is preferred: write its SMF bytes to a temp file,
`mciSendString("open <file> type sequencer alias tuneN")`, `"play tuneN"`; loop
by polling `"status tuneN mode"` for `stopped` and replaying (matches `poll()`).
Simpler code (SMF builder unchanged) but: temp files, coarse control, and MCI
tune aliases are limited. This mirrors the mac's own "temp `.mid` +
AVMIDIPlayer" note (`GAMEPANE_PLAN.md` §5).

### 2.3 Not recommended for MVP — bundled soft-synth + SoundFont

fluidsynth-class synth + a GM SoundFont gives the best fidelity/control and no
dependence on the GS Wavetable Synth, but it is heavy (extra dependency + asset)
and tunes are a deferred M4 feature. Record it as the "fidelity upgrade" path,
not the MVP.

**Recommendation:** ship **2.1 (midiStream)** — it is redistributable-free,
gives real timing + the built-in GM synth, and matches `GpMusic`'s poll-loop
looping. `BuildSmf`'s tick conversion is the reused piece.

---

## 3. Summary of what ports how

| Piece | Windows | Reuse |
|---|---|---|
| `gp_synth.cc` (synth + 11 presets + LCG) | — | **verbatim** (pure C++) |
| `GpSfx` playback | XAudio2 (IXAudio2 + mastering voice + source-voice pool) | rewrite (small) |
| f64 interleaved → PCM | f32 **interleaved** (simpler than mac's deinterleave) | — |
| `GpMusic` playback | winmm `midiStream` + GS Wavetable Synth | rewrite (small) |
| `BuildSmf` | tick math reused by midiStream; whole SMF reused by MCI route | reusable |

Libs: `xaudio2.lib` (already on the link line, `WINDOWS_PORTING_PLAN.md` §4) and
`winmm.lib` (add — not currently listed; the plan's GUI-lib set omits it).
