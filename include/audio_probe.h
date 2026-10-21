#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace atet {

class AudioMixer;

// The recovered twelve-slot SFX table (and the Space probe clip), played the way the original's
// SFX entry 0x6817 plays them: through the same mixer as the music (AudioMixer), on one of two
// voices picked by the call site's class (0 = lock and menu blips, 1 = everything else), so a new
// effect cuts whatever its class voice was playing. Volume is V = min(percent*64/100, 64) on the
// mixer's 0..64 voice scale; pan is the 0..255 ECX term (0x80 = centre), split linearly by the
// mixer; the effect starts at the next music player tick.
class AudioProbe {
 public:
  static constexpr int kPanCenter = 0x80;        // ECX centre value at the 0x6817 call sites
  static constexpr int kEngineVolumeMax = 0x40;  // 0x6817 scales percent into 0..0x40

  AudioProbe() = default;
  AudioProbe(const AudioProbe&) = delete;
  AudioProbe& operator=(const AudioProbe&) = delete;

  ~AudioProbe();

  bool Initialize(AudioMixer* mixer, const std::vector<Uint8>& wav_bytes, std::string* error_out);
  bool InitializeSfxSlots(AudioMixer* mixer, const std::array<std::vector<Uint8>, 12>& wav_bytes,
                          std::string* error_out);
  bool Play(std::string* error_out);
  // volume_percent is the saved user-facing SFX percent (0..100); pan is the 0x6817 ECX term;
  // voice_class is the 0x6817 EDX term.
  bool PlaySfxSlot(int slot, int volume_percent, int pan, int voice_class, std::string* error_out);
  void Shutdown();

  [[nodiscard]] bool ready() const { return probe_handle_ >= 0; }
  [[nodiscard]] bool sfx_ready() const { return loaded_sfx_slots_ == static_cast<int>(sfx_handles_.size()); }
  [[nodiscard]] int loaded_sfx_slots() const { return loaded_sfx_slots_; }
  [[nodiscard]] const std::string& wav_path() const { return probe_label_; }

  // The engine voice volume (0..kEngineVolumeMax) 0x6817 derives from a saved percent.
  [[nodiscard]] static int EngineVolume(int volume_percent);

 private:
  AudioMixer* mixer_ = nullptr;
  int probe_handle_ = -1;
  std::string probe_label_;
  std::array<int, 12> sfx_handles_{{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}};
  int loaded_sfx_slots_ = 0;
};

}  // namespace atet
