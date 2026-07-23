#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace atet {

// Plays the recovered twelve-slot SFX table (and a single probe clip) through
// SDL. Playback models the original mixer entry point 0x6817 (RE:
// docs/findings/audio-playback-parameter-pass-2026-04-14.md): the caller picks a
// slot, a per-play pan (0x80 = centered), and a user-facing SFX volume percent
// that 0x6817 scales into the engine range 0..0x40 before mixing. Clips are the
// original mono assets; pan is rendered as left/right gains through a small
// mono->stereo mixdown, since spatialized (slot 0) versus centered playback is
// the audible parity target the DOS mixer/vtable internals are not.
class AudioProbe {
 public:
  static constexpr int kPanCenter = 0x80;   // ECX center value at the 0x6817 call sites
  static constexpr int kEngineVolumeMax = 0x40;  // 0x6817 scales percent into 0..0x40

  AudioProbe() = default;
  AudioProbe(const AudioProbe&) = delete;
  AudioProbe& operator=(const AudioProbe&) = delete;

  ~AudioProbe();

  bool Initialize(const std::vector<Uint8>& wav_bytes, std::string* error_out);
  bool InitializeSfxSlots(const std::array<std::vector<Uint8>, 12>& wav_bytes, std::string* error_out);
  bool Play(std::string* error_out);
  // volume_percent is the saved user-facing SFX percent (0..100); pan is the
  // 0x6817 ECX term (0..255, kPanCenter = centered).
  bool PlaySfxSlot(int slot, int volume_percent, int pan, std::string* error_out);
  void Shutdown();

  [[nodiscard]] bool ready() const;
  [[nodiscard]] bool sfx_ready() const { return loaded_sfx_slots_ == sfx_slots_.size(); }
  [[nodiscard]] int loaded_sfx_slots() const { return loaded_sfx_slots_; }
  [[nodiscard]] const std::string& wav_path() const;

  // The engine mixer volume (0..kEngineVolumeMax) 0x6817 derives from a saved
  // user-facing percent. Exposed so the volume curve is observable/loggable.
  [[nodiscard]] static int EngineVolume(int volume_percent);

 private:
  struct Clip {
    std::string path;
    SDL_AudioStream* stream = nullptr;  // source format is F32 stereo
    std::vector<float> mono;            // decoded mono samples, normalized [-1, 1]
    int freq = 0;

    [[nodiscard]] bool ready() const { return stream != nullptr && !mono.empty(); }
  };

  bool LoadClip(const std::vector<Uint8>& wav_bytes, std::string_view label, Clip* clip, std::string* error_out);
  bool PlayClip(Clip* clip, float gain, int pan, std::string* error_out);
  void ShutdownClip(Clip* clip);
  void ShutdownSfxSlots();

  Clip probe_clip_;
  std::array<Clip, 12> sfx_slots_{};
  int loaded_sfx_slots_ = 0;
};

}  // namespace atet
