#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // provides the platform entry point so the port can be a
                            // GUI-subsystem app (no console window on double-click)

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "atet_data.h"
#include "audio_probe.h"
#include "milestone_a_demo.h"
#include "music_player.h"

namespace {

std::vector<std::uint8_t> ReadFileBytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// Pull the first `count` unsigned integers appearing after `key` in a JSON blob
// (enough to read the palette `colors_8bit` and font `widths` arrays).
std::vector<int> ExtractIntsAfter(const std::string& text, const std::string& key, size_t count) {
  std::vector<int> values;
  const size_t key_pos = text.find(key);
  if (key_pos == std::string::npos) {
    return values;
  }
  size_t i = key_pos + key.size();
  while (values.size() < count && i < text.size()) {
    if (std::isdigit(static_cast<unsigned char>(text[i]))) {
      int value = 0;
      while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        value = value * 10 + (text[i] - '0');
        ++i;
      }
      values.push_back(value);
    } else {
      ++i;
    }
  }
  return values;
}

// Dev/CI check: decode ATET.DAT in-engine and confirm every buffer the port
// loads is byte-identical to the committed (Python-produced) reference asset.
int RunAtetDataVerify(const std::string& dat_path) {
  const std::vector<std::uint8_t> bytes = ReadFileBytes(dat_path);
  if (bytes.empty()) {
    SDL_Log("verify: could not read %s", dat_path.c_str());
    return 2;
  }
  atet::AtetData data;
  std::string error;
  // A .zip path exercises the ACiD Tetris archive path (extract ATET.DAT via
  // miniz); anything else is treated as a loose ATET.DAT.
  const bool is_zip = dat_path.size() >= 4 &&
                      (dat_path.compare(dat_path.size() - 4, 4, ".zip") == 0 ||
                       dat_path.compare(dat_path.size() - 4, 4, ".ZIP") == 0);
  const bool decoded = is_zip ? data.LoadFromZip(bytes, &error) : data.LoadFromDat(bytes, &error);
  if (!decoded) {
    SDL_Log("verify: decode failed: %s", error.c_str());
    return 2;
  }
  SDL_Log("verify: source = %s", is_zip ? "ACiD Tetris archive (zip)" : "loose ATET.DAT");

#ifndef ATET_PORT_REFERENCE_ASSETS
  // Public builds bundle no extracted reference assets to byte-compare against (those
  // stay in the private archive repo), so confirm the archive decoded into the expected
  // set of buffers instead of doing a byte-for-byte comparison.
  SDL_Log("verify: decoded ddd-splash=%zu gameplay=%zu title=%zu piece-atlas=%zu font-atlas=%zu alert-tiles=%d",
          data.ddd_splash().indices.size(), data.gameplay_screen().indices.size(),
          data.title_screen().indices.size(), data.piece_atlas().indices.size(),
          data.font_atlas().indices.size(), data.alert_tile_count());
  const bool decoded_ok = !data.ddd_splash().indices.empty() && !data.gameplay_screen().indices.empty() &&
                          !data.title_screen().indices.empty() && !data.piece_atlas().indices.empty() &&
                          !data.font_atlas().indices.empty() && data.alert_tile_count() > 6;
  SDL_Log("verify: %s (decoded; no bundled reference assets to byte-compare)",
          decoded_ok ? "OK" : "INCOMPLETE");
  return decoded_ok ? 0 : 1;
#else
  int failures = 0;
  auto check_indices = [&](const char* label, const atet::AtetData::Indexed& image, const char* ref_path) {
    const std::vector<std::uint8_t> reference = ReadFileBytes(ref_path);
    const bool ok = image.indices.size() == reference.size() &&
                    std::equal(image.indices.begin(), image.indices.end(), reference.begin());
    SDL_Log("  %s %-22s (%zu vs %zu bytes)", ok ? "PASS" : "FAIL", label, image.indices.size(), reference.size());
    if (!ok) {
      ++failures;
    }
  };
  auto check_blob = [&](const char* label, const std::vector<std::uint8_t>& blob, const char* ref_path) {
    const std::vector<std::uint8_t> reference = ReadFileBytes(ref_path);
    const bool ok = blob.size() == reference.size() && std::equal(blob.begin(), blob.end(), reference.begin());
    SDL_Log("  %s %-22s (%zu vs %zu bytes)", ok ? "PASS" : "FAIL", label, blob.size(), reference.size());
    if (!ok) {
      ++failures;
    }
  };

  check_indices("ddd-splash", data.ddd_splash(), ATET_PORT_DDD_SPLASH_INDEXED_PATH);
  check_indices("gameplay-screen", data.gameplay_screen(), ATET_PORT_GAMEPLAY_SCREEN_INDEXED_PATH);
  check_indices("warning-splash", data.warning_splash(), ATET_PORT_WARNING_SPLASH_INDEXED_PATH);
  check_indices("title-screen", data.title_screen(), ATET_PORT_TITLE_SCREEN_INDEXED_PATH);
  check_indices("piece-atlas", data.piece_atlas(), ATET_PORT_PIECE_ATLAS_INDEXED_PATH);
  check_indices("digit-atlas", data.digit_atlas(), ATET_PORT_DIGIT_ATLAS_INDEXED_PATH);
  check_indices("game-over-overlay", data.game_over_overlay(), ATET_PORT_GAME_OVER_OVERLAY_INDEXED_PATH);
  check_indices("font-atlas", data.font_atlas(), ATET_PORT_FRONTEND_FONT_ATLAS_INDEXED_PATH);
  check_blob("particle-ramps", data.particle_ramps(), ATET_PORT_PARTICLE_RAMP_PATH);
  check_blob("frontend-bankset", data.frontend_bankset(), ATET_PORT_FRONTEND_BANKSET_PATH);

  const std::string tile06_path = std::string(ATET_PORT_ALERT_TILE_PREFIX) + "06" + ATET_PORT_ALERT_TILE_SUFFIX;
  SDL_Log("  alert tiles decoded: %d", data.alert_tile_count());
  if (data.alert_tile_count() > 6) {
    check_indices("alert-tile06", data.alert_tile(6), tile06_path.c_str());
  } else {
    ++failures;
  }

  // Palettes + font widths live in JSON references; compare parsed values.
  auto check_palette = [&](const char* label, const std::array<SDL_Color, 256>& palette, const char* ref_path) {
    std::ifstream stream(ref_path);
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::vector<int> rgb = ExtractIntsAfter(text, "colors_8bit", 768);
    bool ok = rgb.size() == 768;
    for (size_t index = 0; ok && index < 256; ++index) {
      ok = palette[index].r == rgb[index * 3] && palette[index].g == rgb[index * 3 + 1] &&
           palette[index].b == rgb[index * 3 + 2];
    }
    SDL_Log("  %s %-22s", ok ? "PASS" : "FAIL", label);
    if (!ok) {
      ++failures;
    }
  };
  check_palette("gameplay-palette", data.gameplay_palette(), ATET_PORT_GAMEPLAY_PALETTE_PATH);
  check_palette("title-palette", data.title_palette(), ATET_PORT_TITLE_PALETTE_PATH);

  {
    std::ifstream stream(ATET_PORT_FRONTEND_FONT_WIDTHS_PATH);
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::vector<int> widths = ExtractIntsAfter(text, "\"widths\"", data.font_widths().size());
    bool ok = widths.size() == data.font_widths().size();
    for (size_t i = 0; ok && i < widths.size(); ++i) {
      ok = widths[i] == data.font_widths()[i];
    }
    SDL_Log("  %s %-22s (%zu widths)", ok ? "PASS" : "FAIL", "font-widths", data.font_widths().size());
    if (!ok) {
      ++failures;
    }
  }

  SDL_Log("verify: %s (%d failure(s))", failures == 0 ? "ALL ASSETS MATCH" : "MISMATCH", failures);
  return failures == 0 ? 0 : 1;
#endif  // ATET_PORT_REFERENCE_ASSETS
}

std::string VerifyAtetDatPath(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    constexpr const char* kPrefix = "--verify-atet-dat=";
    if (argument.rfind(kPrefix, 0) == 0) {
      return argument.substr(std::string(kPrefix).size());
    }
  }
  return {};
}

constexpr int kLogicalWidth = 320;
constexpr int kLogicalHeight = 240;
constexpr int kWindowScale = 3;

// ATET.DAT chunk ids for the 12-slot SFX table (RE sound-event-mapping-pass:
// startup registers chunks 15,16,23,24,25,26,19,20,21,22,17,18 into slots 0..11).
constexpr std::array<int, 12> kSfxSlotChunkIds = {15, 16, 23, 24, 25, 26, 19, 20, 21, 22, 17, 18};
constexpr int kProbeWavChunkId = 16;

// Recovered tracker music chunk ids, indexed by the in-game track id (0..5),
// matching the Music: menu order Continuum, Tearing Up SpaceTime, Inner Walls
// Released, Costumed, I See It Now, Simple Song.
constexpr std::array<int, 6> kMusicTrackChunkIds = {9, 10, 11, 12, 13, 14};

struct ScriptedInputEvent {
  int frame = 0;
  SDL_Keycode key = SDLK_UNKNOWN;
  bool key_down = true;
};

void LogSdlFailure(const char* context) {
  SDL_Log("%s: %s", context, SDL_GetError());
}

void UpdateWindowTitle(SDL_Window* window, const atet::MilestoneADemo& demo) {
  if (window == nullptr) {
    return;
  }

  std::string title = "ACiD Tetris Port - Milestone C/D - ";
  title += demo.scene_name();
  SDL_SetWindowTitle(window, title.c_str());
}

int ParseSmokeFrames(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    constexpr const char* kPrefix = "--smoke-frames=";
    constexpr size_t kPrefixLength = 15;
    if (argument.rfind(kPrefix, 0) != 0) {
      continue;
    }

    char* end = nullptr;
    const long value = std::strtol(argument.c_str() + kPrefixLength, &end, 10);
    if (end == argument.c_str() + argument.size() && value > 0) {
      return static_cast<int>(value);
    }
  }

  return 0;
}

bool HasSetupModeArgument(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    if (argument == "setup" || argument == "--setup") {
      return true;
    }
  }

  return false;
}

bool HasTopOutDemoArgument(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    if (argument == "--topout-demo") {
      return true;
    }
  }

  return false;
}

bool HasLiveDemoArgument(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    if (argument == "--live-demo") {
      return true;
    }
  }

  return false;
}

bool HasNoFadeArgument(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    if (argument == "--no-fade") {
      return true;
    }
  }

  return false;
}

bool HasDebugStateArgument(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    if (argument == "--debug-state") {
      return true;
    }
  }

  return false;
}

bool HasResetSetupArgument(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    if (argument == "--reset-setup") {
      return true;
    }
  }

  return false;
}

std::uint32_t ParsePieceSeed(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    constexpr const char* kPrefix = "--piece-seed=";
    constexpr size_t kPrefixLength = 13;
    if (argument.rfind(kPrefix, 0) != 0) {
      continue;
    }

    char* end = nullptr;
    const unsigned long value = std::strtoul(argument.c_str() + kPrefixLength, &end, 0);
    if (end == argument.c_str() + argument.size()) {
      return static_cast<std::uint32_t>(value == 0 ? 1 : value);
    }
  }

  return 0;
}

int ParseStartLevelOverride(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    constexpr const char* kPrefix = "--start-level=";
    constexpr size_t kPrefixLength = 14;
    if (argument.rfind(kPrefix, 0) != 0) {
      continue;
    }

    char* end = nullptr;
    const long value = std::strtol(argument.c_str() + kPrefixLength, &end, 10);
    if (end == argument.c_str() + argument.size()) {
      return static_cast<int>(value);
    }
  }

  return -1;
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

SDL_Keycode KeycodeFromName(const std::string& raw_name) {
  const std::string name = LowerAscii(raw_name);
  if (name == "up") {
    return SDLK_UP;
  }
  if (name == "down") {
    return SDLK_DOWN;
  }
  if (name == "left") {
    return SDLK_LEFT;
  }
  if (name == "right") {
    return SDLK_RIGHT;
  }
  if (name == "enter" || name == "return") {
    return SDLK_RETURN;
  }
  if (name == "esc" || name == "escape") {
    return SDLK_ESCAPE;
  }
  if (name == "space") {
    return SDLK_SPACE;
  }
  if (name == "tab") {
    return SDLK_TAB;
  }
  if (name == "backspace") {
    return SDLK_BACKSPACE;
  }
  if (name == "shift") {
    return SDLK_LSHIFT;
  }
  if (name.size() == 1) {
    const char ch = name[0];
    if (ch >= 'a' && ch <= 'z') {
      return SDLK_A + (ch - 'a');
    }
    if (ch >= '0' && ch <= '9') {
      return SDLK_0 + (ch - '0');
    }
  }
  return SDLK_UNKNOWN;
}

std::string ScriptedInputSpec(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    constexpr const char* kPrefix = "--scripted-input=";
    constexpr size_t kPrefixLength = 17;
    if (argument.rfind(kPrefix, 0) == 0) {
      return argument.substr(kPrefixLength);
    }
  }
  return {};
}

std::vector<ScriptedInputEvent> ParseScriptedInput(std::string spec) {
  if (!spec.empty() && spec[0] == '@') {
    std::ifstream input(spec.substr(1));
    if (input) {
      spec.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
  }

  std::vector<ScriptedInputEvent> events;
  std::stringstream stream(spec);
  std::string token;
  while (std::getline(stream, token, ',')) {
    token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char ch) { return std::isspace(ch); }), token.end());
    if (token.empty()) {
      continue;
    }

    const size_t first_separator = token.find(':');
    if (first_separator == std::string::npos) {
      continue;
    }
    const size_t second_separator = token.find(':', first_separator + 1);
    const int frame = std::max(0, std::atoi(token.substr(0, first_separator).c_str()));
    const std::string key_name = token.substr(
        first_separator + 1,
        second_separator == std::string::npos ? std::string::npos : second_separator - first_separator - 1);
    const SDL_Keycode key = KeycodeFromName(key_name);
    if (key == SDLK_UNKNOWN) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Ignoring unknown scripted input key: %s", key_name.c_str());
      continue;
    }
    bool key_down = true;
    if (second_separator != std::string::npos) {
      const std::string action = LowerAscii(token.substr(second_separator + 1));
      key_down = action != "up" && action != "keyup" && action != "release";
    }
    events.push_back(ScriptedInputEvent{frame, key, key_down});
  }

  std::sort(events.begin(), events.end(), [](const ScriptedInputEvent& lhs, const ScriptedInputEvent& rhs) {
    return lhs.frame < rhs.frame;
  });
  return events;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string verify_atet_dat_path = VerifyAtetDatPath(argc, argv);
  if (!verify_atet_dat_path.empty()) {
    return RunAtetDataVerify(verify_atet_dat_path);
  }

  const int smoke_frames = ParseSmokeFrames(argc, argv);
  const std::vector<ScriptedInputEvent> scripted_input = ParseScriptedInput(ScriptedInputSpec(argc, argv));
  size_t scripted_input_index = 0;
  const bool start_in_sound_setup = HasSetupModeArgument(argc, argv);
  const bool start_in_topout_demo = HasTopOutDemoArgument(argc, argv);
  const bool start_in_live_demo = HasLiveDemoArgument(argc, argv);
  const bool debug_state = HasDebugStateArgument(argc, argv);
  const bool reset_setup = HasResetSetupArgument(argc, argv);
  const std::uint32_t piece_seed = ParsePieceSeed(argc, argv);
  const int start_level_override = ParseStartLevelOverride(argc, argv);

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
    LogSdlFailure("SDL_Init failed");
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow(
      "ACiD Tetris Port Skeleton",
      kLogicalWidth * kWindowScale,
      kLogicalHeight * kWindowScale,
      SDL_WINDOW_RESIZABLE);
  if (window == nullptr) {
    LogSdlFailure("SDL_CreateWindow failed");
    SDL_Quit();
    return 1;
  }

  if (!SDL_SetWindowMinimumSize(window, kLogicalWidth, kLogicalHeight)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_SetWindowMinimumSize failed: %s", SDL_GetError());
  }

  SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
  if (renderer == nullptr) {
    LogSdlFailure("SDL_CreateRenderer failed");
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  if (!SDL_SetRenderLogicalPresentation(
          renderer,
          kLogicalWidth,
          kLogicalHeight,
          SDL_LOGICAL_PRESENTATION_INTEGER_SCALE)) {
    SDL_LogWarn(
        SDL_LOG_CATEGORY_APPLICATION,
        "SDL_SetRenderLogicalPresentation failed: %s",
        SDL_GetError());
  }

  atet::MilestoneADemo demo;
  std::string demo_error;
  if (!demo.Initialize(renderer, &demo_error, start_in_sound_setup)) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Milestone B asset load failed: %s", demo_error.c_str());
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  if (reset_setup) {
    demo.ResetBuildLocalSetupState();
  }
  demo.set_debug_state_logging(debug_state);
  // Screen fades are on for normal play but disabled for --no-fade and for the
  // --live-demo/--topout-demo shortcuts (which jump straight into gameplay and
  // must stay frame-deterministic for smoke runs).
  demo.set_fades_enabled(!(HasNoFadeArgument(argc, argv) || start_in_topout_demo || start_in_live_demo));
  if (piece_seed != 0) {
    demo.set_piece_seed(piece_seed);
  }
  if (start_level_override >= 0) {
    demo.set_start_level_override(start_level_override);
  }
  if (start_in_topout_demo) {
    demo.BeginTopOutDemo(17500, 36);
  } else if (start_in_live_demo) {
    demo.BeginLiveGameplayDemo(4);
  }
  UpdateWindowTitle(window, demo);

  // Route the SFX + music chunks decoded from the user's own ATET.DAT into the
  // players -- no audio/music asset files are shipped.
  const atet::AtetData& atet_data = demo.atet_data();
  std::array<std::vector<Uint8>, 12> sfx_slot_bytes;
  for (size_t slot = 0; slot < sfx_slot_bytes.size(); ++slot) {
    sfx_slot_bytes[slot] = atet_data.chunk(kSfxSlotChunkIds[slot]);
  }
  std::array<std::vector<Uint8>, 6> music_track_bytes;
  for (size_t track = 0; track < music_track_bytes.size(); ++track) {
    music_track_bytes[track] = atet_data.chunk(kMusicTrackChunkIds[track]);
  }

  atet::AudioProbe audio_probe;
  std::string audio_error;
  const bool audio_ready = audio_probe.Initialize(atet_data.chunk(kProbeWavChunkId), &audio_error);
  if (!audio_ready) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Audio probe unavailable: %s", audio_error.c_str());
  } else {
    SDL_Log("Loaded probe WAV from ATET.DAT chunk %d", kProbeWavChunkId);
  }
  const bool sfx_ready = audio_probe.InitializeSfxSlots(sfx_slot_bytes, &audio_error);
  if (!sfx_ready) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Recovered SFX slots unavailable: %s", audio_error.c_str());
  } else {
    SDL_Log("Loaded %d recovered SFX slots.", audio_probe.loaded_sfx_slots());
  }
  atet::MusicPlayer music_player;
  const bool music_ready = music_player.Initialize(music_track_bytes, &audio_error);
  if (!music_ready) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Music player unavailable: %s", audio_error.c_str());
  } else {
    SDL_Log("Music player ready (libmikmod tracker playback).");
  }
  demo.set_audio_event_callback(
      [&audio_probe, &music_player, &audio_error](const atet::MilestoneADemo::AudioEventNotification& event) {
        if (event.is_sfx) {
          if (!audio_probe.sfx_ready()) {
            return;
          }
          // Observe the realized 0x6817 parameters: the engine volume curve
          // (percent -> 0..0x40), the per-play pan, and the coarse voice class.
          SDL_Log(
              "SFX mix: slot=%d vol%%=%d engine=%d/%d pan=%d class=%d",
              event.slot,
              event.sfx_volume,
              atet::AudioProbe::EngineVolume(event.sfx_volume),
              atet::AudioProbe::kEngineVolumeMax,
              event.pan,
              event.voice_class);
          if (!audio_probe.PlaySfxSlot(event.slot, event.sfx_volume, event.pan, &audio_error)) {
            const std::string event_name(event.name);
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Recovered SFX playback failed for %s slot %d: %s",
                event_name.c_str(),
                event.slot,
                audio_error.c_str());
          }
          return;
        }

        // Music event. Route it to the tracker player.
        if (!music_player.available()) {
          return;
        }
        if (event.name == "music-volume-apply") {
          music_player.SetVolume(event.music_volume);
        } else if (event.name == "exit-fade") {
          // Fade the music out over the original's 0x40-tick exit ramp; the
          // main loop keeps pumping Update() until the fade finishes.
          music_player.BeginExitFade(event.music_volume);
        } else {
          // Startup, explicit track-change, and continuity events all mean
          // "ensure the selected track is playing." RequestTrack keeps the
          // current track running untouched when the index has not changed and
          // otherwise runs the original fade-out/load/fade-in track sequence.
          if (!music_player.RequestTrack(event.track, event.music_volume, &audio_error)) {
            const std::string event_name(event.name);
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Music playback failed for %s track %d: %s",
                event_name.c_str(),
                event.track,
                audio_error.c_str());
          }
        }
      });
  SDL_Log(
      "Milestone C/D controls: arrows move frontend rows, held Left/Right repeats editable values, "
      "Enter activates rows, Space plays the probe WAV, Tab toggles frontend/gameplay, 1-3 or rebound "
      "gameplay keys select gameplay overlay presets, C injects filled rows for live line-clear smoke, "
      "O runs the top-out presentation demo, recovered SFX slots play for routed events, "
      "H opens the reveal-gated high-score entry shell "
      "from gameplay, Shift toggles name-entry case, Enter/Space/Esc can advance startup splashes, "
      "Esc backs out or quits from the main menu.");

  bool running = true;
  int rendered_smoke_frames = 0;
  // Precise fixed-RATE loop. The game advances one logic tick + one render per frame,
  // and EVERY per-frame constant -- gameplay (gravity, soft-drop 0x8000/frame, 8-frame
  // DAS) AND presentation (the 0x20/frame menu pulse, the 1-row/frame top-out dissolve,
  // the palette fades) -- runs at this one rate, so it sets the whole game's real-time
  // speed. We pace the WHOLE loop to it off the high-resolution counter (sleep most of
  // the frame, busy-wait the last ~1ms for precision, since SDL_Delay rounds up to the
  // OS timer granularity ~= 15.6ms on Windows). Smoke runs skip the limiter (frame-
  // counted, deterministic).
  //
  // RATE = 60 Hz. RE 0x6c62 calibrates the tick to the VGA mode-13h vertical retrace,
  // which is ~70 Hz on bare metal, and an earlier pass paced the port there -- but that
  // finding flagged it needed a runtime check, and a side-by-side capture against the
  // running game settles it: the reference plays the fixed 64-frame menu pulse in
  // ~1.07s (== 60 Hz), and 70 Hz ran the whole port ~1.19x (70/60) too fast (pulse,
  // particles, everything). Matching the running game's real-time speed => 60 Hz. This
  // is wall-clock only; all frame-COUNT fingerprints (RNG, smokes, dynamic tests) are
  // unchanged since they count ticks, not seconds.
  const double kSimHz = 60.0;
  const Uint64 perf_freq = SDL_GetPerformanceFrequency();
  const Uint64 frame_ticks = static_cast<Uint64>(static_cast<double>(perf_freq) / kSimHz);
  Uint64 next_frame_deadline = SDL_GetPerformanceCounter();
  while (running) {
    while (scripted_input_index < scripted_input.size() &&
           scripted_input[scripted_input_index].frame == rendered_smoke_frames) {
      const ScriptedInputEvent& scripted = scripted_input[scripted_input_index++];
      if (scripted.key_down) {
        if (scripted.key == SDLK_ESCAPE) {
          demo.CaptureGameplaySnapshot(renderer);
        }
        if (demo.HandleKeyDown(scripted.key)) {
          UpdateWindowTitle(window, demo);
          // Quit is deferred until after the music exit fade (see below).
        }
      } else {
        demo.HandleKeyUp(scripted.key);
      }
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_EVENT_QUIT:
          running = false;
          break;

        case SDL_EVENT_KEY_DOWN:
          if (event.key.repeat) {
            break;
          }

          if (event.key.key == SDLK_ESCAPE) {
            demo.CaptureGameplaySnapshot(renderer);
          }
          if (demo.HandleKeyDown(event.key.key)) {
            UpdateWindowTitle(window, demo);
            SDL_Log("Scene: %s | gameplay overlay preset: %d", demo.scene_name(), demo.current_preset_index() + 1);
            // Quit is deferred until after the music exit fade (see below).
          } else if (event.key.key == SDLK_SPACE) {
            if (!audio_ready) {
              SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Audio probe is not ready.");
            } else if (!audio_probe.Play(&audio_error)) {
              SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Probe playback failed: %s", audio_error.c_str());
            }
          }
          break;

        case SDL_EVENT_KEY_UP:
          demo.HandleKeyUp(event.key.key);
          break;

        default:
          break;
      }
    }

    demo.Tick();
    if (debug_state) {
      SDL_Log("%s%s", demo.debug_state_line().c_str(), music_player.debug_state().c_str());
    }
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    demo.Render(renderer);
    // Screen fade-to-black overlay: the demo's gameplay<->frontend / splash
    // fades (0x3830 palette fades) plus the Exit Game music-synced exit fade
    // (state-3). Whichever is darker wins.
    int fade_alpha = demo.screen_fade_alpha();
    if (music_player.exit_fade_active()) {
      fade_alpha = std::max(fade_alpha, std::clamp(static_cast<int>(music_player.exit_fade_progress() * 255.0f), 0, 255));
    }
    if (fade_alpha > 0) {
      SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, static_cast<Uint8>(fade_alpha));
      const SDL_FRect fade_rect = {0.0f, 0.0f, static_cast<float>(kLogicalWidth), static_cast<float>(kLogicalHeight)};
      SDL_RenderFillRect(renderer, &fade_rect);
      SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }
    UpdateWindowTitle(window, demo);
    SDL_RenderPresent(renderer);
    music_player.Update();
    // Defer the actual quit until any scheduled exit-music fade has finished so
    // the exit-to-DOS path plays its fade-out before the window closes. Quit
    // paths that schedule no fade (Esc, sound-setup Exit to Dos) stop at once,
    // since exit_fade_active() is false for them.
    if (demo.quit_requested() && !music_player.exit_fade_active()) {
      running = false;
    }
    if (smoke_frames > 0) {
      ++rendered_smoke_frames;
      if (rendered_smoke_frames >= smoke_frames) {
        running = false;
      }
    }
    // Pace the loop to exactly kSimHz (skip for frame-counted smoke runs). Sleep the
    // bulk of the remaining frame time, then busy-wait the final ~1ms so we hit the
    // deadline precisely rather than overshooting to the OS timer granularity.
    if (smoke_frames == 0) {
      next_frame_deadline += frame_ticks;
      Uint64 now = SDL_GetPerformanceCounter();
      if (now < next_frame_deadline) {
        const Uint64 remaining = next_frame_deadline - now;
        const Uint64 spin_margin = perf_freq / 1000;  // ~1ms busy-wait tail
        if (remaining > spin_margin) {
          SDL_Delay(static_cast<Uint32>((remaining - spin_margin) * 1000 / perf_freq));
        }
        while (SDL_GetPerformanceCounter() < next_frame_deadline) { /* spin to deadline */ }
      } else {
        next_frame_deadline = now;  // behind schedule; reset rather than accumulate debt
      }
    }
  }

  music_player.Shutdown();
  audio_probe.Shutdown();
  demo.Shutdown();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
