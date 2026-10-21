#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atet {

class AudioMixer;

// Plays the recovered ACiD Tetris tracker music (MikMod UN05 modules) through libmikmod.
// The original embeds a pre-3.0 MikMod (credited "Music System: Jean Paul Mikkers"); libmikmod
// 3.3.13 runs here with its MikMod 2.x compatibility switches on (cmake/libmikmod_mm2_compat.cmake)
// plus two load-time fixups for these modules, and it renders through AudioMixer, the port's
// MikMod 2.x-style mixer on an SDL3 audio stream (all platforms, no WinMM).
//
// Volume follows the original: the music percent P (0..100) becomes the MikMod 2.x song volume
// floor(7P/20) (0x68BB), applied per voice by the player. Ramps model 0x699E/0x6620/0x6965 and the
// track loader 0x6544: every load (startup included) first runs a 0x20-tick fade-out, then loads,
// starts the module and fades in over 0x20 ticks to floor(31P/32), where the music stays until the
// player edits the volume. The exit-to-DOS path fades out over 0x40 ticks. One tick is one logic
// step here (the original steps the ramp from its 60 Hz timer ISR).
class MusicPlayer {
 public:
  static constexpr int kTrackFadeTicks = 0x20;  // 32: track-change fade in/out (0x6544)
  static constexpr int kExitFadeTicks = 0x40;   // 64: exit-to-DOS fade out (state 3)

  MusicPlayer() = default;
  MusicPlayer(const MusicPlayer&) = delete;
  MusicPlayer& operator=(const MusicPlayer&) = delete;
  ~MusicPlayer();

  // track_data: the six .uni module byte buffers (from ATET.DAT), indexed by track id. The
  // mixer must be open; it outlives the player.
  bool Initialize(AudioMixer* mixer, const std::array<std::vector<std::uint8_t>, 6>& track_data,
                  std::string* error_out);

  // Ensure the given track is the one playing (0x6544). The current track keeps playing
  // untouched (no volume change: the original never re-applies P after a fade-in). Otherwise the
  // music fades out over kTrackFadeTicks (also with nothing playing, as at startup), the new
  // track loads and starts, and it fades in over kTrackFadeTicks. A request made while a change is
  // still fading out or loading waits for the load and then runs as its own change (latest request
  // wins), as the original's blocking loader does with a key released during it. Ignored once the
  // exit fade began.
  bool RequestTrack(int index, int volume_percent, std::string* error_out);

  // Schedule the exit-to-DOS music fade (kExitFadeTicks). Playback stops when it completes.
  // A second call, or a track request, during the exit fade is ignored (the original takes no
  // input once Exit Game is chosen).
  void BeginExitFade(int volume_percent);

  // The player edited the music volume (Options): applies P at once when no ramp runs; a running
  // ramp follows the new value.
  void SetVolume(int volume_percent);

  void Update();  // advance the ramp; once per logic step
  void Stop();
  void Shutdown();

  [[nodiscard]] bool available() const { return initialized_; }
  [[nodiscard]] int current_track() const { return current_track_; }
  // MikMod 2.x song volume last applied (0..35 for P = 0..100).
  [[nodiscard]] int song_volume() const { return applied_song_volume_; }

  // Compact ramp state for the --debug-state per-frame log.
  [[nodiscard]] std::string debug_state() const;

  [[nodiscard]] bool exit_fade_active() const { return exiting_ && ramp_mode_ != RampMode::kNone; }
  [[nodiscard]] float exit_fade_progress() const;
  [[nodiscard]] bool fade_active() const { return ramp_mode_ != RampMode::kNone || pending_track_ >= 0; }

  // ---- Audit instrumentation (audit/instrument worktree only) ----
  // Capture mode: the caller pulls exactly the frames it wants with CaptureMix() (music and
  // effects, through the shared mixer). Must be set before Initialize().
  void SetCaptureMode(bool enabled) { capture_mode_ = enabled; }
  size_t CaptureMix(std::int16_t* out, size_t frames);
  [[nodiscard]] std::string AuditJson() const;

 private:
  bool capture_mode_ = false;
  enum class RampMode { kNone, kFadeIn, kFadeOut };

  bool LoadAndStart(int index, std::string* error_out);
  void BeginTrackChange(int index);
  void ApplyMikMod2xFixups(void* module, int index);
  void BeginFadeIn();
  void ApplyMusicPercent(int volume_percent);
  [[nodiscard]] int RampVolumePercent() const;
  void CompleteRamp();
  void UnloadModule();

  AudioMixer* mixer_ = nullptr;
  bool initialized_ = false;
  int current_track_ = -1;
  void* module_ = nullptr;  // libmikmod MODULE*, kept opaque to avoid leaking the header
  std::array<std::vector<std::uint8_t>, 6> track_data_{};
  int keyoffs_rewritten_ = 0;    // last load: (UNI_NOTE 96) -> (UNI_KEYFADE 0)
  std::uint64_t module_start_frame_ = 0;  // mixer frame of the last Player_Start

  // Volume-ramp controller state.
  int target_volume_percent_ = 100;  // user-configured music volume the ramps scale
  int applied_percent_ = 0;          // percent last applied through 0x68BB
  int applied_song_volume_ = 0;      // floor(7 * applied_percent_ / 20)
  RampMode ramp_mode_ = RampMode::kNone;
  int ramp_total_ticks_ = 0;
  int ramp_remaining_ticks_ = 0;
  int pending_track_ = -1;  // track to load once the running fade-out completes
  int queued_track_ = -1;   // latest request made during a change; its own change follows the load
  bool exiting_ = false;    // an exit fade has been scheduled
};

}  // namespace atet
