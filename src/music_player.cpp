#include "music_player.h"

#include <cstring>

#include <SDL3/SDL.h>
#include <mikmod.h>

namespace atet {
namespace {

int ClampPercent(int value) {
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

int ToMikModVolume(int percent) {
  // Exact original music-volume curve (RE 0x68bb): the mixer music volume is
  // percent * 7 / 20 on MikMod's 0..128 scale, so 100% -> 35 (music sits well
  // below full, matching the original mix) rather than the full 128. The ramp
  // system (0x6965) feeds a linear percent through this same scaling.
  return (ClampPercent(percent) * 7) / 20;
}

}  // namespace

MusicPlayer::~MusicPlayer() { Shutdown(); }

bool MusicPlayer::Initialize(const std::array<std::vector<std::uint8_t>, 6>& track_data, std::string* error_out) {
  for (std::size_t i = 0; i < track_data.size(); ++i) {
    track_data_[i] = track_data[i];
  }

  MikMod_RegisterAllLoaders();

  // libmikmod owns the music device. Use WinMM on the desktop, but force the
  // nosound driver when SDL is running headless (SDL_AUDIODRIVER=dummy) so
  // automated smoke runs stay deterministic and need no audio device.
  const char* sdl_driver = SDL_getenv("SDL_AUDIODRIVER");
  const bool headless = sdl_driver != nullptr && std::strcmp(sdl_driver, "dummy") == 0;
#ifdef ATET_PORT_HAVE_WINMM
  if (!headless) {
    MikMod_RegisterDriver(&drv_win);
  }
#else
  (void)headless;
#endif
  MikMod_RegisterDriver(&drv_nos);

  md_mode |= DMODE_SOFT_MUSIC | DMODE_16BITS | DMODE_STEREO | DMODE_INTERP;
  md_mixfreq = 44100;

  if (MikMod_Init("")) {
    if (error_out != nullptr) {
      *error_out = std::string("MikMod_Init failed: ") + MikMod_strerror(MikMod_errno);
    }
    initialized_ = false;
    return false;
  }

  MikMod_SetNumVoices(-1, -1);
  output_enabled_ = (MikMod_EnableOutput() == 0);
  initialized_ = true;
  return true;
}

bool MusicPlayer::LoadAndStart(int index, std::string* error_out) {
  UnloadModule();

  const std::vector<std::uint8_t>& track = track_data_[static_cast<std::size_t>(index)];
  MODULE* mod = Player_LoadMem(reinterpret_cast<const char*>(track.data()), static_cast<int>(track.size()), 64, 0);
  if (mod == nullptr) {
    if (error_out != nullptr) {
      *error_out = std::string("Player_LoadMem failed: ") + MikMod_strerror(MikMod_errno);
    }
    current_track_ = -1;
    return false;
  }

  mod->wrap = 1;  // restart at the module's repeat position at the end -> loop
  mod->loop = 1;
  module_ = mod;
  current_track_ = index;
  Player_Start(mod);
  SDL_Log("Music: playing track %d ('%s')", index, mod->songname != nullptr ? mod->songname : "");
  return true;
}

bool MusicPlayer::RequestTrack(int index, int volume_percent, std::string* error_out) {
  if (!initialized_) {
    return false;
  }
  if (index < 0 || index >= static_cast<int>(track_data_.size())) {
    return false;
  }

  target_volume_percent_ = ClampPercent(volume_percent);

  // Continuity: the requested track is already the current one, so keep it
  // playing uninterrupted across scene changes and only (re)apply the volume
  // when no ramp is in flight.
  if (index == current_track_ && module_ != nullptr) {
    if (ramp_mode_ == RampMode::kNone) {
      ApplyMikModVolume(target_volume_percent_);
    }
    return true;
  }

  // A fade-out to this same track is already queued; nothing more to schedule.
  if (index == pending_track_) {
    return true;
  }

  // Track change with something already playing: fade the current track out,
  // then load + fade in the new one when the fade-out completes (0x6544).
  if (module_ != nullptr) {
    pending_track_ = index;
    ramp_mode_ = RampMode::kFadeOut;
    ramp_total_ticks_ = kTrackFadeTicks;
    ramp_remaining_ticks_ = kTrackFadeTicks;
    return true;
  }

  // Cold start with nothing playing: load immediately and fade in.
  if (!LoadAndStart(index, error_out)) {
    return false;
  }
  BeginFadeIn();
  return true;
}

void MusicPlayer::BeginFadeIn() {
  ApplyMikModVolume(0);
  ramp_mode_ = RampMode::kFadeIn;
  ramp_total_ticks_ = kTrackFadeTicks;
  ramp_remaining_ticks_ = kTrackFadeTicks;
}

void MusicPlayer::BeginExitFade(int volume_percent) {
  target_volume_percent_ = ClampPercent(volume_percent);
  exiting_ = true;
  pending_track_ = -1;  // an exit fade cancels any queued track load

  // Nothing playing -> the exit fade is trivially complete and the caller can
  // quit right away.
  if (module_ == nullptr) {
    ramp_mode_ = RampMode::kNone;
    return;
  }

  ramp_mode_ = RampMode::kFadeOut;
  ramp_total_ticks_ = kExitFadeTicks;
  ramp_remaining_ticks_ = kExitFadeTicks;
}

void MusicPlayer::SetVolume(int volume_percent) {
  target_volume_percent_ = ClampPercent(volume_percent);
  if (!initialized_) {
    return;
  }
  // Applies immediately to the currently playing track when idle; while a ramp
  // is active the ramp math already targets target_volume_percent_.
  if (ramp_mode_ == RampMode::kNone) {
    ApplyMikModVolume(target_volume_percent_);
  }
}

void MusicPlayer::ApplyMikModVolume(int volume_percent) {
  if (!initialized_) {
    return;
  }
  Player_SetVolume(static_cast<SWORD>(ToMikModVolume(volume_percent)));
}

int MusicPlayer::RampVolumePercent() const {
  if (ramp_total_ticks_ <= 0) {
    return target_volume_percent_;
  }
  int elapsed = ramp_total_ticks_ - ramp_remaining_ticks_;
  if (elapsed < 0) elapsed = 0;
  if (elapsed > ramp_total_ticks_) elapsed = ramp_total_ticks_;

  // mode 1 climbs 0 -> configured; mode 2 falls configured -> 0, both linear
  // against the configured user music volume, matching the ramp math at 0x6965.
  if (ramp_mode_ == RampMode::kFadeIn) {
    return target_volume_percent_ * elapsed / ramp_total_ticks_;
  }
  return target_volume_percent_ * (ramp_total_ticks_ - elapsed) / ramp_total_ticks_;
}

void MusicPlayer::CompleteRamp() {
  const RampMode finished = ramp_mode_;
  ramp_mode_ = RampMode::kNone;
  ramp_total_ticks_ = 0;
  ramp_remaining_ticks_ = 0;

  if (finished == RampMode::kFadeOut) {
    if (pending_track_ >= 0) {
      const int next = pending_track_;
      pending_track_ = -1;
      if (LoadAndStart(next, nullptr)) {
        BeginFadeIn();
      }
      return;
    }
    if (exiting_) {
      Stop();
      return;
    }
    ApplyMikModVolume(0);
    return;
  }

  // Fade-in finished: settle exactly on the configured volume.
  ApplyMikModVolume(target_volume_percent_);
}

void MusicPlayer::Update() {
  if (!initialized_) {
    return;
  }
  MikMod_Update();

  if (ramp_mode_ == RampMode::kNone) {
    return;
  }
  ApplyMikModVolume(RampVolumePercent());
  if (ramp_remaining_ticks_ > 0) {
    --ramp_remaining_ticks_;
  }
  if (ramp_remaining_ticks_ <= 0) {
    CompleteRamp();
  }
}

float MusicPlayer::exit_fade_progress() const {
  if (!exiting_) {
    return 0.0f;
  }
  if (ramp_total_ticks_ <= 0) {
    return 1.0f;  // nothing was playing -> exit fade is trivially complete
  }
  const int elapsed = ramp_total_ticks_ - ramp_remaining_ticks_;
  if (elapsed <= 0) return 0.0f;
  if (elapsed >= ramp_total_ticks_) return 1.0f;
  return static_cast<float>(elapsed) / static_cast<float>(ramp_total_ticks_);
}

std::string MusicPlayer::debug_state() const {
  const char* ramp = "none";
  if (ramp_mode_ == RampMode::kFadeIn) {
    ramp = "fadein";
  } else if (ramp_mode_ == RampMode::kFadeOut) {
    ramp = "fadeout";
  }
  const int vol = ramp_mode_ == RampMode::kNone ? target_volume_percent_ : RampVolumePercent();
  return " music_track=" + std::to_string(current_track_) +
         " music_ramp=" + ramp +
         " music_ramp_left=" + std::to_string(ramp_remaining_ticks_) +
         " music_vol=" + std::to_string(vol);
}

void MusicPlayer::UnloadModule() {
  if (module_ != nullptr) {
    Player_Free(static_cast<MODULE*>(module_));
    module_ = nullptr;
  }
}

void MusicPlayer::Stop() {
  if (module_ != nullptr) {
    Player_Stop();
  }
  UnloadModule();
  current_track_ = -1;
  ramp_mode_ = RampMode::kNone;
  ramp_total_ticks_ = 0;
  ramp_remaining_ticks_ = 0;
  pending_track_ = -1;
}

void MusicPlayer::Shutdown() {
  if (module_ != nullptr) {
    Player_Stop();
    UnloadModule();
  }
  if (initialized_) {
    MikMod_Exit();
    initialized_ = false;
  }
  current_track_ = -1;
  ramp_mode_ = RampMode::kNone;
  pending_track_ = -1;
  exiting_ = false;
}

}  // namespace atet
