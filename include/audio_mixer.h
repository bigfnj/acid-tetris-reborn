#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct MDRIVER;

namespace atet {

// One software mixer for music AND sound effects, modelled on the MikMod 2.x mixer the original
// runs (RE, flat image: VC_WriteSamples 0x7C10, AddChannel 0x7882, output stage 0x7A71, volume
// tables 0x7E11, voice setters 0x7FE3-0x8028, SFX entry 0x6817):
//
//   * every voice steps its sample with an 11-bit fixed-point accumulator, increment
//     floor(frq * 2048 / mixfreq) (0x769E), forward/reverse/bidi loops as in 0x7882;
//   * kicks, volume, pan and frequency take effect only at a player tick, which comes every
//     mixfreq*125 / (bpm*50) output frames (0x7C3D-0x7C5B); the libmikmod player runs there;
//   * a voice adds table[idx][s8] per channel with idxL = (255-pan)*v/255, idxR = pan*v/255
//     (0x7CFD-0x7D41). The tables were built once for two voices, so one table step is worth
//     4 output units (stereo, 16-bit; mono uses table[v], 2 units). Music and both SFX voices
//     share the buffer and one saturating clamp;
//   * kAuthentic reads the high byte of every sample with zero-order hold (the original's 8-bit
//     mixer); kClean interpolates 16-bit samples linearly at the same gains (the port's former
//     character, kept as an option).
//
// The mixer is libmikmod's output driver (MikModDriver()), so the tracker player is driven from
// the audio thread at the exact sample positions of its ticks, and it feeds an SDL3 audio stream
// on every platform (this replaces libmikmod's WinMM and nosound drivers). Without a device
// (Open(open_device=false), used by --dump-audio) the caller pulls frames with RenderOutput(), or,
// for --dump-audio-via-stream, with PullThroughStream().
class AudioMixer {
 public:
  enum class Quality { kAuthentic, kClean };
  struct Config {
    int mix_rate = 44100;  // SETUP.DAT default
    bool stereo = true;
    // false: 8-bit output. 0x7A71 shifts the accumulator by 24-bits instead of 16-bits and clamps
    // to -128..127, which equals the 16-bit result shifted right by 8 (kept on the int16 scale).
    bool sixteen_bit = true;
    Quality quality = Quality::kAuthentic;
    // Output delay in frames per channel at mix_rate, music and effects together. 0 (default): no
    // added delay, which sounded closest to the original in a side-by-side listening test.
    // kOriginalOutputDelay (any negative value): the original's Sound Blaster DMA lap,
    // OriginalOutputDelayFrames(mix_rate). N > 0: N frames, clamped to kMaxOutputDelaySeconds.
    int output_delay_frames = 0;
  };
  // The SB driver's Init sizes its DMA buffer as mixfreq / 12 frames (0xCFC5-0xCFDE, a truncating
  // idiv by 12) and mixes one such lap ahead of the play position, so every sound leaves the mixer
  // that much before it is heard: 3675 frames (83.3 ms) at 44,100 Hz. Output buffering, not game
  // logic: the player ticks, kicks and frames_rendered() keep the mixer's own timeline.
  static int OriginalOutputDelayFrames(int mix_rate) { return mix_rate / 12; }
  static constexpr int kOriginalOutputDelay = -1;
  static constexpr int kMaxOutputDelaySeconds = 2;
  static constexpr int kSfxVoiceClasses = 2;  // 0x6817 EDX: voice = module channels + class

  // One entry per player tick or SFX kick, for the audit trace and tests.
  struct Event {
    std::uint64_t frame = 0;  // mixer frame at which it took effect (heard output_delay_frames() later)
    int kind = 0;             // 0 = player tick, 1 = SFX voice kicked
    int voice_class = -1;     // SFX: class voice
    int sample = -1;          // SFX: sample handle
    int volume = 0;           // SFX: 0..64
    int pan = 0;              // SFX: 0..255
    int tick_length = 0;      // tick: frames until the next tick
  };

  AudioMixer() = default;
  AudioMixer(const AudioMixer&) = delete;
  AudioMixer& operator=(const AudioMixer&) = delete;
  ~AudioMixer();

  bool Open(const Config& config, bool open_device, std::string* error_out);
  // Stops the SDL device (the audio thread no longer calls into libmikmod). Call before
  // MikMod_Exit; Close() calls it too.
  void CloseDevice();
  void Close();

  [[nodiscard]] bool is_open() const { return open_; }
  [[nodiscard]] bool has_device() const { return stream_ != nullptr; }
  [[nodiscard]] const Config& config() const { return config_; }
  void set_quality(Quality quality);

  // The libmikmod output driver backed by this mixer (one mixer at a time).
  static MDRIVER* MikModDriver();

  // Every libmikmod call made outside the audio thread must hold this lock.
  void Lock();
  void Unlock();
  class ScopedLock {
   public:
    explicit ScopedLock(AudioMixer& mixer) : mixer_(mixer) { mixer_.Lock(); }
    ~ScopedLock() { mixer_.Unlock(); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

   private:
    AudioMixer& mixer_;
  };

  // Loads a PCM WAV (8-bit unsigned or 16-bit signed, mono) as a sound-effect sample and returns
  // its handle (or -1).
  int LoadWavSample(const std::vector<Uint8>& wav_bytes, std::string* error_out);
  // 0x6817: percent 0 returns at once; otherwise V = min(percent*64/100, 64), pan masked to 8 bits,
  // the class voice is (re)kicked at the next player tick (a newer request on the same class
  // replaces an older one, also when V is 0).
  void PlaySfx(int voice_class, int sample_handle, int volume_percent, int pan);

  // Mixes `frames` interleaved int16 frames (channels per config) and advances the player.
  size_t Render(std::int16_t* out, size_t frames);
  // Render() followed by the output delay line: what leaves the mixer, delayed by
  // output_delay_frames(). Both outputs use it (the SDL device callback and the offline capture,
  // MusicPlayer::CaptureMix), so the device and the capture carry the same lap.
  size_t RenderOutput(std::int16_t* out, size_t frames);
  // Active output delay, frames per channel (set by Open from the config).
  [[nodiscard]] int output_delay_frames() const { return delay_frames_; }
  // --dump-audio-via-stream (no device open): the capture pulls `frames` from an SDL_AudioStream
  // attached with AttachStream() and fed by StreamCallback(), so it has the device's pull semantics
  // on the step clock (whatever happens to a stream on attach shows in the capture too).
  size_t PullThroughStream(std::int16_t* out, size_t frames);

  // Diagnostics (take the lock themselves).
  [[nodiscard]] std::uint64_t frames_rendered();
  [[nodiscard]] std::uint64_t player_ticks();
  std::vector<Event> TakeEvents();
  [[nodiscard]] std::string AuditJson();

  // ---- libmikmod driver entry points (internal; called with the lock held) ----
  struct Sample {
    std::vector<std::int16_t> data;  // signed 16-bit (8-bit sources are value << 8)
    std::uint32_t length = 0;
    std::uint32_t rate = 0;          // SFX samples: playback rate
    std::uint16_t flags = 0;         // libmikmod SF_* of the sample
    bool from_mikmod = false;
  };
  struct Voice {
    bool kick = false;
    bool active = false;
    std::uint16_t flags = 0;  // libmikmod SF_LOOP / SF_BIDI / SF_REVERSE
    int handle = -1;
    std::uint32_t start = 0;
    std::uint32_t size = 0;
    std::uint32_t reppos = 0;
    std::uint32_t repend = 0;
    std::uint32_t frq = 10000;  // 2.x VC_Init default
    int vol = 0;                // 0..64
    int pan = 128;              // 0..255
    std::int64_t current = 0;   // 11-bit fixed point
    std::int64_t increment = 0;
    int lsel = 0;               // stereo: left table index; mono: table index
    int rsel = 0;
  };
  int DriverSampleLoad(void* sload, int type);
  void DriverSampleUnload(int handle);
  int DriverSetNumVoices(int voices);
  Voice* MusicVoice(int voice);

 private:
  static void SDLCALL StreamCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount);
  // Wires an output stream to the mixer (its get-callback). The one attach both the device stream
  // and the capture stream (PullThroughStream) go through.
  bool AttachStream(SDL_AudioStream* stream);
  size_t RenderLocked(std::int16_t* out, size_t frames);
  void BeginTick();
  void MixPortion(int frames);
  void AddChannel(Voice& voice, int frames);
  int AllocateSampleSlot();

  Config config_{};
  bool open_ = false;
  SDL_Mutex* mutex_ = nullptr;
  SDL_AudioStream* stream_ = nullptr;
  std::vector<std::int16_t> callback_buffer_;
  // Output delay line: a ring of delay_frames_ * channels samples, zeroed at Open. Each output
  // sample swaps with the oldest one, so the delay stays exactly delay_frames_ whatever block
  // sizes the device or the capture ask for (a one-off silence prefill in the SDL stream would
  // drain, because the get-callback only tops up what is missing).
  std::vector<std::int16_t> delay_line_;
  size_t delay_pos_ = 0;
  int delay_frames_ = 0;
  SDL_AudioStream* capture_stream_ = nullptr;  // PullThroughStream only
  std::uint64_t callback_frames_ = 0;          // frames StreamCallback rendered (any stream)

  std::vector<Sample> samples_;
  std::vector<Voice> music_voices_;
  std::array<Voice, kSfxVoiceClasses> sfx_voices_{};
  std::vector<std::int32_t> accumulator_;  // per output sample
  std::uint32_t tick_left_ = 0;
  std::uint32_t tick_length_ = 0;
  std::uint64_t frames_rendered_ = 0;
  std::uint64_t player_ticks_ = 0;
  std::vector<Event> events_;
};

}  // namespace atet
