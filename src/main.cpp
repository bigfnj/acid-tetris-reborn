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
#include "frame_pacer.h"
#include "milestone_a_demo.h"
#include "music_player.h"
#include "audit_instrument.h"  // audit/instrument worktree only

#include <cmath>
#include <cstdio>

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

bool HasFlagArgument(int argc, char** argv, const char* flag) {
  for (int index = 1; index < argc; ++index) {
    if (argv[index] != nullptr && std::string(argv[index]) == flag) {
      return true;
    }
  }
  return false;
}

// --name=N (integer); `fallback` when absent or malformed.
long ParseIntArgument(int argc, char** argv, const char* prefix, long fallback) {
  const size_t prefix_length = std::char_traits<char>::length(prefix);
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    if (argument.rfind(prefix, 0) != 0) {
      continue;
    }
    char* end = nullptr;
    const long value = std::strtol(argument.c_str() + prefix_length, &end, 10);
    if (end == argument.c_str() + argument.size() && end != argument.c_str() + prefix_length) {
      return value;
    }
  }
  return fallback;
}

// Absolute PIT tick source for the main loop: a virtual tick (frame-exact, deterministic) until
// SwitchToRealTime(), then the performance counter, continuing from the virtual tick reached.
class GameClock final : public atet::TickClock {
 public:
  explicit GameClock(Uint64 freq) : freq_(freq == 0 ? 1 : freq) {}

  void SwitchToRealTime() {
    offset_ = virtual_.Now();
    origin_ = SDL_GetPerformanceCounter();
    real_time_ = true;
  }
  [[nodiscard]] bool real_time() const { return real_time_; }

  std::uint64_t Now() override {
    if (!real_time_) {
      return virtual_.Now();
    }
    return offset_ + atet::PitTicksForCounts(SDL_GetPerformanceCounter() - origin_, freq_);
  }

  // Sleep to about 1 ms before the next tick edge (SDL_Delay rounds to the OS timer), then spin.
  void WaitForTickAfter(std::uint64_t tick) override {
    last_sleep_request_ms_ = 0.0;
    last_sleep_us_ = 0.0;
    last_spin_us_ = 0.0;
    last_slack_us_ = 0.0;
    if (!real_time_) {
      virtual_.WaitForTickAfter(tick);
      return;
    }
    if (tick + 1 <= offset_) {
      return;
    }
    const Uint64 target = origin_ + atet::PitCountsForTicks(tick + 1 - offset_, freq_);
    Uint64 now = SDL_GetPerformanceCounter();
    last_slack_us_ = now <= target ? ToUs(target - now) : -ToUs(now - target);
    const Uint64 spin_margin = freq_ / 1000;
    if (now < target && target - now > spin_margin) {
      const Uint32 delay_ms = static_cast<Uint32>((target - now - spin_margin) * 1000 / freq_);
      if (delay_ms > 0) {
        last_sleep_request_ms_ = delay_ms;
        SDL_Delay(delay_ms);
        const Uint64 after = SDL_GetPerformanceCounter();
        last_sleep_us_ = ToUs(after - now);
        now = after;
      }
    }
    const Uint64 spin_start = now;
    while (now < target) {
      now = SDL_GetPerformanceCounter();
    }
    last_spin_us_ = ToUs(now - spin_start);
  }

  // Smoke runs: the frame's work took `ticks` whole ticks (--smoke-frame-ticks minus one).
  void SimulateFrameWork(std::uint64_t ticks) {
    if (!real_time_) {
      virtual_.SimulateWork(ticks);
    }
  }

  [[nodiscard]] double last_sleep_request_ms() const { return last_sleep_request_ms_; }
  [[nodiscard]] double last_sleep_us() const { return last_sleep_us_; }
  [[nodiscard]] double last_spin_us() const { return last_spin_us_; }
  [[nodiscard]] double last_slack_us() const { return last_slack_us_; }

 private:
  double ToUs(Uint64 counts) const { return static_cast<double>(counts) * 1.0e6 / static_cast<double>(freq_); }

  Uint64 freq_ = 1;
  bool real_time_ = false;
  atet::VirtualTickClock virtual_;
  std::uint64_t offset_ = 0;
  Uint64 origin_ = 0;
  double last_sleep_request_ms_ = 0.0;
  double last_sleep_us_ = 0.0;
  double last_spin_us_ = 0.0;
  double last_slack_us_ = 0.0;
};

}  // namespace

int main(int argc, char** argv) {
  // ---- audit instrumentation: startup clock + options -----------------------
  const Uint64 audit_t_main = SDL_GetPerformanceCounter();
  const atet::ProcessStats audit_stats_at_main = atet::QueryProcessStats();
  std::string audit_error;
  const atet::AuditOptions audit = atet::ParseAuditOptions(argc, argv, &audit_error);
  if (!audit_error.empty()) {
    std::fprintf(stderr, "Audit option error: %s\n", audit_error.c_str());
    return 2;
  }

  const std::string verify_atet_dat_path = VerifyAtetDatPath(argc, argv);
  if (!verify_atet_dat_path.empty()) {
    return RunAtetDataVerify(verify_atet_dat_path);
  }
  // Dev/CI check: fake-clock unit tests of the tick arithmetic and the frame pacer.
  if (HasFlagArgument(argc, argv, "--selftest-pacer")) {
    return atet::RunFramePacerSelfTest() == 0 ? 0 : 1;
  }

  const int smoke_frames = ParseSmokeFrames(argc, argv);
  // Smoke runs: ticks each frame takes on the virtual clock (1 = every frame lasts one tick; 2..6
  // run that many logic steps per frame, 7+ also drop ticks, like a machine that slow).
  const int smoke_frame_ticks = static_cast<int>(std::clamp(ParseIntArgument(argc, argv, "--smoke-frame-ticks=", 1), 1L, 600L));
  // Dev check: draw every frame twice and fail if the second Render changed any step counter.
  const bool check_render_purity = HasFlagArgument(argc, argv, "--check-render-purity");
  if (smoke_frames > 0 && audit.paced_frames > 0) {
    std::fprintf(stderr, "--smoke-frames and --paced-frames are mutually exclusive.\n");
    return 2;
  }
  const std::vector<ScriptedInputEvent> scripted_input = ParseScriptedInput(ScriptedInputSpec(argc, argv));
  size_t scripted_input_index = 0;
  const bool start_in_sound_setup = HasSetupModeArgument(argc, argv);
  const bool start_in_topout_demo = HasTopOutDemoArgument(argc, argv);
  const bool start_in_live_demo = HasLiveDemoArgument(argc, argv);
  const bool debug_state = HasDebugStateArgument(argc, argv);
  const bool reset_setup = HasResetSetupArgument(argc, argv);
  const std::uint32_t piece_seed = ParsePieceSeed(argc, argv);
  const int start_level_override = ParseStartLevelOverride(argc, argv);

  if (audit.no_activate) {
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
  }
  if (audit.quiet_log) {
    SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_WARN);
  }

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
    LogSdlFailure("SDL_Init failed");
    return 1;
  }
  const Uint64 audit_t_sdl_init = SDL_GetPerformanceCounter();

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
  const Uint64 audit_t_window = SDL_GetPerformanceCounter();
  // Presentation: vsync on by default for real-time play (--vsync=0 turns it off). The original
  // triple-buffers and never waits for the display; the port gets the same property from the pacer
  // order: it presents first and then waits for the next tick edge on the performance counter, so a
  // present that blocks for a vblank only shortens the tick wait, and a frame that crosses an extra
  // tick edge just runs 2 steps next time. With vsync every frame is shown exactly once; the
  // 60.0011 Hz logic against a 59.97 Hz panel then costs one 2-step frame per beat (about 36 s)
  // instead of bursts of dropped and repeated frames. Smoke runs default to off: the software
  // renderer would simulate vsync with a sleep per present.
  const int vsync = static_cast<int>(ParseIntArgument(argc, argv, "--vsync=", smoke_frames > 0 ? 0 : 1));
  if (!SDL_SetRenderVSync(renderer, vsync)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SDL_SetRenderVSync(%d) failed: %s", vsync, SDL_GetError());
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
  const Uint64 audit_t_demo = SDL_GetPerformanceCounter();
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
  const bool audio_capture_requested = !audit.audio_path.empty();
  if (audio_capture_requested) {
    music_player.SetCaptureMode(true);  // nosound VC mixer, mixed on demand per step
  }
  const bool music_ready = music_player.Initialize(music_track_bytes, &audio_error);
  if (!music_ready) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Music player unavailable: %s", audio_error.c_str());
  } else {
    SDL_Log("Music player ready (libmikmod tracker playback).");
  }
  const Uint64 audit_t_audio = SDL_GetPerformanceCounter();

  // ---- audit: offline audio capture state -----------------------------------
  constexpr int kCaptureRate = 44100;
  constexpr size_t kFramesPerStep = 735;  // 44100 / 60: one simulation step of virtual time
  constexpr size_t kWinmmBufferFrames = 44100 * 120 / 1000;  // drv_win.c DEFAULT_BUFFERSIZE_MS
  constexpr size_t kWinmmBuffers = 2;                         // drv_win.c DEFAULT_NUMBUFFERS
  atet::OfflineSfxMixer sfx_mixer;
  bool capturing = false;
  if (audio_capture_requested) {
    std::string mixer_error;
    if (!sfx_mixer.Load(sfx_slot_bytes, kCaptureRate, &mixer_error)) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Audit: offline SFX mixer failed: %s", mixer_error.c_str());
    }
    capturing = music_ready;
    if (!capturing) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Audit: --dump-audio requested but music player is unavailable");
    }
  }
  std::vector<float> music_timeline;  // interleaved stereo float, absolute output frames
  std::vector<float> sfx_timeline;
  std::vector<std::int16_t> capture_scratch;
  std::vector<std::uint64_t> winmm_queue_ends;
  std::uint64_t winmm_queue_end = 0;
  std::uint64_t winmm_underrun_frames = 0;

  struct DeliveredEvent {
    std::string name;
    bool is_sfx = false;
    int slot = 0;
    int track = 0;
    int music_volume = 0;
    int sfx_volume = 0;
    int pan = 0x80;
    int voice_class = 0;
  };
  std::vector<DeliveredEvent> delivered_events;

  demo.set_audio_event_callback(
      [&audio_probe, &music_player, &audio_error, &delivered_events, &sfx_mixer, &capturing](
          const atet::MilestoneADemo::AudioEventNotification& event) {
        delivered_events.push_back(DeliveredEvent{std::string(event.name), event.is_sfx, event.slot, event.track,
                                                  event.music_volume, event.sfx_volume, event.pan,
                                                  event.voice_class});
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
          if (capturing) {
            sfx_mixer.Trigger(event.slot, event.sfx_volume, event.pan);  // offline, no live playback
            return;
          }
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

  // ---- audit: report + output files -------------------------------------------
  {
    int vsync_now = -99;
    SDL_GetRenderVSync(renderer, &vsync_now);
    std::fprintf(stderr, "Audit: renderer=%s vsync=%d video=%s audio=%s data=%s\n", SDL_GetRendererName(renderer),
                 vsync_now, SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
                 SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
                 atet::g_audit_startup.data_source.c_str());
  }
  if (audit.report_displays) {
    atet::ReportDisplays(window, renderer);
  }
  std::FILE* trace_file = nullptr;
  if (!audit.trace_path.empty()) {
    trace_file = std::fopen(audit.trace_path.c_str(), "wb");
    if (trace_file == nullptr) {
      std::fprintf(stderr, "Audit: cannot open trace file %s\n", audit.trace_path.c_str());
      return 1;
    }
  }
  std::FILE* perf_file = nullptr;
  if (!audit.perf_path.empty()) {
    perf_file = std::fopen(audit.perf_path.c_str(), "wb");
    if (perf_file == nullptr) {
      std::fprintf(stderr, "Audit: cannot open perf file %s\n", audit.perf_path.c_str());
      return 1;
    }
    std::fputs(
        "step,scene,input_us,tick_us,render_us,overlay_dump_us,title_us,present_us,music_us,capture_trace_us,"
        "work_us,sleep_req_ms,sleep_actual_us,spin_us,slack_us,behind_reset,present_interval_us,"
        "texture_calls,fillrect_calls,color_calls,pointfield_points,glyph_rows,debris_drawn,debris_active,"
        "pointfield_us,rows_us,debris_render_us,debris_step_us,topout_update_us,lineclear_seed_us,"
        "t0_us,present_end_us,dxgi_ok,dxgi_last_present,dxgi_present_count,dxgi_present_refresh,dxgi_sync_refresh,"
        "dxgi_sync_qpc_us,tick,frame_kind,steps_run,pacer_elapsed,logic_steps_total\n",
        perf_file);
  }
  if (!audit.dump_dir.empty()) {
    SDL_CreateDirectory(audit.dump_dir.c_str());
  }
  SDL_Texture* dump_target = nullptr;

  bool running = true;
  int rendered_smoke_frames = 0;
  // ---- Game clock and frame pacing ------------------------------------------------------------
  // The original keeps one clock: PIT channel 0, programmed once at startup with divisor 19886,
  // ticks at 1,193,181.818 Hz / 19,886 = 60.00109716 Hz, and every game speed, fade, menu
  // animation, gravity and auto-repeat is a whole number of those ticks. Each interactive screen
  // runs n logic steps, draws once and calls the pacer, which flips first and then waits for the
  // next tick edge; n is the ticks since the previous flip, capped at 6 (atet::FramePacer). Splash
  // pages and the gameplay palette fades are busy tick loops the pacer never sees
  // (MilestoneADemo::InBlockingPhase): they run one step per elapsed tick and leave the pacer
  // baseline alone, so the next pacer return is 6, as in the original. Nothing waits for the
  // display: the original never reads port 0x3DA, its 59.713 Hz Mode X refresh just shows the last
  // flipped page. Real-time runs derive the tick from the performance counter with exact rational
  // arithmetic (no drift); --smoke-frames runs and the --fast-forward prefix use a virtual tick, so
  // they stay frame-exact and deterministic (every frame takes --smoke-frame-ticks ticks, default 1).
  const Uint64 perf_freq = SDL_GetPerformanceFrequency();
  auto ticks_to_us = [perf_freq](Uint64 ticks) {
    return static_cast<double>(ticks) * 1.0e6 / static_cast<double>(perf_freq);
  };
  GameClock clock(perf_freq);
  atet::FramePacer pacer(clock);
  const std::uint64_t smoke_work_ticks = static_cast<std::uint64_t>(smoke_frame_ticks - 1);
  constexpr std::uint64_t kMaxTimerCatchUp = 128;      // > the longest music ramp (64 ticks)
  constexpr std::uint64_t kMaxBlockingSteps = 3600;    // a tick loop never needs more in one go
  int steps = 1;                          // the pacer's last return: the next pacer frame's step count
  bool blocking_clock_valid = false;      // the previous iteration was a tick-loop frame of this phase
  std::uint64_t blocking_last_tick = 0;   // tick at which that frame's steps were due
  std::uint64_t serviced_tick = 0;        // timer-service (music) position
  std::uint64_t logic_steps_total = 0;
  std::uint64_t ticks_dropped_total = 0;
  std::uint64_t purity_checks = 0;
  int purity_failures = 0;
  const atet::ProcessStats audit_stats_loop_start = atet::QueryProcessStats();
  const Uint64 audit_loop_start = SDL_GetPerformanceCounter();
  if (smoke_frames == 0 && audit.fast_forward == 0) {
    clock.SwitchToRealTime();  // tick 0 = the start of the boot sequence (the PIT install point)
  }
  std::uint64_t step = 0;
  Uint64 previous_present = 0;
  Uint64 first_present = 0;
  std::vector<double> present_intervals_us;
  std::vector<double> work_us_samples;
  int behind_resets = 0;
  Uint64 paced_phase_start = 0;
  std::uint64_t paced_phase_start_step = 0;
  std::uint64_t paced_phase_start_logic = 0;
  std::uint64_t paced_phase_start_tick = 0;
  atet::ProcessStats stats_paced_start;
  double total_spin_us = 0.0;
  double total_sleep_us = 0.0;
  std::array<std::uint64_t, 8> frames_by_steps{};
  atet::JsonOut pre_json;
  atet::JsonOut json;
  struct InjectedKey {
    std::string source;
    std::string key;
    bool down = true;
  };
  std::vector<InjectedKey> injected_keys;

  // The original's INT 8 handler (0x6e51 -> 0x6620) runs once per tick whatever the game loop is
  // doing: one music volume-ramp step and one sound-driver update. MusicPlayer::Update() is that
  // pair, so it runs once per elapsed tick, not once per presented frame.
  auto service_timer = [&]() {
    const std::uint64_t now_tick = clock.Now();
    std::uint64_t due = now_tick > serviced_tick ? now_tick - serviced_tick : 0;
    serviced_tick = now_tick;
    due = std::min(due, kMaxTimerCatchUp);
    demo.AuditSetStage("music");
    for (; due > 0; --due) {
      music_player.Update();
    }
  };

  while (running) {
    const Uint64 t0 = SDL_GetPerformanceCounter();
    atet::g_audit.Reset();
    demo.AuditBeginStep(step);
    demo.AuditSetStage("input");
    delivered_events.clear();
    injected_keys.clear();

    if (smoke_frames == 0 && step == audit.fast_forward) {
      if (audit.fast_forward > 0) {
        clock.SwitchToRealTime();  // the virtual prefix ran unpaced; pace from here
        paced_phase_start = SDL_GetPerformanceCounter();
        stats_paced_start = atet::QueryProcessStats();
      } else {
        paced_phase_start = audit_loop_start;
        stats_paced_start = audit_stats_loop_start;
      }
      paced_phase_start_step = step;
      paced_phase_start_logic = logic_steps_total;
      paced_phase_start_tick = clock.Now();
    }
    const std::uint64_t tick_at_start = clock.Now();

    // Per-iteration diagnostics (audit perf/trace).
    double input_us = 0.0, tick_us = 0.0, render_us = 0.0, overlay_dump_us = 0.0, title_us = 0.0;
    double present_us = 0.0, music_us = 0.0;
    double sleep_req_ms = 0.0, sleep_actual_us = 0.0, spin_us = 0.0, slack_us = 0.0;
    int fade_alpha = 0;
    bool dump_this = false;
    bool frame_ok = false;
    std::string frame_file;
    std::uint64_t frame_hash = atet::kFnvOffsetBasis;
    int frame_w = 0;
    int frame_h = 0;
    atet::DxgiPresentStats dxgi{};
    double present_interval_us = -1.0;
    Uint64 t_present_end = 0;
    const char* frame_kind = "pacer";
    int steps_planned = 0;
    int steps_run = 0;
    bool reset_this_frame = false;
    std::uint64_t pacer_elapsed = 0;

    auto record_wait = [&]() {
      sleep_req_ms += clock.last_sleep_request_ms();
      sleep_actual_us += clock.last_sleep_us();
      spin_us += clock.last_spin_us();
      slack_us = clock.last_slack_us();
    };

    auto poll_input = [&]() {
      const Uint64 i0 = SDL_GetPerformanceCounter();
      demo.AuditSetStage("input");
      // Scripted input is keyed to the loop iteration (one presented frame), which advances in
      // BOTH smoke and paced modes in this audit build. The shipped main.cpp keys it to
      // rendered_smoke_frames, which only advances under --smoke-frames.
      const std::uint64_t scripted_key =
          audit.legacy_scripted_input ? static_cast<std::uint64_t>(rendered_smoke_frames) : step;
      while (scripted_input_index < scripted_input.size() &&
             static_cast<std::uint64_t>(scripted_input[scripted_input_index].frame) == scripted_key) {
        const ScriptedInputEvent& scripted = scripted_input[scripted_input_index++];
        injected_keys.push_back(InjectedKey{"script", SDL_GetKeyName(scripted.key), scripted.key_down});
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
            injected_keys.push_back(InjectedKey{"host", SDL_GetKeyName(event.key.key), true});

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
            injected_keys.push_back(InjectedKey{"host", SDL_GetKeyName(event.key.key), false});
            demo.HandleKeyUp(event.key.key);
            break;

          default:
            break;
        }
      }
      input_us += ticks_to_us(SDL_GetPerformanceCounter() - i0);
    };

    // The frame's single draw: between the last step's TickLogic and its TickPresentation.
    auto render_frame = [&]() {
      if (debug_state) {
        // tick = the frame's tick, steps = logic steps run for this frame, loop = tick (a tick-loop
        // phase: splash page or gameplay palette fade) or paced (a pacer frame).
        SDL_Log("%s%s tick=%llu steps=%d loop=%s", demo.debug_state_line().c_str(), music_player.debug_state().c_str(),
                static_cast<unsigned long long>(clock.Now()), steps_run, frame_kind[0] == 'b' ? "tick" : "paced");
      }
      if (trace_file != nullptr) {
        pre_json.Clear();
        demo.AuditWritePresentation(pre_json);
      }
      demo.AuditSetStage("render");
      dump_this = !audit.dump_dir.empty() && step >= audit.dump_from && step <= audit.dump_to;
      if (dump_this && !audit.dump_window_mode) {
        if (dump_target == nullptr) {
          dump_target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, kLogicalWidth,
                                          kLogicalHeight);
          if (dump_target != nullptr) {
            SDL_SetTextureScaleMode(dump_target, SDL_SCALEMODE_NEAREST);
            SDL_SetTextureBlendMode(dump_target, SDL_BLENDMODE_NONE);
          } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Audit: cannot create dump target: %s", SDL_GetError());
          }
        }
        if (dump_target != nullptr) {
          SDL_SetRenderTarget(renderer, dump_target);
        }
      }
      const Uint64 r0 = SDL_GetPerformanceCounter();
      const std::string purity_before = check_render_purity ? demo.StepStateFingerprint() : std::string();
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
      SDL_RenderClear(renderer);
      demo.Render(renderer);
      if (check_render_purity) {
        demo.Render(renderer);  // the same picture again; must not move any step counter
        const std::string purity_after = demo.StepStateFingerprint();
        ++purity_checks;
        if (purity_after != purity_before) {
          ++purity_failures;
          SDL_Log("Render purity: FAIL frame=%llu\n  before: %s\n  after:  %s",
                  static_cast<unsigned long long>(step), purity_before.c_str(), purity_after.c_str());
        }
      }
      const Uint64 r1 = SDL_GetPerformanceCounter();
      render_us += ticks_to_us(r1 - r0);
      // Screen fade-to-black overlay, from the state this frame drew: the demo's gameplay<->frontend
      // / splash fades plus the Exit Game music-synced exit fade (state-3). Whichever is darker wins.
      fade_alpha = demo.screen_fade_alpha();
      if (music_player.exit_fade_active()) {
        fade_alpha =
            std::max(fade_alpha, std::clamp(static_cast<int>(music_player.exit_fade_progress() * 255.0f), 0, 255));
      }
      if (fade_alpha > 0) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, static_cast<Uint8>(fade_alpha));
        const SDL_FRect fade_rect = {0.0f, 0.0f, static_cast<float>(kLogicalWidth), static_cast<float>(kLogicalHeight)};
        SDL_RenderFillRect(renderer, &fade_rect);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
      }
      if (audit.inject_load_ms > 0.0 && (step % static_cast<std::uint64_t>(audit.inject_every)) == 0) {
        atet::BusyWaitMs(audit.inject_load_ms);
      }
      clock.SimulateFrameWork(smoke_work_ticks);  // smoke runs: this frame's work took K-1 ticks
      if (dump_this) {
        SDL_Surface* raw = SDL_RenderReadPixels(renderer, nullptr);
        if (!audit.dump_window_mode && dump_target != nullptr) {
          // Present the composed logical frame to the window unchanged (nearest 1:1
          // texels under the integer-scaled logical presentation).
          SDL_SetRenderTarget(renderer, nullptr);
          SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
          SDL_RenderClear(renderer);
          const SDL_FRect full = {0.0f, 0.0f, static_cast<float>(kLogicalWidth), static_cast<float>(kLogicalHeight)};
          SDL_RenderTexture(renderer, dump_target, nullptr, &full);
        }
        if (raw != nullptr) {
          SDL_Surface* rgb = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGB24);
          SDL_DestroySurface(raw);
          if (rgb != nullptr) {
            frame_w = rgb->w;
            frame_h = rgb->h;
            const auto* pixels = static_cast<const std::uint8_t*>(rgb->pixels);
            for (int y = 0; y < rgb->h; ++y) {
              frame_hash = atet::Fnv1a64Update(frame_hash, pixels + static_cast<size_t>(y) * rgb->pitch,
                                               static_cast<size_t>(rgb->w) * 3);
            }
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%06llu.png", static_cast<unsigned long long>(step));
            frame_file = name;
            const std::string path = audit.dump_dir + "/" + frame_file;
            frame_ok = SDL_SavePNG(rgb, path.c_str());
            if (!frame_ok) {
              SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Audit: SDL_SavePNG(%s) failed: %s", path.c_str(), SDL_GetError());
            }
            SDL_DestroySurface(rgb);
          }
        } else {
          SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Audit: SDL_RenderReadPixels failed: %s", SDL_GetError());
        }
      }
      const Uint64 r2 = SDL_GetPerformanceCounter();
      overlay_dump_us += ticks_to_us(r2 - r1);
      UpdateWindowTitle(window, demo);
      title_us += ticks_to_us(SDL_GetPerformanceCounter() - r2);
    };

    auto present_frame = [&]() {
      const Uint64 p0 = SDL_GetPerformanceCounter();
      SDL_RenderPresent(renderer);
      const Uint64 p1 = SDL_GetPerformanceCounter();
      present_us += ticks_to_us(p1 - p0);
      t_present_end = p1;
      if (perf_file != nullptr) {
        dxgi = atet::QueryDxgiPresentStats(renderer);
      }
      if (previous_present != 0) {
        present_interval_us = ticks_to_us(p1 - previous_present);
        if (step > audit.fast_forward) {  // only intervals that crossed a paced boundary
          present_intervals_us.push_back(present_interval_us);
        }
      } else {
        first_present = p1;
      }
      previous_present = p1;
    };

    // Up to `count` logic steps, the draw inside the last one, then that step's presentation half.
    // The original's loops leave only after a present, so a step that moves the game into or out
    // of a tick-loop phase ends the frame: the rest of its ticks go to the next pacer return.
    auto run_steps = [&](int count, bool frame_blocking) {
      steps_planned = count;
      bool presentation_done = count <= 0;
      const Uint64 k0 = SDL_GetPerformanceCounter();
      for (int i = 0; i < count; ++i) {
        demo.AuditSetStage("tick");
        demo.TickLogic();
        ++steps_run;
        ++logic_steps_total;
        if (i == count - 1 || demo.InBlockingPhase() != frame_blocking) {
          break;
        }
        demo.AuditSetStage("tick-presentation");
        demo.TickPresentation();
        if (demo.InBlockingPhase() != frame_blocking) {
          presentation_done = true;
          break;
        }
      }
      tick_us += ticks_to_us(SDL_GetPerformanceCounter() - k0);
      render_frame();
      if (!presentation_done) {
        const Uint64 k1 = SDL_GetPerformanceCounter();
        demo.AuditSetStage("tick-presentation");
        demo.TickPresentation();
        tick_us += ticks_to_us(SDL_GetPerformanceCounter() - k1);
      }
    };

    // A tick-loop frame waits until the tick differs from the one read before the wait (as the
    // original's splash and fade loops do), then runs one step per tick elapsed since the previous
    // tick-loop frame, so a frame whose work crossed a tick edge catches that tick up.
    int blocking_steps = 0;
    bool waited = false;
    auto blocking_wait = [&]() {
      const std::uint64_t snapshot = clock.Now();
      if (!blocking_clock_valid) {
        blocking_last_tick = snapshot;  // entering a tick loop: its first step is due at the next edge
      }
      clock.WaitForTickAfter(snapshot);
      record_wait();
      const std::uint64_t now_tick = clock.Now();
      blocking_steps = static_cast<int>(std::min(now_tick - blocking_last_tick, kMaxBlockingSteps));
      blocking_last_tick = now_tick;
      const Uint64 m0 = SDL_GetPerformanceCounter();
      service_timer();
      music_us += ticks_to_us(SDL_GetPerformanceCounter() - m0);
      waited = true;
    };

    if (demo.InBlockingPhase()) {
      blocking_wait();
    }
    poll_input();
    if (demo.InBlockingPhase()) {
      // A splash page or a gameplay palette fade: no pacer call, the baseline goes stale.
      if (!waited) {
        blocking_wait();  // the phase began with this frame's input (e.g. Esc in gameplay)
      }
      frame_kind = "blocking";
      demo.set_tick_count(clock.Now());
      run_steps(blocking_steps, true);
      present_frame();
      blocking_clock_valid = demo.InBlockingPhase();
    } else {
      frame_kind = "pacer";
      if (demo.ConsumePacerResetRequest()) {
        pacer.Reset();  // 0x24d0(1) at 0x4db-0x4ea: rebase, then exactly one step
        steps = 1;
        reset_this_frame = true;
      }
      demo.set_tick_count(clock.Now());
      run_steps(steps, false);
      steps = pacer.Pace(present_frame);
      record_wait();
      pacer_elapsed = pacer.last_elapsed();
      if (pacer_elapsed > static_cast<std::uint64_t>(atet::FramePacer::kMaxStepsPerFrame)) {
        ticks_dropped_total += pacer_elapsed - atet::FramePacer::kMaxStepsPerFrame;
        behind_resets += 1;
      }
      const Uint64 m0 = SDL_GetPerformanceCounter();
      service_timer();
      music_us += ticks_to_us(SDL_GetPerformanceCounter() - m0);
      blocking_clock_valid = false;
    }
    frames_by_steps[static_cast<size_t>(std::clamp(steps_run, 0, 7))] += 1;
    const std::uint64_t tick_at_end = clock.Now();
    const Uint64 t5b = SDL_GetPerformanceCounter();

    // ---- audit: offline audio for the ticks this iteration covered (735 frames per tick) --------
    const std::uint64_t block_start = tick_at_start * kFramesPerStep;
    const std::uint64_t block_frames = (tick_at_end - tick_at_start) * kFramesPerStep;
    if (capturing && block_frames > 0) {
      const std::uint64_t block_end = block_start + block_frames;
      if (sfx_timeline.size() < block_end * 2) {
        sfx_timeline.resize(block_end * 2, 0.0f);
      }
      sfx_mixer.MixInto(sfx_timeline.data() + block_start * 2, block_frames);
      auto mix_music_at = [&](std::uint64_t position, size_t frames) {
        capture_scratch.assign(frames * 2, 0);
        music_player.CaptureMix(capture_scratch.data(), frames);
        if (music_timeline.size() < (position + frames) * 2) {
          music_timeline.resize((position + frames) * 2, 0.0f);
        }
        for (size_t i = 0; i < frames * 2; ++i) {
          music_timeline[position * 2 + i] = static_cast<float>(capture_scratch[i]) / 32768.0f;
        }
      };
      if (!audit.audio_model_winmm) {
        // Ideal model: the tracker advances exactly the iteration's virtual time.
        mix_music_at(block_start, block_frames);
      } else {
        // WinMM model (drv_win.c WIN_Update): whenever fewer than 2 buffers are
        // queued, mix a whole 120 ms buffer at the CURRENT volume and append it
        // behind the queue; a buffer retires when the device clock passes its end.
        winmm_queue_ends.erase(
            std::remove_if(winmm_queue_ends.begin(), winmm_queue_ends.end(),
                           [block_start](std::uint64_t end) { return end <= block_start; }),
            winmm_queue_ends.end());
        if (winmm_queue_ends.empty() && winmm_queue_end < block_start) {
          winmm_underrun_frames += block_start - winmm_queue_end;
          winmm_queue_end = block_start;
        }
        while (winmm_queue_ends.size() < kWinmmBuffers) {
          mix_music_at(winmm_queue_end, kWinmmBufferFrames);
          winmm_queue_end += kWinmmBufferFrames;
          winmm_queue_ends.push_back(winmm_queue_end);
        }
      }
    }

    // ---- audit: trace record (state AFTER the iteration's steps, draw and timer service) --------
    if (trace_file != nullptr) {
      json.Clear();
      json.BeginObject();
      json.Key("schema").Str("atet-port-audit-trace").Key("version").Int(1);
      json.Key("step").UInt(step);
      json.Key("mode").Str(smoke_frames > 0 ? "smoke" : "paced");
      json.Key("timing")
          .BeginObject()
          .Key("tick_start").UInt(tick_at_start)
          .Key("tick_end").UInt(tick_at_end)
          .Key("tick_at_steps").UInt(demo.tick_count())
          .Key("kind").Str(frame_kind)
          .Key("steps_planned").Int(steps_planned)
          .Key("steps").Int(steps_run)
          .Key("pacer_reset").Bool(reset_this_frame)
          .Key("pacer_elapsed").UInt(pacer_elapsed)
          .Key("next_steps").Int(steps)
          .Key("logic_steps_total").UInt(logic_steps_total)
          .EndObject();
      json.Key("input_injected").BeginArray();
      for (const InjectedKey& key : injected_keys) {
        json.BeginObject().Key("source").Str(key.source).Key("key").Str(key.key).Key("down").Bool(key.down).EndObject();
      }
      json.EndArray();
      json.Key("pre_render").Raw(pre_json.str());
      demo.AuditWriteState(json, audit.trace_rng_values);
      json.Key("events").BeginObject().Key("emitted");
      demo.AuditWriteEmittedEvents(json);
      json.Key("delivered").BeginArray();
      for (const DeliveredEvent& delivered : delivered_events) {
        json.BeginObject().Key("kind").Str(delivered.is_sfx ? "sfx" : "music").Key("name").Str(delivered.name);
        if (delivered.is_sfx) {
          json.Key("slot").Int(delivered.slot).Key("sfx_volume").Int(delivered.sfx_volume).Key("pan").Int(
              delivered.pan).Key("voice_class").Int(delivered.voice_class);
        } else {
          json.Key("track").Int(delivered.track).Key("music_volume").Int(delivered.music_volume);
        }
        json.EndObject();
      }
      json.EndArray().EndObject();
      json.Key("music").Raw(music_player.AuditJson());
      json.Key("overlay")
          .BeginObject()
          .Key("alpha").Int(fade_alpha)
          .Key("music_exit_fade_active").Bool(music_player.exit_fade_active())
          .Key("music_exit_fade_progress").Double(music_player.exit_fade_progress(), 4)
          .EndObject();
      json.Key("frame");
      if (dump_this && frame_w > 0) {
        json.BeginObject()
            .Key("file").Str(frame_file)
            .Key("saved").Bool(frame_ok)
            .Key("width").Int(frame_w)
            .Key("height").Int(frame_h)
            .Key("rgb24_fnv1a64").Str(atet::Hex64(frame_hash))
            .Key("mode").Str(audit.dump_window_mode ? "window" : "logical")
            .EndObject();
      } else {
        json.Null();
      }
      json.Key("audio");
      if (capturing) {
        json.BeginObject()
            .Key("model").Str(audit.audio_model_winmm ? "winmm" : "ideal")
            .Key("block_start_frame").UInt(block_start)
            .Key("frames").UInt(block_frames)
            .Key("sfx_active_voices").Int(sfx_mixer.active_voices())
            .Key("winmm_queue_end_frame").UInt(winmm_queue_end)
            .EndObject();
      } else {
        json.Null();
      }
      json.EndObject();
      std::fputs(json.str().c_str(), trace_file);
      std::fputc('\n', trace_file);
    }
    const Uint64 t6 = SDL_GetPerformanceCounter();

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
    if (audit.paced_frames > 0 && step + 1 >= static_cast<std::uint64_t>(audit.paced_frames)) {
      running = false;
    }
    total_spin_us += spin_us;
    total_sleep_us += sleep_actual_us;
    const double work_us = ticks_to_us(t6 - t0) - sleep_actual_us - spin_us;
    if (step >= audit.fast_forward) {
      work_us_samples.push_back(work_us);
    }
    if (perf_file != nullptr) {
      std::fprintf(perf_file,
                   "%llu,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.1f,%.1f,%.1f,%d,%.1f,"
                   "%u,%u,%u,%u,%u,%u,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%u,%u,%u,%u,%.1f,"
                   "%llu,%s,%d,%llu,%llu\n",
                   static_cast<unsigned long long>(step), demo.scene_name(), input_us, tick_us, render_us,
                   overlay_dump_us, title_us, present_us, music_us, ticks_to_us(t6 - t5b), work_us, sleep_req_ms,
                   sleep_actual_us, spin_us, slack_us, pacer_elapsed > 6 ? 1 : 0, present_interval_us,
                   atet::g_audit.render_texture_calls, atet::g_audit.fill_rect_calls, atet::g_audit.draw_color_calls,
                   atet::g_audit.pointfield_points, atet::g_audit.glyph_row_draws, atet::g_audit.debris_drawn,
                   std::to_string(atet::g_audit.debris_active).c_str(), ticks_to_us(atet::g_audit.pointfield_ticks),
                   ticks_to_us(atet::g_audit.frontend_rows_ticks),
                   ticks_to_us(atet::g_audit.debris_render_ticks), ticks_to_us(atet::g_audit.debris_step_ticks),
                   ticks_to_us(atet::g_audit.topout_update_ticks), ticks_to_us(atet::g_audit.line_clear_seed_ticks),
                   ticks_to_us(t0 - audit_loop_start),
                   t_present_end > audit_loop_start ? ticks_to_us(t_present_end - audit_loop_start) : -1.0,
                   dxgi.valid ? 1 : 0, dxgi.last_present_count, dxgi.present_count, dxgi.present_refresh_count,
                   dxgi.sync_refresh_count,
                   dxgi.valid && dxgi.sync_qpc > static_cast<long long>(audit_loop_start)
                       ? ticks_to_us(static_cast<Uint64>(dxgi.sync_qpc) - audit_loop_start)
                       : -1.0,
                   static_cast<unsigned long long>(tick_at_end), frame_kind, steps_run,
                   static_cast<unsigned long long>(pacer_elapsed), static_cast<unsigned long long>(logic_steps_total));
    }
    ++step;
  }

  // ---- audit: summaries --------------------------------------------------------
  const Uint64 audit_loop_end = SDL_GetPerformanceCounter();
  const atet::ProcessStats audit_stats_end = atet::QueryProcessStats();
  const double loop_seconds = static_cast<double>(audit_loop_end - audit_loop_start) / static_cast<double>(perf_freq);
  auto percentile = [](std::vector<double> values, double p) {
    if (values.empty()) return -1.0;
    std::sort(values.begin(), values.end());
    const size_t index = std::min(values.size() - 1, static_cast<size_t>(p * static_cast<double>(values.size() - 1) + 0.5));
    return values[index];
  };
  double interval_mean = 0.0;
  for (const double value : present_intervals_us) interval_mean += value;
  if (!present_intervals_us.empty()) interval_mean /= static_cast<double>(present_intervals_us.size());
  double interval_var = 0.0;
  for (const double value : present_intervals_us) interval_var += (value - interval_mean) * (value - interval_mean);
  if (present_intervals_us.size() > 1) interval_var /= static_cast<double>(present_intervals_us.size() - 1);
  const double cpu_loop = (audit_stats_end.user_seconds + audit_stats_end.kernel_seconds) -
                          (audit_stats_loop_start.user_seconds + audit_stats_loop_start.kernel_seconds);
  const double ms = 1000.0 / static_cast<double>(perf_freq);
  const double paced_seconds =
      paced_phase_start != 0 ? static_cast<double>(audit_loop_end - paced_phase_start) / static_cast<double>(perf_freq)
                             : 0.0;
  const std::uint64_t paced_steps = paced_phase_start != 0 ? step - paced_phase_start_step : 0;
  const std::uint64_t paced_logic_steps = paced_phase_start != 0 ? logic_steps_total - paced_phase_start_logic : 0;
  const std::uint64_t paced_ticks = paced_phase_start != 0 ? clock.Now() - paced_phase_start_tick : 0;
  const double cpu_paced = paced_phase_start != 0
                               ? (audit_stats_end.user_seconds + audit_stats_end.kernel_seconds) -
                                     (stats_paced_start.user_seconds + stats_paced_start.kernel_seconds)
                               : 0.0;
  atet::JsonOut summary;
  summary.BeginObject()
      .Key("schema").Str("atet-port-audit-perf-summary").Key("version").Int(1)
      .Key("mode").Str(smoke_frames > 0 ? "smoke" : "paced")
      .Key("renderer").Str(SDL_GetRendererName(renderer) ? SDL_GetRendererName(renderer) : "?")
      .Key("video_driver").Str(SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?")
      .Key("audio_driver").Str(SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?")
      .Key("data_source").Str(atet::g_audit_startup.data_source)
      .Key("steps").UInt(step)
      .Key("loop_seconds").Double(loop_seconds, 6)
      .Key("effective_hz").Double(loop_seconds > 0 ? static_cast<double>(step) / loop_seconds : 0.0, 4)
      .Key("fast_forward_steps").UInt(audit.fast_forward)
      .Key("paced_steps").UInt(paced_steps)
      .Key("paced_seconds").Double(paced_seconds, 6)
      .Key("paced_effective_hz").Double(paced_seconds > 0 ? static_cast<double>(paced_steps) / paced_seconds : 0.0, 4)
      .Key("paced_cpu_percent_of_one_core").Double(paced_seconds > 0 ? 100.0 * cpu_paced / paced_seconds : 0.0, 2)
      .Key("vsync").Int(vsync)
      .Key("smoke_frame_ticks").Int(smoke_frame_ticks)
      .Key("logic_steps").UInt(logic_steps_total)
      .Key("paced_logic_steps").UInt(paced_logic_steps)
      .Key("paced_ticks").UInt(paced_ticks)
      .Key("paced_logic_hz").Double(paced_seconds > 0 ? static_cast<double>(paced_logic_steps) / paced_seconds : 0.0, 4)
      .Key("paced_tick_hz").Double(paced_seconds > 0 ? static_cast<double>(paced_ticks) / paced_seconds : 0.0, 4)
      .Key("ticks_dropped").UInt(ticks_dropped_total)
      .Key("frames_by_steps")
      .BeginArray()
      .UInt(frames_by_steps[0]).UInt(frames_by_steps[1]).UInt(frames_by_steps[2]).UInt(frames_by_steps[3])
      .UInt(frames_by_steps[4]).UInt(frames_by_steps[5]).UInt(frames_by_steps[6]).UInt(frames_by_steps[7])
      .EndArray()
      .Key("render_purity").BeginObject().Key("checks").UInt(purity_checks).Key("failures").Int(purity_failures).EndObject()
      .Key("present_interval_us")
      .BeginObject()
      .Key("count").UInt(present_intervals_us.size())
      .Key("mean").Double(interval_mean, 2)
      .Key("stdev").Double(std::sqrt(interval_var), 2)
      .Key("min").Double(percentile(present_intervals_us, 0.0), 1)
      .Key("p01").Double(percentile(present_intervals_us, 0.01), 1)
      .Key("p50").Double(percentile(present_intervals_us, 0.50), 1)
      .Key("p99").Double(percentile(present_intervals_us, 0.99), 1)
      .Key("max").Double(percentile(present_intervals_us, 1.0), 1)
      .EndObject()
      .Key("work_us")
      .BeginObject()
      .Key("p50").Double(percentile(work_us_samples, 0.50), 1)
      .Key("p99").Double(percentile(work_us_samples, 0.99), 1)
      .Key("max").Double(percentile(work_us_samples, 1.0), 1)
      .EndObject()
      .Key("behind_resets").Int(behind_resets)
      .Key("total_spin_ms").Double(total_spin_us / 1000.0, 3)
      .Key("total_sleep_ms").Double(total_sleep_us / 1000.0, 3)
      .Key("cpu_seconds_loop").Double(cpu_loop, 4)
      .Key("cpu_percent_of_one_core").Double(loop_seconds > 0 ? 100.0 * cpu_loop / loop_seconds : 0.0, 2)
      .Key("startup_ms")
      .BeginObject()
      .Key("process_creation_to_main").Double(audit_stats_at_main.seconds_since_process_creation * 1000.0, 2)
      .Key("sdl_init").Double(static_cast<double>(audit_t_sdl_init - audit_t_main) * ms, 2)
      .Key("window_renderer").Double(static_cast<double>(audit_t_window - audit_t_sdl_init) * ms, 2)
      .Key("demo_initialize").Double(static_cast<double>(audit_t_demo - audit_t_window) * ms, 2)
      .Key("demo_data_decode").Double(static_cast<double>(atet::g_audit_startup.data_ticks) * ms, 2)
      .Key("demo_textures").Double(static_cast<double>(atet::g_audit_startup.textures_ticks) * ms, 2)
      .Key("demo_tables").Double(static_cast<double>(atet::g_audit_startup.tables_ticks) * ms, 2)
      .Key("demo_setup").Double(static_cast<double>(atet::g_audit_startup.setup_ticks) * ms, 2)
      .Key("audio_init").Double(static_cast<double>(audit_t_audio - audit_t_demo) * ms, 2)
      .Key("main_to_first_present")
      .Double(first_present != 0 ? static_cast<double>(first_present - audit_t_main) * ms : -1.0, 2)
      .EndObject()
      .Key("memory_bytes")
      .BeginObject()
      .Key("working_set_loop_start").UInt(audit_stats_loop_start.working_set)
      .Key("private_loop_start").UInt(audit_stats_loop_start.private_bytes)
      .Key("working_set_end").UInt(audit_stats_end.working_set)
      .Key("private_end").UInt(audit_stats_end.private_bytes)
      .Key("peak_working_set").UInt(audit_stats_end.peak_working_set)
      .Key("peak_private").UInt(audit_stats_end.peak_private_bytes)
      .EndObject()
      .Key("audio_capture")
      .BeginObject()
      .Key("enabled").Bool(capturing)
      .Key("model").Str(audit.audio_model_winmm ? "winmm" : "ideal")
      .Key("winmm_underrun_frames").UInt(winmm_underrun_frames)
      .EndObject()
      .EndObject();
  std::fprintf(stderr, "Audit summary: %s\n", summary.str().c_str());
  if (check_render_purity) {
    std::fprintf(stderr, "Render purity: %s (%llu frames drawn twice, %d changed step state)\n",
                 purity_failures == 0 ? "PASS" : "FAIL", static_cast<unsigned long long>(purity_checks), purity_failures);
  }
  if (!audit.perf_summary_path.empty()) {
    if (std::FILE* out = std::fopen(audit.perf_summary_path.c_str(), "wb")) {
      std::fputs(summary.str().c_str(), out);
      std::fputc('\n', out);
      std::fclose(out);
    }
  }
  if (capturing) {
    atet::WavWriter wav;
    if (wav.Open(audit.audio_path, kCaptureRate, 2)) {
      const size_t frames = std::max(music_timeline.size(), sfx_timeline.size()) / 2;
      std::vector<std::int16_t> block(4096 * 2);
      size_t done = 0;
      while (done < frames) {
        const size_t count = std::min<size_t>(4096, frames - done);
        for (size_t i = 0; i < count * 2; ++i) {
          const size_t index = done * 2 + i;
          float sample = 0.0f;
          if (index < music_timeline.size()) sample += music_timeline[index];
          if (index < sfx_timeline.size()) sample += sfx_timeline[index];
          sample = std::clamp(sample, -1.0f, 1.0f);
          block[i] = static_cast<std::int16_t>(std::lround(sample * 32767.0f));
        }
        wav.AppendFrames(block.data(), count);
        done += count;
      }
      wav.Close();
      std::fprintf(stderr, "Audit: wrote %zu frames (%.3f s) to %s\n", frames,
                   static_cast<double>(frames) / kCaptureRate, audit.audio_path.c_str());
    } else {
      std::fprintf(stderr, "Audit: cannot open %s\n", audit.audio_path.c_str());
    }
  }
  if (trace_file != nullptr) {
    std::fclose(trace_file);
  }
  if (perf_file != nullptr) {
    std::fclose(perf_file);
  }
  if (dump_target != nullptr) {
    SDL_DestroyTexture(dump_target);
  }

  music_player.Shutdown();
  audio_probe.Shutdown();
  demo.Shutdown();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return purity_failures > 0 ? 3 : 0;
}
