#include "music_player.h"

#include "audio_mixer.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // before mikmod_internals.h, which pulls it in
#endif

#include <cctype>
#include <cstring>

#include <SDL3/SDL.h>
#include <mikmod.h>
#include <mikmod_internals.h>  // UNI_* opcodes and unioperands for the load-time fixups

// MikMod 2.x compatibility switches compiled into libmikmod's player (cmake/libmikmod_mm2_compat.cmake).
extern "C" {
extern int atet_mm2_compat;
extern int atet_mm2_song_volume;
}

namespace atet {
namespace {

int ClampPercent(int value) {
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

// 0x68BB: the MikMod 2.x song volume is percent * 7 / 20 (100 % -> 35) on the player's 0..100
// scale; the player multiplies it into every voice (0x9F1E-0x9F2F).
int ToSongVolume(int percent) { return (ClampPercent(percent) * 7) / 20; }

bool ContainsNoCase(const char* text, const char* needle) {
  if (text == nullptr) return false;
  const size_t n = std::strlen(needle);
  for (const char* p = text; *p != '\0'; ++p) {
    size_t i = 0;
    while (i < n && p[i] != '\0' &&
           std::tolower(static_cast<unsigned char>(p[i])) == std::tolower(static_cast<unsigned char>(needle[i]))) {
      ++i;
    }
    if (i == n) return true;
  }
  return false;
}

}  // namespace

MusicPlayer::~MusicPlayer() { Shutdown(); }

bool MusicPlayer::Initialize(AudioMixer* mixer, const std::array<std::vector<std::uint8_t>, 6>& track_data,
                             std::string* error_out) {
  mixer_ = mixer;
  if (mixer_ == nullptr || !mixer_->is_open()) {
    if (error_out != nullptr) *error_out = "audio mixer is not open";
    return false;
  }
  for (std::size_t i = 0; i < track_data.size(); ++i) {
    track_data_[i] = track_data[i];
  }

  AudioMixer::ScopedLock lock(*mixer_);
  MikMod_RegisterAllLoaders();
  // The only output driver is the port's SDL3 mixer, on every platform.
  MikMod_RegisterDriver(AudioMixer::MikModDriver());
  md_device = 0;
  md_mode = DMODE_SOFT_MUSIC | DMODE_SOFT_SNDFX | DMODE_16BITS | (mixer_->config().stereo ? DMODE_STEREO : 0);
  md_mixfreq = static_cast<UWORD>(mixer_->config().mix_rate);
  atet_mm2_compat = 1;
  atet_mm2_song_volume = 0;

  if (MikMod_Init("")) {
    if (error_out != nullptr) {
      *error_out = std::string("MikMod_Init failed: ") + MikMod_strerror(MikMod_errno);
    }
    initialized_ = false;
    return false;
  }
  MikMod_SetNumVoices(-1, -1);
  MikMod_EnableOutput();
  initialized_ = true;
  return true;
}

void MusicPlayer::ApplyMikMod2xFixups(void* module, int index) {
  MODULE* mod = static_cast<MODULE*>(module);

  // UN05 has no key-off opcode: the 2.x row parser treats (UNI_NOTE, 96) as a key-off (0x92FE),
  // libmikmod plays it as a note and restarts the previous sample. Rewrite it in place to
  // (UNI_KEYFADE, 0), which releases the note (both opcodes take one operand).
  int rewritten = 0;
  for (UWORD t = 0; t < mod->numtrk; ++t) {
    UBYTE* row = mod->tracks != nullptr ? mod->tracks[t] : nullptr;
    if (row == nullptr) continue;
    while (*row != 0) {
      const int length = *row & 0x1f;
      if (length == 0) break;
      UBYTE* op = row + 1;
      UBYTE* const end = row + length;
      while (op < end) {
        const UBYTE opcode = *op;
        if (opcode == 0 || opcode >= UNI_LAST) break;
        if (opcode == UNI_NOTE && op + 1 < end && op[1] == 96) {
          op[0] = UNI_KEYFADE;
          op[1] = 0;
          ++rewritten;
        }
        op += 1 + unioperands[opcode];
      }
      row = end;
    }
  }
  keyoffs_rewritten_ = rewritten;

  // Modules converted from Scream Tracker 3: the 2.x DoS3MVolSlide (0x8D52) applies D0x/Dx0 on
  // every tick including tick 0, which libmikmod does only with UF_S3MSLIDES.
  const bool from_s3m = ContainsNoCase(mod->modtype, "scream") || ContainsNoCase(mod->modtype, "s3m");
  if (from_s3m) {
    mod->flags |= UF_S3MSLIDES;
  }
  SDL_Log("Music: track %d '%s' type '%s' flags 0x%x, %d key-offs rewritten%s", index,
          mod->songname != nullptr ? mod->songname : "", mod->modtype != nullptr ? mod->modtype : "",
          static_cast<unsigned>(mod->flags), rewritten, from_s3m ? ", S3M slides" : "");
}

bool MusicPlayer::LoadAndStart(int index, std::string* error_out) {
  AudioMixer::ScopedLock lock(*mixer_);
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
  ApplyMikMod2xFixups(mod, index);
  mod->wrap = 1;  // restart at the module's repeat position at the end -> loop
  mod->loop = 1;
  module_ = mod;
  current_track_ = index;
  Player_Start(mod);  // with atet_mm2_compat the player then skips two ticks (2.x isfirst)
  module_start_frame_ = mixer_->frames_rendered();
  SDL_Log("Music: playing track %d ('%s')", index, mod->songname != nullptr ? mod->songname : "");
  return true;
}

bool MusicPlayer::RequestTrack(int index, int volume_percent, std::string* error_out) {
  (void)error_out;
  if (!initialized_ || exiting_) {
    return false;
  }
  if (index < 0 || index >= static_cast<int>(track_data_.size())) {
    return false;
  }

  target_volume_percent_ = ClampPercent(volume_percent);

  // A change is in flight (fade-out, then load). The original's 0x6544 blocks the menu until the
  // new track has loaded; the keyboard handler keeps the latest key release, which the menu acts
  // on after the load as a full change of its own (MU-VO-01). So a request made now waits for the
  // load instead of restarting the fade-out or replacing the track being loaded, and the latest
  // request wins. The music then always ends on the last requested track (MU-VP-03).
  if (pending_track_ >= 0) {
    queued_track_ = index;
    return true;
  }

  // Continuity: the requested track is already playing, so it keeps playing untouched. The
  // original never re-applies P after a fade-in; only a volume edit does (SetVolume).
  if (index == current_track_ && module_ != nullptr) {
    return true;
  }

  BeginTrackChange(index);
  return true;
}

void MusicPlayer::BeginTrackChange(int index) {
  // 0x6544: fade out over 0x20 ticks (also when nothing plays yet, as at startup), then load,
  // start and fade in (CompleteRamp). The fade-out's first step applies the full P again.
  pending_track_ = index;
  ramp_mode_ = RampMode::kFadeOut;
  ramp_total_ticks_ = kTrackFadeTicks;
  ramp_remaining_ticks_ = kTrackFadeTicks;
}

void MusicPlayer::BeginFadeIn() {
  ApplyMusicPercent(0);  // 0x6544 mutes (0x68BB(0)) around the load
  ramp_mode_ = RampMode::kFadeIn;
  ramp_total_ticks_ = kTrackFadeTicks;
  ramp_remaining_ticks_ = kTrackFadeTicks;
}

void MusicPlayer::BeginExitFade(int volume_percent) {
  if (exiting_) {
    return;  // state 3 runs once; later input cannot restart or replace it
  }
  target_volume_percent_ = ClampPercent(volume_percent);
  exiting_ = true;
  pending_track_ = -1;  // an exit fade cancels any queued track load
  queued_track_ = -1;

  // Nothing playing -> the exit fade is trivially complete and the caller can quit right away.
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
  // Applies immediately when idle; while a ramp is active the ramp math already scales
  // target_volume_percent_.
  if (ramp_mode_ == RampMode::kNone) {
    ApplyMusicPercent(target_volume_percent_);
  }
}

void MusicPlayer::ApplyMusicPercent(int volume_percent) {
  applied_percent_ = ClampPercent(volume_percent);
  applied_song_volume_ = ToSongVolume(applied_percent_);
  if (!initialized_) {
    return;
  }
  AudioMixer::ScopedLock lock(*mixer_);
  atet_mm2_song_volume = applied_song_volume_;
}

int MusicPlayer::RampVolumePercent() const {
  if (ramp_total_ticks_ <= 0) {
    return target_volume_percent_;
  }
  int elapsed = ramp_total_ticks_ - ramp_remaining_ticks_;
  if (elapsed < 0) elapsed = 0;
  if (elapsed > ramp_total_ticks_) elapsed = ramp_total_ticks_;

  // 0x6965(x) with x = ticks left: fade-in floor((T-x)*P/T) climbs 0 .. (T-1)/T of P; fade-out
  // floor(x*P/T) falls from P to P/T.
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
      // A request made during the change starts its own change now (MU-VO-01: in the original
      // the in-between song is heard for one fade-out that starts at the full volume).
      const int queued = queued_track_;
      queued_track_ = -1;
      if (queued >= 0 && !(queued == current_track_ && module_ != nullptr)) {
        BeginTrackChange(queued);
      }
      return;
    }
    if (exiting_) {
      Stop();
      return;
    }
    ApplyMusicPercent(0);
    return;
  }
  // Fade-in finished: 0x6620 clears the ramp after applying floor(31P/32) and nothing
  // re-applies P, so the music stays at the last ramp value.
}

void MusicPlayer::Update() {
  if (!initialized_) {
    return;
  }
  // No MikMod_Update(): the SDL audio callback (or CaptureMix) drives the mixer and the player.
  if (ramp_mode_ == RampMode::kNone) {
    return;
  }
  ApplyMusicPercent(RampVolumePercent());
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
  return " music_track=" + std::to_string(current_track_) +
         " music_ramp=" + ramp +
         " music_ramp_left=" + std::to_string(ramp_remaining_ticks_) +
         " music_vol=" + std::to_string(applied_percent_) +
         " music_mp=" + std::to_string(applied_song_volume_);
}

size_t MusicPlayer::CaptureMix(std::int16_t* out, size_t frames) {
  if (!initialized_ || !capture_mode_ || out == nullptr || frames == 0) {
    return 0;
  }
  if (mixer_->config().stereo) {
    return mixer_->Render(out, frames);
  }
  // Mono output (SETUP.DAT stereo flag 0): the capture is stereo, so both channels carry it.
  const size_t done = mixer_->Render(out, frames);
  for (size_t i = done; i-- > 0;) {
    out[2 * i + 1] = out[i];
    out[2 * i] = out[i];
  }
  return done;
}

std::string MusicPlayer::AuditJson() const {
  const char* ramp = "none";
  if (ramp_mode_ == RampMode::kFadeIn) {
    ramp = "fadein";
  } else if (ramp_mode_ == RampMode::kFadeOut) {
    ramp = "fadeout";
  }
  std::string json = "{\"track\":" + std::to_string(current_track_) + ",\"pending_track\":" +
                     std::to_string(pending_track_) + ",\"queued_track\":" + std::to_string(queued_track_) +
                     ",\"ramp\":\"" + ramp + "\",\"ramp_left\":" +
                     std::to_string(ramp_remaining_ticks_) + ",\"ramp_total\":" + std::to_string(ramp_total_ticks_) +
                     ",\"volume_percent\":" + std::to_string(applied_percent_) + ",\"target_percent\":" +
                     std::to_string(target_volume_percent_) + ",\"song_volume\":" + std::to_string(applied_song_volume_) +
                     ",\"exiting\":" + (exiting_ ? "true" : "false") +
                     ",\"capture\":" + (capture_mode_ ? "true" : "false") +
                     ",\"keyoffs_rewritten\":" + std::to_string(keyoffs_rewritten_) +
                     ",\"module_start_frame\":" + std::to_string(module_start_frame_);
  if (mixer_ != nullptr && initialized_) {
    AudioMixer::ScopedLock lock(*mixer_);
    const MODULE* mod = static_cast<const MODULE*>(module_);
    if (mod != nullptr) {
      json += ",\"module\":{\"sngpos\":" + std::to_string(mod->sngpos) + ",\"patpos\":" + std::to_string(mod->patpos) +
              ",\"vbtick\":" + std::to_string(mod->vbtick) + ",\"sngspd\":" + std::to_string(mod->sngspd) +
              ",\"bpm\":" + std::to_string(mod->bpm) + ",\"sngtime_1024ths\":" + std::to_string(mod->sngtime) +
              ",\"flags\":" + std::to_string(mod->flags) + ",\"mikmod_volume\":" + std::to_string(mod->volume) + "}";
    } else {
      json += ",\"module\":null";
    }
    json += ",\"mixer\":" + mixer_->AuditJson();
  } else {
    json += ",\"module\":null";
  }
  json += "}";
  return json;
}

void MusicPlayer::UnloadModule() {
  if (module_ != nullptr && mixer_ != nullptr) {
    AudioMixer::ScopedLock lock(*mixer_);
    Player_Stop();
    Player_Free(static_cast<MODULE*>(module_));
    module_ = nullptr;
  }
}

void MusicPlayer::Stop() {
  UnloadModule();
  current_track_ = -1;
  ramp_mode_ = RampMode::kNone;
  ramp_total_ticks_ = 0;
  ramp_remaining_ticks_ = 0;
  pending_track_ = -1;
  queued_track_ = -1;
}

void MusicPlayer::Shutdown() {
  UnloadModule();
  if (initialized_ && mixer_ != nullptr) {
    AudioMixer::ScopedLock lock(*mixer_);
    MikMod_Exit();
    atet_mm2_compat = 0;
  }
  initialized_ = false;
  current_track_ = -1;
  ramp_mode_ = RampMode::kNone;
  pending_track_ = -1;
  queued_track_ = -1;
  exiting_ = false;
}

}  // namespace atet
