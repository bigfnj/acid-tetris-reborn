#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atet {

// Plays the recovered ACiD Tetris tracker music (MikMod UN05 modules) through
// libmikmod. The original game's credited "Music System" is MikMod's own
// author, and ATET.DAT embeds MikMod UNI modules, so libmikmod reproduces the
// original music engine rather than approximating it.
//
// libmikmod owns the music output device (WinMM on the Windows desktop, the
// nosound driver when SDL is running headless), so playback is independent of
// the SDL audio path used for sound effects. Call Update() once per frame to
// keep the software mixer fed and to advance the volume-ramp controller.
//
// Volume ramps model the original's small music-ramp system (RE:
// docs/findings/frontend-exit-and-music-ramp-pass-2026-04-14.md). The original
// track loader (0x6544) fades the current track out over 0x20 ticks, waits for
// the ramp to finish, loads the new track, then fades it in over 0x20 ticks.
// The hard exit-to-DOS path (state 3) fades out over 0x40 ticks before shutting
// the runtime down. One "tick" maps to one rendered frame here, matching the
// port's other frame-count parity gates.
class MusicPlayer {
 public:
  // Ticks (rendered frames) for each modelled ramp, taken from the original.
  static constexpr int kTrackFadeTicks = 0x20;  // 32: track-change fade in/out
  static constexpr int kExitFadeTicks = 0x40;   // 64: exit-to-DOS fade out

  MusicPlayer() = default;
  MusicPlayer(const MusicPlayer&) = delete;
  MusicPlayer& operator=(const MusicPlayer&) = delete;
  ~MusicPlayer();

  // track_data: the six .uni module byte buffers (from ATET.DAT), indexed by track id.
  bool Initialize(const std::array<std::vector<std::uint8_t>, 6>& track_data, std::string* error_out);

  // Ensure the given track is the one playing, modelling the original track
  // loader. If it is already the current track, playback continues
  // uninterrupted (music continuity across scene changes) and only the target
  // volume is (re)applied. If it is a different track and something is already
  // playing, the current track fades out over kTrackFadeTicks, then the new
  // track loads and fades in over kTrackFadeTicks. With nothing playing (cold
  // startup) the new track loads immediately and fades in.
  bool RequestTrack(int index, int volume_percent, std::string* error_out);

  // Schedule the exit-to-DOS music fade (mode 2, kExitFadeTicks). Playback is
  // stopped once the fade completes. exit_fade_active() reports whether the
  // caller should keep pumping Update() before shutting down.
  void BeginExitFade(int volume_percent);

  // Set the target (user-configured) music volume. Applies immediately when no
  // ramp is running; while a ramp is active the ramp lands on the new target.
  void SetVolume(int volume_percent);

  void Update();  // pump the software mixer and advance the ramp; once per frame
  void Stop();
  void Shutdown();

  [[nodiscard]] bool available() const { return initialized_; }
  [[nodiscard]] int current_track() const { return current_track_; }

  // Compact ramp state for the --debug-state per-frame log, so the modelled
  // track-change and exit fades are observable the same way the gameplay gates
  // are (e.g. " music_track=0 music_ramp=fadeout music_ramp_left=63 music_vol=98").
  [[nodiscard]] std::string debug_state() const;

  // True while a scheduled exit fade is still ramping. Once it finishes the
  // module is stopped and this returns false, so the main loop can safely quit.
  [[nodiscard]] bool exit_fade_active() const { return exiting_ && ramp_mode_ != RampMode::kNone; }

  // Exit-fade progress 0.0 (just started) -> 1.0 (fully faded). Lets the frontend
  // fade the screen to black in step with the music (state-3 exit path 0x3830).
  [[nodiscard]] float exit_fade_progress() const;

  // True while any ramp (fade in/out) or a pending track load is in flight.
  [[nodiscard]] bool fade_active() const { return ramp_mode_ != RampMode::kNone || pending_track_ >= 0; }

 private:
  enum class RampMode { kNone, kFadeIn, kFadeOut };

  bool LoadAndStart(int index, std::string* error_out);
  void BeginFadeIn();
  void ApplyMikModVolume(int volume_percent);
  [[nodiscard]] int RampVolumePercent() const;
  void CompleteRamp();
  void UnloadModule();

  bool initialized_ = false;
  bool output_enabled_ = false;
  int current_track_ = -1;
  void* module_ = nullptr;  // libmikmod MODULE*, kept opaque to avoid leaking the header
  std::array<std::vector<std::uint8_t>, 6> track_data_{};

  // Volume-ramp controller state.
  int target_volume_percent_ = 100;  // user-configured music volume the ramp targets
  RampMode ramp_mode_ = RampMode::kNone;
  int ramp_total_ticks_ = 0;
  int ramp_remaining_ticks_ = 0;
  int pending_track_ = -1;  // track queued to load once a fade-out completes
  bool exiting_ = false;    // an exit fade has been scheduled
};

}  // namespace atet
