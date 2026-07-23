#include "milestone_a_demo.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "indexed_asset_loader.h"

namespace atet {

namespace {

constexpr int kGameplayClearX = 109;
constexpr int kGameplayClearY = 21;
constexpr int kGameplayClearWidth = 80;
constexpr int kGameplayClearHeight = 160;
constexpr int kBoardWidth = 10;
constexpr int kBoardHeight = 20;
constexpr int kPreviewX = 30;
constexpr int kPreviewY = 56;
constexpr int kPreviewWidth = 32;
constexpr int kPreviewHeight = 16;
constexpr int kPieceTileSize = 8;
constexpr int kDigitWidth = 5;
constexpr int kDigitHeight = 5;
constexpr int kPieceStatX = 248;
constexpr std::array<int, 7> kPieceStatRows = {47, 58, 69, 80, 91, 102, 113};
constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 240;
constexpr int kFrontendGlyphCount = 66;
constexpr int kFrontendGlyphWidth = 16;
constexpr int kFrontendGlyphHeight = 12;
constexpr int kFrontendGlyphAtlasColumns = 11;
constexpr int kFrontendRenderHeight = 16;
constexpr int kFrontendRowY = 0x64;
constexpr int kFrontendRowStep = 0x11;
constexpr int kFrontendTextCenterX = 0xA0;
// High-score table columns (logical 320-space), measured from the running
// original: names left-aligned, score and lines right-aligned into columns.
constexpr int kHiscoreNameLeftX = 94;
constexpr int kHiscoreScoreRightX = 230;
constexpr int kHiscoreLinesRightX = 266;
constexpr int kFrontendSteadyFrames = 0x258;
constexpr int kFrontendMorphFrames = 0x80;
constexpr int kFrontendObjectWarmupFrames = 12;
constexpr int kFrontendRowRevealIntervalFrames = 4;
constexpr int kCreditsPageFrames = 360;
constexpr int kFrontendHoldStartFrames = 12;
constexpr int kFrontendHoldRepeatFrames = 4;
constexpr int kGameplayBindingRepeatFrames = 8;
constexpr int kGameplayGravityThreshold = 0x10000;
// RE gameplay-input-timing-pass: while Down is held the per-frame gravity increment
// is overridden to 0x8000 (half the descent threshold) => one row every two frames.
constexpr int kSoftDropIncrement = 0x8000;
constexpr int kGameplayLockFrames = 24;
constexpr int kGameplayCollapseFramesPerRow = 12;
constexpr int kGameplayMaxDebrisParticles = 4096;  // 0x1000, matching the original particle-pool cap
constexpr int kDebrisAgeFull = 0x400;              // age accumulator reaches this at end of life (4 stages of 0x100)
constexpr int kGameplayDownLockoutFrames = 8;
constexpr int kWarningLowRow = 8;
constexpr int kWarningMediumRow = 5;
constexpr int kWarningHighRow = 2;
constexpr int kWarningCooldownFrames = 120;
constexpr int kSleepyNoClearFrames = 60 * 45;
constexpr int kAlertEventLifetime = 96;  // frames a one-shot mood face (clear/wake) stays up (~1.4s @70fps)
constexpr int kHighScoreConcealFrames = 36;
constexpr int kHighScoreTableRevealRows = 6;
constexpr int kSplashPageFrames = 120;
constexpr int kScreenFadeTicks = 0x40;   // 64: palette fade-out 0x64f0 / fade-in 0x6498
constexpr int kSplashFadeTicks = 24;     // splash fade-in/out within a page (fits the 120-frame hold)
constexpr int kTopOutTimingFps = 60;
// RE 0x1f8c/0x1fc6: the top-out dissolve sweeps exactly ONE pixel-row per frame down
// the whole gameplay area (kBoardHeight*8 = 160 pixel rows), so the spray completes in
// ~160 frames (~2.3s at 70Hz), not the earlier 11s guess. With each particle living
// 16..79 frames, dozens of rows are mid-explosion at once = a continuous cascade.
constexpr int kTopOutDissolveFrames = kBoardHeight * kPieceTileSize;  // 160
constexpr int kTopOutOverlayStartFrame = kTopOutDissolveFrames;       // GAME OVER right after
constexpr int kTopOutOverlayEndFrame = kTopOutOverlayStartFrame + 10 * kTopOutTimingFps;  // ~hold
constexpr int kTopOutHandoffFrame = kTopOutOverlayEndFrame + 20;
constexpr int kState9BootstrapFrames = 72;
constexpr int kFrontendRecordSize = 0x1C;
constexpr int kFrontendRecordCount = 0x400;
constexpr int kFrontendBankCount = 8;
constexpr int kFrontendPointColorBase = 0xEF;
constexpr int kSetupDatSize = 178;
constexpr int kSetupSignatureOffset = 0x00;
constexpr int kSetupSignatureLength = 5;
constexpr int kSetupSoundDeviceOffset = 0x05;
constexpr int kSetupMixingRateOffset = 0x09;
constexpr int kSetupStereoFlagOffset = 0x0D;
constexpr int kSetupBitDepthFlagOffset = 0x11;
constexpr int kSetupKeyBindingOffset = 0x15;
constexpr int kSetupKeyBindingLength = 5;
constexpr int kSetupMusicVolumeOffset = 0x1A;
constexpr int kSetupSfxVolumeOffset = 0x1E;
constexpr int kSetupTrackIndexOffset = 0x22;
constexpr int kSetupHighScoreOffset = 0x26;
constexpr int kSetupHighScoreRecordSize = 0x1C;
constexpr int kSetupHighScoreNameLength = 20;
constexpr int kSetupHighScoreCount = 5;

bool IsTopOutAdvanceKey(SDL_Keycode key) {
  return key == SDLK_RETURN || key == SDLK_ESCAPE || key == SDLK_BACKSPACE || key == SDLK_SPACE;
}

struct CounterLayout {
  int x;
  int y;
  int digits;
  int value;
};

struct PiecePlacement {
  int atlas_index;
  int shape_index;
  int x;
  int y;
};

struct GameplayPreset {
  const char* name;
  CounterLayout high_score;
  CounterLayout score;
  CounterLayout level;
  CounterLayout lines;
  std::array<int, 7> piece_stats;
  PiecePlacement preview_piece;
  std::vector<PiecePlacement> board_pieces;
};

struct CellOffset {
  int x;
  int y;
};

struct DefaultHighScoreRecord {
  const char* name;
  int score;
  int lines;
};

constexpr std::array<std::array<CellOffset, 4>, 7> kTetrominoShapes = {{
    {{{0, 0}, {1, 0}, {2, 0}, {3, 0}}},
    {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}},
    {{{0, 0}, {1, 0}, {2, 0}, {1, 1}}},
    {{{1, 0}, {2, 0}, {0, 1}, {1, 1}}},
    {{{0, 0}, {1, 0}, {1, 1}, {2, 1}}},
    {{{0, 0}, {0, 1}, {1, 1}, {2, 1}}},
    {{{2, 0}, {0, 1}, {1, 1}, {2, 1}}},
}};

const std::array<GameplayPreset, 3> kGameplayPresets = {{
    {
        "bootstrap-inspired",
        {36, 89, 7, 500000},
        {36, 101, 7, 0},
        {60, 112, 3, 0},
        {144, 195, 4, 0},
        {1, 0, 0, 0, 0, 0, 0},
        {0, 0, kPreviewX, kPreviewY},
        {
            {6, 6, 3, 17},
            {1, 1, 7, 18},
            {2, 2, 0, 15},
            {3, 3, 5, 13},
            {4, 4, 2, 10},
            {5, 5, 0, 7},
            {0, 0, 4, 4},
        },
    },
    {
        "in-progress",
        {36, 89, 7, 500000},
        {36, 101, 7, 124860},
        {60, 112, 3, 4},
        {144, 195, 4, 38},
        {7, 4, 12, 3, 9, 11, 6},
        {2, 2, kPreviewX, kPreviewY},
        {
            {0, 0, 0, 18},
            {1, 1, 7, 18},
            {2, 2, 3, 15},
            {3, 3, 0, 13},
            {4, 4, 7, 12},
            {5, 5, 1, 9},
            {6, 6, 6, 7},
        },
    },
    {
        "late-run",
        {36, 89, 7, 500000},
        {36, 101, 7, 475320},
        {60, 112, 3, 9},
        {144, 195, 4, 96},
        {14, 18, 15, 19, 20, 17, 21},
        {6, 6, kPreviewX, kPreviewY},
        {
            {1, 1, 0, 18},
            {3, 3, 3, 16},
            {4, 4, 6, 15},
            {2, 2, 0, 12},
            {5, 5, 7, 11},
            {0, 0, 2, 8},
            {6, 6, 5, 5},
        },
    },
}};

const std::array<const char*, 6> kMusicTracks = {
    "Continuum",
    "Tearing Up SpaceTime",
    "Inner Walls Released",
    "Costumed",
    "I See It Now",
    "Simple Song",
};

const std::array<const char*, 6> kSoundDeviceNames = {
    "Autodetect",
    "Gravis Ultrasound",
    "SB Family",
    "Ensoniq Soundscape",
    "None",
    "PAS Family",
};

const std::array<int, 7> kMixingRates = {
    11025,
    16357,
    22050,
    27562,
    33074,
    38586,
    44100,
};

const std::array<const char*, 6> kCreditsPages = {
    "Acid Tetris\nCopyright 1997\nDungeon Dwellers Design",
    "Programming:\nJason Pimble\nGraphics:\nScott Emerle",
    "Music System:\nJean Paul Mikkers\nOrignal Concept\nDesign and Program By:\nAlexey Pazhitnov",
    "Protected Mode Extender By:\nCharles Scheffold\nThomas Pytel",
    "Music:\nChris Emerle\nScott Emerle\nLiam Hesse\nJon Dal Kristbjornsson\nBobby Tamburrino",
    "Special Thanks To:\nPaul Mason\nCathleen Crandall\nJohn Hood",
};

const std::array<const char*, 5> kKeyboardBindingLabels = {
    "Down",
    "Left",
    "Right",
    "Rotate Left",
    "Rotate Right",
};

const std::array<std::uint8_t, 5> kDefaultKeyboardBindings = {
    0xD0,
    0xCB,
    0xCD,
    0x1E,
    0x1F,
};

const std::array<DefaultHighScoreRecord, kSetupHighScoreCount> kDefaultHighScores = {{
    {"Jason", 100000, 100},
    {"Scott", 15000, 50},
    {"Bobby", 10000, 30},
    {"Liam", 6000, 20},
    {"Paul", 3000, 15},
}};

constexpr std::array<Uint8, kSetupSignatureLength> kSetupSignature = {'A', 'c', 'i', 'D', 0};

void SetErrorMessage(std::string* error_out, std::string message) {
  if (error_out == nullptr) {
    return;
  }

  *error_out = std::move(message);
}

int NormalizeModulo(int value, int modulus) {
  if (modulus <= 0) {
    return 0;
  }

  return (value % modulus + modulus) % modulus;
}


std::uint32_t ReadLittleEndian32(const std::vector<Uint8>& bytes, size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset + 0]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void WriteLittleEndian32(std::array<Uint8, kSetupDatSize>* bytes, size_t offset, std::uint32_t value) {
  if (bytes == nullptr || offset + 4 > bytes->size()) {
    return;
  }

  (*bytes)[offset + 0] = static_cast<Uint8>(value & 0xFF);
  (*bytes)[offset + 1] = static_cast<Uint8>((value >> 8) & 0xFF);
  (*bytes)[offset + 2] = static_cast<Uint8>((value >> 16) & 0xFF);
  (*bytes)[offset + 3] = static_cast<Uint8>((value >> 24) & 0xFF);
}

bool ReadBinaryAsset(const char* path, std::vector<Uint8>* bytes_out, std::string* error_out) {
  if (bytes_out == nullptr) {
    SetErrorMessage(error_out, "Binary output buffer was null.");
    return false;
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    SetErrorMessage(error_out, "Failed to open binary asset: " + std::string(path));
    return false;
  }

  bytes_out->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) {
    SetErrorMessage(error_out, "Failed while reading binary asset: " + std::string(path));
    return false;
  }

  return true;
}

bool ReadTextAsset(const char* path, std::string* text_out, std::string* error_out) {
  if (text_out == nullptr) {
    SetErrorMessage(error_out, "Text output buffer was null.");
    return false;
  }

  std::ifstream input(path);
  if (!input) {
    SetErrorMessage(error_out, "Failed to open text asset: " + std::string(path));
    return false;
  }

  text_out->assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (!input.good() && !input.eof()) {
    SetErrorMessage(error_out, "Failed while reading text asset: " + std::string(path));
    return false;
  }

  return true;
}

bool ParsePaletteJson(std::string_view json, std::array<SDL_Color, 256>* palette_out, std::string* error_out) {
  if (palette_out == nullptr) {
    SetErrorMessage(error_out, "Palette output buffer was null.");
    return false;
  }

  const size_t key_position = json.find("\"colors_8bit\"");
  if (key_position == std::string_view::npos) {
    SetErrorMessage(error_out, "Palette JSON is missing the colors_8bit array.");
    return false;
  }

  const size_t array_start = json.find('[', key_position);
  if (array_start == std::string_view::npos) {
    SetErrorMessage(error_out, "Palette JSON has no colors_8bit array start.");
    return false;
  }

  std::array<int, 256 * 3> components{};
  size_t component_index = 0;
  int value = 0;
  bool in_number = false;

  for (size_t i = array_start; i < json.size() && component_index < components.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(json[i]);
    if (std::isdigit(ch)) {
      value = in_number ? (value * 10) + static_cast<int>(ch - '0') : static_cast<int>(ch - '0');
      in_number = true;
      continue;
    }

    if (in_number) {
      components[component_index++] = value;
      value = 0;
      in_number = false;
    }
  }

  if (component_index != components.size()) {
    SetErrorMessage(error_out, "Palette JSON did not contain 256 RGB colors in colors_8bit.");
    return false;
  }

  for (size_t color_index = 0; color_index < palette_out->size(); ++color_index) {
    const size_t offset = color_index * 3;
    (*palette_out)[color_index] = SDL_Color{
        static_cast<Uint8>(components[offset + 0]),
        static_cast<Uint8>(components[offset + 1]),
        static_cast<Uint8>(components[offset + 2]),
        SDL_ALPHA_OPAQUE};
  }

  return true;
}

bool ParseFontWidthsJson(std::string_view json, std::array<int, kFrontendGlyphCount>* widths_out, std::string* error_out) {
  if (widths_out == nullptr) {
    SetErrorMessage(error_out, "Font width output buffer was null.");
    return false;
  }

  const size_t key_position = json.find("\"widths\"");
  if (key_position == std::string_view::npos) {
    SetErrorMessage(error_out, "Font widths JSON is missing widths.");
    return false;
  }

  const size_t array_start = json.find('[', key_position);
  if (array_start == std::string_view::npos) {
    SetErrorMessage(error_out, "Font widths JSON has no widths array start.");
    return false;
  }

  size_t width_index = 0;
  int value = 0;
  bool in_number = false;
  for (size_t i = array_start; i < json.size() && width_index < widths_out->size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(json[i]);
    if (std::isdigit(ch)) {
      value = in_number ? (value * 10) + static_cast<int>(ch - '0') : static_cast<int>(ch - '0');
      in_number = true;
      continue;
    }

    if (in_number) {
      (*widths_out)[width_index++] = value;
      value = 0;
      in_number = false;
    }
  }

  if (width_index != widths_out->size()) {
    SetErrorMessage(error_out, "Font widths JSON did not contain 66 widths.");
    return false;
  }

  return true;
}

std::int32_t ReadSignedLittleEndian32(const std::vector<Uint8>& bytes, size_t offset) {
  const std::uint32_t value = static_cast<std::uint32_t>(bytes[offset + 0]) |
                              (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                              (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                              (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
  return static_cast<std::int32_t>(value);
}

int TruncDiv(std::int64_t numerator, std::int64_t denominator) {
  if (denominator == 0) {
    return 0;
  }

  const bool negative = (numerator < 0) != (denominator < 0);
  const std::uint64_t abs_numerator =
      numerator < 0 ? static_cast<std::uint64_t>(-numerator) : static_cast<std::uint64_t>(numerator);
  const std::uint64_t abs_denominator =
      denominator < 0 ? static_cast<std::uint64_t>(-denominator) : static_cast<std::uint64_t>(denominator);
  const std::uint64_t quotient = abs_numerator / abs_denominator;
  return negative ? -static_cast<int>(quotient) : static_cast<int>(quotient);
}

std::array<int, 2048> BuildSineTable() {
  std::array<int, 2048> table{};
  constexpr double kPi = 3.14159265358979323846;
  for (int index = 0; index < 0x200; ++index) {
    const int value = static_cast<int>(std::lround(std::sin(index * kPi / 1024.0) * 65536.0));
    table[index] = value;
    table[0x3FF - index] = value;
    table[0x400 + index] = -value;
    table[0x7FF - index] = -value;
  }
  return table;
}

std::uint8_t OriginalScancodeForKey(SDL_Keycode key) {
  switch (key) {
    case SDLK_ESCAPE:
      return 0x01;
    case SDLK_1:
      return 0x02;
    case SDLK_2:
      return 0x03;
    case SDLK_3:
      return 0x04;
    case SDLK_4:
      return 0x05;
    case SDLK_5:
      return 0x06;
    case SDLK_6:
      return 0x07;
    case SDLK_7:
      return 0x08;
    case SDLK_8:
      return 0x09;
    case SDLK_9:
      return 0x0A;
    case SDLK_0:
      return 0x0B;
    case SDLK_BACKSPACE:
      return 0x0E;
    case SDLK_TAB:
      return 0x0F;
    case SDLK_Q:
      return 0x10;
    case SDLK_W:
      return 0x11;
    case SDLK_E:
      return 0x12;
    case SDLK_R:
      return 0x13;
    case SDLK_T:
      return 0x14;
    case SDLK_Y:
      return 0x15;
    case SDLK_U:
      return 0x16;
    case SDLK_I:
      return 0x17;
    case SDLK_O:
      return 0x18;
    case SDLK_P:
      return 0x19;
    case SDLK_RETURN:
      return 0x1C;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
      return 0x1D;
    case SDLK_A:
      return 0x1E;
    case SDLK_S:
      return 0x1F;
    case SDLK_D:
      return 0x20;
    case SDLK_F:
      return 0x21;
    case SDLK_G:
      return 0x22;
    case SDLK_H:
      return 0x23;
    case SDLK_J:
      return 0x24;
    case SDLK_K:
      return 0x25;
    case SDLK_L:
      return 0x26;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
      return 0x2A;
    case SDLK_Z:
      return 0x2C;
    case SDLK_X:
      return 0x2D;
    case SDLK_C:
      return 0x2E;
    case SDLK_V:
      return 0x2F;
    case SDLK_B:
      return 0x30;
    case SDLK_N:
      return 0x31;
    case SDLK_M:
      return 0x32;
    case SDLK_LALT:
    case SDLK_RALT:
      return 0x38;
    case SDLK_SPACE:
      return 0x39;
    case SDLK_UP:
      return 0xC8;
    case SDLK_LEFT:
      return 0xCB;
    case SDLK_RIGHT:
      return 0xCD;
    case SDLK_DOWN:
      return 0xD0;
    default:
      return 0;
  }
}

std::string KeyNameForBinding(std::uint8_t scan_code) {
  switch (scan_code) {
    case 0xCB:
      return "Left";
    case 0xCD:
      return "Right";
    case 0xC8:
      return "Up";
    case 0xD0:
      return "Down";
    case 0x39:
      return "Space";
    case 0x1C:
      return "Enter";
    case 0x0F:
      return "Tab";
    case 0x0E:
      return "Backspace";
    case 0x2A:
      return "Shift";
    case 0x1D:
      return "Ctrl";
    case 0x38:
      return "Alt";
    default:
      if (scan_code >= 0x02 && scan_code <= 0x0B) {
        constexpr std::array<char, 10> digits = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
        return std::string(1, digits[scan_code - 0x02]);
      }
      if (scan_code >= 0x10 && scan_code <= 0x19) {
        constexpr std::array<char, 10> row = {'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P'};
        return std::string(1, row[scan_code - 0x10]);
      }
      if (scan_code >= 0x1E && scan_code <= 0x26) {
        constexpr std::array<char, 9> row = {'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L'};
        return std::string(1, row[scan_code - 0x1E]);
      }
      if (scan_code >= 0x2C && scan_code <= 0x32) {
        constexpr std::array<char, 7> row = {'Z', 'X', 'C', 'V', 'B', 'N', 'M'};
        return std::string(1, row[scan_code - 0x2C]);
      }
      return "Key";
  }
}

int GlyphCodeForCharacter(unsigned char ch) {
  if (ch >= 'A' && ch <= 'Z') {
    return static_cast<int>(ch - 'A') + 1;
  }

  if (ch >= 'a' && ch <= 'z') {
    return static_cast<int>(ch - 'a') + 27;
  }

  // The chunk-5 frontend font lays out digits as 1,2,...,9,0 (zero LAST), not
  // 0-9. Verified against the original: with a naive 0-9 map the Options screen
  // rendered 100->"211" and 90->"01" (every digit shifted +1), and the menu
  // showed "Level 1" for start level 0.
  if (ch >= '1' && ch <= '9') {
    return static_cast<int>(ch - '1') + 53;
  }

  if (ch == '0') {
    return 62;
  }

  if (ch == '\'') {
    return 63;
  }

  if (ch == ':') {
    return 64;
  }

  if (ch == '`') {
    return 65;
  }

  if (ch == '_') {
    return 66;
  }

  return 0;
}

std::string FormatCounterValue(int value, int digits) {
  if (digits <= 0) {
    return {};
  }

  int modulus = 1;
  for (int i = 0; i < digits; ++i) {
    modulus *= 10;
  }

  const int clamped_value = std::max(0, value) % modulus;
  std::string text = std::to_string(clamped_value);
  if (static_cast<int>(text.size()) < digits) {
    text.insert(text.begin(), digits - static_cast<int>(text.size()), '0');
  }

  return text;
}

std::string FormatPercentRow(const char* label, int value) {
  return std::string(label) + std::to_string(std::clamp(value, 0, 100));
}

int MixingRateIndex(int rate_hz) {
  for (size_t index = 0; index < kMixingRates.size(); ++index) {
    if (kMixingRates[index] == rate_hz) {
      return static_cast<int>(index);
    }
  }

  return static_cast<int>(kMixingRates.size()) - 1;
}

void DrawTexture(SDL_Renderer* renderer, SDL_Texture* texture, int width, int height) {
  if (texture == nullptr) {
    return;
  }

  const SDL_FRect destination = {
      0.0f,
      0.0f,
      static_cast<float>(width),
      static_cast<float>(height),
  };
  SDL_RenderTexture(renderer, texture, nullptr, &destination);
}

void DrawTextureAt(SDL_Renderer* renderer, SDL_Texture* texture, int x, int y, int width, int height) {
  if (texture == nullptr) {
    return;
  }

  const SDL_FRect destination = {
      static_cast<float>(x),
      static_cast<float>(y),
      static_cast<float>(width),
      static_cast<float>(height),
  };
  SDL_RenderTexture(renderer, texture, nullptr, &destination);
}

void DrawPieceTile(SDL_Renderer* renderer, SDL_Texture* atlas, int atlas_index, int x, int y) {
  if (atlas == nullptr) {
    return;
  }

  const SDL_FRect source = {
      static_cast<float>(atlas_index * kPieceTileSize),
      0.0f,
      static_cast<float>(kPieceTileSize),
      static_cast<float>(kPieceTileSize),
  };
  const SDL_FRect destination = {
      static_cast<float>(x),
      static_cast<float>(y),
      static_cast<float>(kPieceTileSize),
      static_cast<float>(kPieceTileSize),
  };
  SDL_RenderTexture(renderer, atlas, &source, &destination);
}

void DrawTetromino(
    SDL_Renderer* renderer,
    SDL_Texture* atlas,
    int atlas_index,
    int shape_index,
    int screen_x,
    int screen_y) {
  const auto& shape = kTetrominoShapes[shape_index % static_cast<int>(kTetrominoShapes.size())];
  for (const CellOffset& cell : shape) {
    DrawPieceTile(
        renderer,
        atlas,
        atlas_index,
        screen_x + (cell.x * kPieceTileSize),
        screen_y + (cell.y * kPieceTileSize));
  }
}

std::array<CellOffset, 4> RotatedCells(int shape_index, int rotation) {
  std::array<CellOffset, 4> cells = kTetrominoShapes[NormalizeModulo(shape_index, static_cast<int>(kTetrominoShapes.size()))];
  rotation = NormalizeModulo(rotation, 4);
  for (CellOffset& cell : cells) {
    int x = cell.x;
    int y = cell.y;
    for (int step = 0; step < rotation; ++step) {
      const int rotated_x = y;
      const int rotated_y = -x;
      x = rotated_x;
      y = rotated_y;
    }
    cell = {x, y};
  }

  int min_x = cells[0].x;
  int min_y = cells[0].y;
  for (const CellOffset& cell : cells) {
    min_x = std::min(min_x, cell.x);
    min_y = std::min(min_y, cell.y);
  }

  for (CellOffset& cell : cells) {
    cell.x -= min_x;
    cell.y -= min_y;
  }
  return cells;
}

void DrawLiveTetromino(
    SDL_Renderer* renderer,
    SDL_Texture* atlas,
    int piece_type,
    int rotation,
    int board_x,
    int board_y) {
  const std::array<CellOffset, 4> cells = RotatedCells(piece_type, rotation);
  for (const CellOffset& cell : cells) {
    DrawPieceTile(
        renderer,
        atlas,
        NormalizeModulo(piece_type, static_cast<int>(kTetrominoShapes.size())),
        kGameplayClearX + ((board_x + cell.x) * kPieceTileSize),
        kGameplayClearY + ((board_y + cell.y) * kPieceTileSize));
  }
}

void DrawCounter(SDL_Renderer* renderer, SDL_Texture* digit_atlas, const CounterLayout& layout) {
  if (digit_atlas == nullptr) {
    return;
  }

  const std::string text = FormatCounterValue(layout.value, layout.digits);
  for (size_t i = 0; i < text.size(); ++i) {
    const int digit = text[i] - '0';
    const SDL_FRect source = {
        static_cast<float>((digit % 5) * kDigitWidth),
        static_cast<float>((digit / 5) * kDigitHeight),
        static_cast<float>(kDigitWidth),
        static_cast<float>(kDigitHeight),
    };
    const SDL_FRect destination = {
        static_cast<float>(layout.x + static_cast<int>(i) * kDigitWidth),
        static_cast<float>(layout.y),
        static_cast<float>(kDigitWidth),
        static_cast<float>(kDigitHeight),
    };
    SDL_RenderTexture(renderer, digit_atlas, &source, &destination);
  }
}

}  // namespace

MilestoneADemo::~MilestoneADemo() {
  Shutdown();
}

bool MilestoneADemo::Initialize(SDL_Renderer* renderer, std::string* error_out, bool start_in_sound_setup) {
  Shutdown();

  // Decode the game data in-engine -- no extracted/copyrighted assets are shipped;
  // every buffer below comes from AtetData (see atet_data.cpp). Resolve, in order:
  // a loose ATET.DAT, then the intact ACiD Tetris archive (AcidTetris.zip /
  // ATETRIS.ZIP, ATET.DAT extracted via miniz), first next to the executable
  // (portable / release layout, robust to the build machine's drive letter), then
  // the working directory, then the build-tree ATET.DAT as a dev fallback.
  std::string dat_error = "not found";
  bool loaded = false;
  std::vector<std::string> search_dirs;
  if (const char* base_path = SDL_GetBasePath()) {
    search_dirs.emplace_back(base_path);
  }
  search_dirs.emplace_back("");  // working directory
  for (const std::string& dir : search_dirs) {
    std::vector<Uint8> bytes;
    std::string ignored;
    // A loose data file. ACiD Tetris ships ATET.DAT; its 2002 rename SABA ("Super
    // ACiD Block Attack", the trademark-safe re-release) ships saba.dat -- which is
    // byte-identical to ATET.DAT except the title-screen logo (chunk 4), so the port
    // plays identically from either (see AtetData / atet_data.cpp).
    bool found = false;
    for (const char* dat_name : {"ATET.DAT", "saba.dat", "SABA.DAT"}) {
      if (ReadBinaryAsset((dir + dat_name).c_str(), &bytes, &ignored) &&
          atet_data_.LoadFromDat(bytes, &dat_error)) {
        loaded = true;
        found = true;
        break;
      }
    }
    if (found) {
      break;
    }
    // Or an intact archive of either release; LoadFromZip extracts the data file.
    for (const char* archive_name :
         {"AcidTetris.zip", "ATETRIS.ZIP", "acidtetris.zip", "saba.zip", "SABA.ZIP"}) {
      if (ReadBinaryAsset((dir + archive_name).c_str(), &bytes, &ignored) &&
          atet_data_.LoadFromZip(bytes, &dat_error)) {
        loaded = true;
        break;
      }
    }
    if (loaded) {
      break;
    }
  }
  if (!loaded) {
    std::vector<Uint8> bytes;
    std::string ignored;
    if (ReadBinaryAsset(ATET_PORT_ATET_DAT_PATH, &bytes, &ignored) &&
        atet_data_.LoadFromDat(bytes, &dat_error)) {
      loaded = true;
    }
  }
  if (!loaded) {
    SetErrorMessage(error_out,
                    "Could not load the game data (" + dat_error +
                        "). Place ATET.DAT (or SABA's saba.dat), or an ACiD Tetris / SABA "
                        "archive, beside the executable.");
    return false;
  }

  const std::array<SDL_Color, 256>& gameplay_palette = atet_data_.gameplay_palette();
  const std::array<SDL_Color, 256>& title_palette = atet_data_.title_palette();

  if (!LoadTextureFromData(renderer, atet_data_.ddd_splash(), atet_data_.ddd_palette(), -1, &ddd_splash_, error_out) ||
      !LoadTextureFromData(renderer, atet_data_.warning_splash(), atet_data_.warning_palette(), -1, &warning_splash_,
                           error_out) ||
      !LoadTextureFromData(renderer, atet_data_.title_screen(), title_palette, -1, &title_screen_, error_out) ||
      !LoadTextureFromData(renderer, atet_data_.gameplay_screen(), gameplay_palette, -1, &gameplay_screen_, error_out) ||
      !LoadTextureFromData(renderer, atet_data_.piece_atlas(), gameplay_palette, 0, &piece_atlas_, error_out) ||
      !LoadTextureFromData(renderer, atet_data_.digit_atlas(), gameplay_palette, 0, &digit_atlas_, error_out) ||
      !LoadTextureFromData(renderer, atet_data_.game_over_overlay(), gameplay_palette, 0, &game_over_overlay_,
                           error_out) ||
      !LoadTextureFromData(renderer, atet_data_.font_atlas(), title_palette, 0, &frontend_font_atlas_, error_out)) {
    return false;
  }

  // chunk-6 alert/mood-face bank (14 50x50 tiles). Each keys palette index 0x20
  // transparent so a face reads as a circle over the water, not a box
  // (RE board-alert-pass: 0x1793d treats index 0x20 as transparent).
  if (atet_data_.alert_tile_count() < static_cast<int>(alert_tiles_.size())) {
    SetErrorMessage(error_out, "ATET.DAT chunk 6 yielded fewer alert tiles than expected.");
    return false;
  }
  for (size_t tile = 0; tile < alert_tiles_.size(); ++tile) {
    if (!LoadTextureFromData(renderer, atet_data_.alert_tile(static_cast<int>(tile)), gameplay_palette, 0x20,
                             &alert_tiles_[tile], error_out)) {
      return false;
    }
  }

  if (!LoadFrontendFont(error_out)) {
    return false;
  }

  if (!LoadFrontendPointfield(error_out)) {
    return false;
  }

  if (!LoadGameplayParticleAssets(error_out)) {
    return false;
  }

  scene_ = Scene::kFrontend;
  SetFrontendView(FrontendView::kMainMenu, 0);
  gameplay_preset_index_ = 0;
  keyboard_capture_row_ = -1;
  held_original_scancodes_.fill(false);
  held_original_scancode_frames_.fill(0);
  high_score_entry_row_ = -1;
  high_score_entry_name_.clear();
  high_score_entry_lowercase_ = false;
  high_score_entry_commit_pending_ = false;
  top_out_handoff_pending_ = false;
  top_out_frame_ = 0;
  top_out_score_ = 0;
  top_out_lines_ = 0;
  live_game_active_ = false;
  live_board_cells_.fill(0);
  live_piece_stats_.fill(0);
  live_piece_ = LivePiece{};
  live_next_piece_type_ = 0;
  live_score_ = 0;
  live_lines_ = 0;
  live_level_ = 0;
  gameplay_phase_ = GameplayPhase::kFalling;
  gravity_accumulator_ = 0;
  lock_frames_ = 0;
  down_lockout_frames_ = 0;
  pending_clear_rows_.clear();
  collapse_frame_ = 0;
  gameplay_top_out_frame_ = 0;
  warning_band_ = 0;
  warning_cooldowns_.fill(0);
  debris_particles_.clear();
  live_snapshot_ = LiveSnapshot{};
  gameplay_snapshot_texture_valid_ = false;
  last_clear_was_four_ = false;
  sleepy_ = false;
  frames_since_clear_ = 0;
  high_score_bootstrap_frames_ = 0;
  high_score_conceal_frames_ = 0;
  audio_events_.clear();
  line_clear_demo_count_ = 0;
  ResetSetupStateToDefaults();
  live_game_available_ = false;
  quit_requested_ = false;
  const bool loaded_setup = LoadSetupDat();
  if (!loaded_setup) {
    PersistSetupState();
  }
  if (start_in_sound_setup || !loaded_setup) {
    SetFrontendView(FrontendView::kSoundSetup, 0);
    scene_ = Scene::kFrontend;
  } else {
    BeginStartupSplashes();
  }
  if (error_out != nullptr) {
    error_out->clear();
  }

  return true;
}

bool MilestoneADemo::HandleKey(SDL_Keycode key) {
  return HandleKeyDown(key);
}

void MilestoneADemo::ResetBuildLocalSetupState() {
  ResetSetupStateToDefaults();
  PersistSetupState();
}

bool MilestoneADemo::HandleKeyDown(SDL_Keycode key) {
  // Input is blocked while a gameplay<->frontend screen fade is in progress.
  if (screen_fade_ != ScreenFade::kNone) {
    return false;
  }
  TrackKeyDown(key);

  if (scene_ == Scene::kSplash) {
    if (key == SDLK_RETURN || key == SDLK_SPACE || key == SDLK_ESCAPE || key == SDLK_BACKSPACE) {
      splash_frame_ = kSplashPageFrames - 1;
      AdvanceSplashTimers();
      return true;
    }
    return false;
  }

  if (scene_ == Scene::kTopOut) {
    if (IsTopOutAdvanceKey(key)) {
      if (top_out_frame_ < kTopOutOverlayStartFrame) {
        top_out_frame_ = kTopOutOverlayStartFrame;
        top_out_handoff_pending_ = false;
        return true;
      }
      if (top_out_frame_ < kTopOutOverlayEndFrame) {
        top_out_frame_ = kTopOutOverlayEndFrame;
        top_out_handoff_pending_ = false;
        return true;
      }
      top_out_handoff_pending_ = true;
      return true;
    }
    return false;
  }

  if (scene_ == Scene::kFrontend && frontend_view_ == FrontendView::kKeyboardSetup &&
      keyboard_capture_row_ >= 0) {
    const std::uint8_t original_scancode = OriginalScancodeForKey(key);
    if (key == SDLK_UNKNOWN || original_scancode == 0 || original_scancode == 0x01) {
      return true;
    }

    CaptureKeyboardBinding(original_scancode);
    return true;
  }

  if (scene_ == Scene::kFrontend && frontend_view_ == FrontendView::kHighScoreEntry) {
    return HandleHighScoreNameEntry(key);
  }

  if (key == SDLK_TAB) {
    scene_ = (scene_ == Scene::kFrontend) ? Scene::kGameplay : Scene::kFrontend;
    return true;
  }

  if (key == SDLK_T) {
    scene_ = Scene::kFrontend;
    SetFrontendView(FrontendView::kMainMenu, 0);
    return true;
  }

  if (key == SDLK_G) {
    if (!live_game_available_) {
      ResetLiveGameplayState();
      live_game_available_ = true;
    }
    live_game_active_ = true;
    scene_ = Scene::kGameplay;
    return true;
  }

  if (key >= SDLK_1 && key <= SDLK_3) {
    gameplay_preset_index_ = static_cast<int>(key - SDLK_1);
    live_game_active_ = false;
    scene_ = Scene::kGameplay;
    return true;
  }

  if (scene_ == Scene::kGameplay) {
    if (key == SDLK_ESCAPE) {
      SaveLiveSnapshot();
      BeginSceneFade(PendingScene::kFrontendFromGameplay);
      return true;
    }
    if (key == SDLK_H) {
      BeginHighScoreNameEntry(CurrentRunScore(), CurrentRunLines());
      return true;
    }
    if (key == SDLK_O) {
      BeginTopOutDemo(CurrentRunScore(), CurrentRunLines());
      return true;
    }
    if (key == SDLK_C && live_game_active_) {
      InjectFilledRowsForSmoke((line_clear_demo_count_ % 4) + 1);
      ++line_clear_demo_count_;
      return true;
    }
    if (key == SDLK_B && live_game_active_) {
      // Debug: always inject a four-line clear. Press twice in a row to exercise
      // the back-to-back tetris reward (alert tile 8 + slot 11).
      InjectFilledRowsForSmoke(4);
      return true;
    }
    if (HandleGameplayBinding(key)) {
      return true;
    }
    return false;
  }

  if (frontend_view_ == FrontendView::kHighScores) {
    if (key == SDLK_RETURN || key == SDLK_ESCAPE || key == SDLK_BACKSPACE) {
      const std::vector<std::string> rows = CurrentFrontendRows();
      if (RevealedFrontendRowCount(rows.size()) >= static_cast<int>(rows.size())) {
        BeginHighScoreConcealToMain();
      }
      return true;
    }
    return false;
  }

  if (frontend_view_ == FrontendView::kCredits) {
    if (key == SDLK_RIGHT || key == SDLK_DOWN || key == SDLK_RETURN) {
      credits_page_ = (credits_page_ + 1) % static_cast<int>(kCreditsPages.size());
      credits_page_timer_ = 0;
      return true;
    }
    if (key == SDLK_LEFT || key == SDLK_UP) {
      credits_page_ = (credits_page_ + static_cast<int>(kCreditsPages.size()) - 1) %
                      static_cast<int>(kCreditsPages.size());
      credits_page_timer_ = 0;
      return true;
    }
    if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) {
      SetFrontendView(FrontendView::kMainMenu, 0);
      return true;
    }
    return false;
  }

  if (key == SDLK_UP) {
    MoveFrontendSelection(-1);
    return true;
  }

  if (key == SDLK_DOWN) {
    MoveFrontendSelection(1);
    return true;
  }

  if (key == SDLK_LEFT || key == SDLK_RIGHT) {
    return ApplyFrontendHorizontalEdit(key == SDLK_LEFT ? -1 : 1, true);
  }

  if (key == SDLK_RETURN) {
    ActivateFrontendSelection();
    return true;
  }

  if (key == SDLK_ESCAPE || key == SDLK_BACKSPACE) {
    if (frontend_view_ == FrontendView::kMainMenu) {
      quit_requested_ = true;
    } else if (frontend_view_ == FrontendView::kSoundSetup) {
      BeginStartupSplashes();
    } else if (frontend_view_ == FrontendView::kKeyboardSetup) {
      SetFrontendView(FrontendView::kOptions, 2);
    } else {
      SetFrontendView(FrontendView::kMainMenu, 0);
    }
    return true;
  }

  return false;
}

void MilestoneADemo::HandleKeyUp(SDL_Keycode key) {
  TrackKeyUp(key);
  if (scene_ == Scene::kTopOut && IsTopOutAdvanceKey(key) && top_out_handoff_pending_) {
    top_out_handoff_pending_ = false;
    BeginHighScoreNameEntry(top_out_score_, top_out_lines_);
    return;
  }
  if (scene_ == Scene::kFrontend && frontend_view_ == FrontendView::kHighScoreEntry &&
      key == SDLK_RETURN && high_score_entry_commit_pending_) {
    CommitHighScoreNameEntry();
  }
}

void MilestoneADemo::Tick() {
  // While a screen fade is in progress the original busy-waits on the timer and
  // does not advance game logic; freeze everything except the fade (and audio
  // delivery) until it completes.
  if (screen_fade_ != ScreenFade::kNone) {
    AdvanceScreenFade();
    FlushAudioEvents();
    return;
  }

  for (size_t scan_code = 0; scan_code < held_original_scancodes_.size(); ++scan_code) {
    if (!held_original_scancodes_[scan_code]) {
      continue;
    }

    ++held_original_scancode_frames_[scan_code];
  }

  if (scene_ == Scene::kFrontend &&
      (frontend_view_ == FrontendView::kOptions || frontend_view_ == FrontendView::kSoundSetup)) {
    const bool left_held = held_original_scancodes_[0xCB];
    const bool right_held = held_original_scancodes_[0xCD];
    if (left_held != right_held) {
      const std::uint8_t scan_code = left_held ? 0xCB : 0xCD;
      const int frames = held_original_scancode_frames_[scan_code];
      if (frames >= kFrontendHoldStartFrames &&
          (frames - kFrontendHoldStartFrames) % kFrontendHoldRepeatFrames == 0) {
        ApplyFrontendHorizontalEdit(left_held ? -1 : 1, false);
      }
    }
  }

  if (scene_ != Scene::kGameplay) {
    FlushAudioEvents();
    return;
  }

  if (live_game_active_) {
    // Soft-drop is polled per frame inside the gravity step (CurrentDropIncrement):
    // while Down is held the accumulator increment is 0x8000, i.e. one row every two
    // frames, matching the original. It is deliberately NOT a discrete per-frame row
    // move here. The move/rotate auto-repeat below excludes binding 0 (Down).
    StepLiveGameplay();
  }

  for (const int binding_index : {1, 2, 3, 4}) {
    const std::uint8_t scan_code = keyboard_bindings_[binding_index];
    if (scan_code == 0 || !held_original_scancodes_[scan_code]) {
      continue;
    }

    const int frames = held_original_scancode_frames_[scan_code];
    if (frames > 0 && frames % kGameplayBindingRepeatFrames == 0) {
      ApplyGameplayBinding(scan_code, false);
    }
  }
  FlushAudioEvents();
}

void MilestoneADemo::Render(SDL_Renderer* renderer) {
  if (scene_ == Scene::kSplash) {
    RenderSplash(renderer);
    return;
  }

  if (scene_ == Scene::kTopOut) {
    RenderTopOut(renderer);
    return;
  }

  if (scene_ == Scene::kFrontend) {
    RenderFrontend(renderer);
    return;
  }

  RenderGameplay(renderer);
}

bool MilestoneADemo::CaptureGameplaySnapshot(SDL_Renderer* renderer) {
  if (renderer == nullptr || scene_ != Scene::kGameplay || !live_game_active_) {
    return false;
  }

  if (gameplay_snapshot_texture_ == nullptr) {
    gameplay_snapshot_texture_ = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET,
        kScreenWidth,
        kScreenHeight);
    if (gameplay_snapshot_texture_ == nullptr) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to create gameplay snapshot texture: %s", SDL_GetError());
      gameplay_snapshot_texture_valid_ = false;
      return false;
    }
  }

  gameplay_snapshot_texture_valid_ = RenderGameplayToTexture(renderer, gameplay_snapshot_texture_);
  return gameplay_snapshot_texture_valid_;
}

void MilestoneADemo::Shutdown() {
  const std::array<TextureAsset*, 8> textures = {
      &ddd_splash_,
      &warning_splash_,
      &title_screen_,
      &gameplay_screen_,
      &piece_atlas_,
      &digit_atlas_,
      &game_over_overlay_,
      &frontend_font_atlas_,
  };

  const auto destroy_texture = [](TextureAsset* texture) {
    if (texture->texture != nullptr) {
      SDL_DestroyTexture(texture->texture);
      texture->texture = nullptr;
    }
    texture->width = 0;
    texture->height = 0;
  };
  for (TextureAsset* texture : textures) {
    destroy_texture(texture);
  }
  for (TextureAsset& tile : alert_tiles_) {
    destroy_texture(&tile);
  }
  if (gameplay_snapshot_texture_ != nullptr) {
    SDL_DestroyTexture(gameplay_snapshot_texture_);
    gameplay_snapshot_texture_ = nullptr;
  }
  gameplay_snapshot_texture_valid_ = false;
  if (top_out_saved_under_texture_ != nullptr) {
    SDL_DestroyTexture(top_out_saved_under_texture_);
    top_out_saved_under_texture_ = nullptr;
  }
  top_out_saved_under_valid_ = false;

  title_indices_.clear();
  piece_atlas_indices_.clear();
  particle_ramps_.fill(0);
  for (std::vector<FrontendPoint>& bank : frontend_banks_) {
    bank.clear();
  }
}

const char* MilestoneADemo::scene_name() const {
  if (scene_ == Scene::kSplash) {
    return splash_page_ == 0 ? "DDD Splash" : "Warning Splash";
  }

  if (scene_ == Scene::kTopOut) {
    return "Top Out";
  }

  if (scene_ == Scene::kGameplay) {
    return "Gameplay";
  }

  switch (frontend_view_) {
    case FrontendView::kMainMenu:
      return "Main Menu";
    case FrontendView::kOptions:
      return "Options";
    case FrontendView::kKeyboardSetup:
      return "Keyboard Setup";
    case FrontendView::kHighScores:
      return "High Scores";
    case FrontendView::kHighScoreEntry:
      return "High Score Entry";
    case FrontendView::kCredits:
      return "Credits";
    case FrontendView::kSoundSetup:
      return "Sound Setup";
  }

  return "Frontend";
}

std::string MilestoneADemo::debug_state_line() const {
  const char* phase = "falling";
  if (gameplay_phase_ == GameplayPhase::kCollapse) {
    phase = "collapse";
  } else if (gameplay_phase_ == GameplayPhase::kTopOutDissolve) {
    phase = "topout";
  }

  std::ostringstream stream;
  stream << "Debug state: scene=" << scene_name()
         << " phase=" << phase
         << " piece=" << live_piece_.type << ":" << live_piece_.rotation
         << " pos=" << live_piece_.x << "," << live_piece_.y
         << " next=" << live_next_piece_type_
         << " score=" << live_score_
         << " lines=" << live_lines_
         << " level=" << live_level_
         << " gravity=" << gravity_accumulator_
         << " lock=" << lock_frames_
         << " rng=" << gameplay_random_index_
         << " clears=" << pending_clear_rows_.size()
         << " collapse=" << collapse_frame_
         << " topout=" << top_out_frame_
         << " front=" << frontend_view_frame_
         << " hsboot=" << HighScoreBootstrapRemainingFrames()
         << " rows=" << (scene_ == Scene::kFrontend ? VisibleFrontendRowCount(CurrentFrontendRows().size()) : 0)
         << " conceal=" << high_score_conceal_frames_
         << " commit=" << (high_score_entry_commit_pending_ ? 1 : 0)
         << " warning=" << warning_band_
         << " alert=" << alert_effect_id_
         << " pulse=" << PulseRevealAmount()
         << " debris=" << debris_particles_.size()
         << " sleepy=" << (sleepy_ ? 1 : 0)
         << " snapshot=" << (gameplay_snapshot_texture_valid_ ? 1 : 0)
         << " savedunder=" << (top_out_saved_under_valid_ ? 1 : 0)
         << " fade=" << (screen_fade_ == ScreenFade::kOut ? "out" : screen_fade_ == ScreenFade::kIn ? "in" : "none")
         << ":" << screen_fade_frame_;
  return stream.str();
}

void MilestoneADemo::set_start_level_override(int level) {
  start_level_ = std::clamp(level, 0, 9);
  live_level_ = start_level_;
}

bool MilestoneADemo::LoadTextureFromData(
    SDL_Renderer* renderer,
    const AtetData::Indexed& image,
    const std::array<SDL_Color, 256>& palette,
    int transparent_index,
    TextureAsset* asset_out,
    std::string* error_out) {
  if (asset_out == nullptr) {
    SetErrorMessage(error_out, "Texture asset output was null.");
    return false;
  }

  const IndexedImage rgba =
      BuildIndexedImage(image.indices, image.width, image.height, palette, transparent_index);
  SDL_Texture* texture = CreateTextureFromImage(renderer, rgba, error_out);
  if (texture == nullptr) {
    return false;
  }

  asset_out->texture = texture;
  asset_out->width = image.width;
  asset_out->height = image.height;
  return true;
}

bool MilestoneADemo::LoadFrontendFont(std::string* error_out) {
  title_indices_ = atet_data_.title_screen().indices;
  if (title_indices_.size() != static_cast<size_t>(kScreenWidth * kScreenHeight)) {
    SetErrorMessage(error_out, "Title screen size mismatch from ATET.DAT.");
    return false;
  }

  title_palette_ = atet_data_.title_palette();

  const std::vector<Uint8>& widths = atet_data_.font_widths();
  if (widths.size() != frontend_font_widths_.size()) {
    SetErrorMessage(error_out, "Font width table size mismatch from ATET.DAT.");
    return false;
  }
  for (size_t index = 0; index < widths.size(); ++index) {
    frontend_font_widths_[index] = static_cast<int>(widths[index]);
  }

  sine_table_ = BuildSineTable();
  return true;
}

bool MilestoneADemo::LoadFrontendPointfield(std::string* error_out) {
  const std::vector<Uint8>& bankset = atet_data_.frontend_bankset();

  const size_t expected_size =
      static_cast<size_t>(kFrontendBankCount) * kFrontendRecordCount * kFrontendRecordSize;
  if (bankset.size() != expected_size) {
    SetErrorMessage(
        error_out,
        "Chunk-7 frontend bankset size mismatch: expected " + std::to_string(expected_size) +
            " bytes, got " + std::to_string(bankset.size()) + ".");
    return false;
  }

  for (int bank_index = 0; bank_index < kFrontendBankCount; ++bank_index) {
    std::vector<FrontendPoint>& bank = frontend_banks_[bank_index];
    bank.clear();
    bank.reserve(kFrontendRecordCount);
    const size_t bank_offset = static_cast<size_t>(bank_index) * kFrontendRecordCount * kFrontendRecordSize;
    for (int record_index = 0; record_index < kFrontendRecordCount; ++record_index) {
      const size_t offset = bank_offset + static_cast<size_t>(record_index) * kFrontendRecordSize;
      bank.push_back(
          FrontendPoint{
              ReadSignedLittleEndian32(bankset, offset + 0),
              ReadSignedLittleEndian32(bankset, offset + 4),
              ReadSignedLittleEndian32(bankset, offset + 8),
          });
    }
  }

  frontend_pointfield_state_ = FrontendPointfieldState{};
  frontend_current_bank_ = 2;
  frontend_next_bank_ = 0;
  frontend_cycle_counter_ = 0;
  frontend_frame_counter_ = 0;
  return true;
}

bool MilestoneADemo::LoadGameplayParticleAssets(std::string* error_out) {
  piece_atlas_indices_ = atet_data_.piece_atlas().indices;
  if (piece_atlas_indices_.size() != static_cast<size_t>(56 * 8)) {
    SetErrorMessage(error_out, "Piece atlas size mismatch from ATET.DAT.");
    return false;
  }

  const std::vector<Uint8>& ramps = atet_data_.particle_ramps();
  if (ramps.size() != particle_ramps_.size()) {
    SetErrorMessage(error_out, "Chunk-3 particle ramp size mismatch from ATET.DAT.");
    return false;
  }
  std::copy(ramps.begin(), ramps.end(), particle_ramps_.begin());

  gameplay_palette_ = atet_data_.gameplay_palette();
  return true;
}

bool MilestoneADemo::LoadSetupDat() {
  std::vector<Uint8> bytes;
  std::string ignored_error;
  if (!ReadBinaryAsset(ATET_PORT_SETUP_DAT_PATH, &bytes, &ignored_error)) {
    return false;
  }

  if (bytes.size() != kSetupDatSize) {
    return false;
  }

  for (size_t index = 0; index < kSetupSignature.size(); ++index) {
    if (bytes[kSetupSignatureOffset + index] != kSetupSignature[index]) {
      return false;
    }
  }

  sound_device_index_ = std::clamp(
      static_cast<int>(ReadLittleEndian32(bytes, kSetupSoundDeviceOffset)),
      0,
      5);
  mixing_rate_hz_ = static_cast<int>(ReadLittleEndian32(bytes, kSetupMixingRateOffset));
  stereo_flag_ = ReadLittleEndian32(bytes, kSetupStereoFlagOffset) == 0 ? 0 : 1;
  bit_depth_flag_ = ReadLittleEndian32(bytes, kSetupBitDepthFlagOffset) == 0 ? 0 : 1;
  music_volume_ = std::clamp(static_cast<int>(ReadLittleEndian32(bytes, kSetupMusicVolumeOffset)), 0, 100);
  sfx_volume_ = std::clamp(static_cast<int>(ReadLittleEndian32(bytes, kSetupSfxVolumeOffset)), 0, 100);
  music_track_index_ = NormalizeModulo(
      static_cast<int>(ReadLittleEndian32(bytes, kSetupTrackIndexOffset)),
      static_cast<int>(kMusicTracks.size()));

  for (size_t index = 0; index < keyboard_bindings_.size(); ++index) {
    const Uint8 binding = bytes[kSetupKeyBindingOffset + index];
    keyboard_bindings_[index] = (binding == 0 || binding == 0x01) ? kDefaultKeyboardBindings[index] : binding;
  }

  for (size_t index = 0; index < high_scores_.size(); ++index) {
    const size_t offset = kSetupHighScoreOffset + (index * kSetupHighScoreRecordSize);
    const char* name_begin = reinterpret_cast<const char*>(&bytes[offset]);
    const char* name_end = name_begin;
    while (name_end < name_begin + kSetupHighScoreNameLength && *name_end != '\0') {
      ++name_end;
    }
    high_scores_[index].name.assign(name_begin, name_end);
    high_scores_[index].score = static_cast<int>(ReadLittleEndian32(bytes, offset + kSetupHighScoreNameLength));
    high_scores_[index].lines =
        static_cast<int>(ReadLittleEndian32(bytes, offset + kSetupHighScoreNameLength + 4));
  }

  return true;
}

bool MilestoneADemo::SaveSetupDat() const {
  std::array<Uint8, kSetupDatSize> bytes{};
  for (size_t index = 0; index < kSetupSignature.size(); ++index) {
    bytes[kSetupSignatureOffset + index] = kSetupSignature[index];
  }

  WriteLittleEndian32(&bytes, kSetupSoundDeviceOffset, static_cast<std::uint32_t>(std::clamp(sound_device_index_, 0, 5)));
  WriteLittleEndian32(&bytes, kSetupMixingRateOffset, static_cast<std::uint32_t>(std::max(0, mixing_rate_hz_)));
  WriteLittleEndian32(&bytes, kSetupStereoFlagOffset, stereo_flag_ == 0 ? 0U : 1U);
  WriteLittleEndian32(&bytes, kSetupBitDepthFlagOffset, bit_depth_flag_ == 0 ? 0U : 1U);
  for (size_t index = 0; index < keyboard_bindings_.size(); ++index) {
    bytes[kSetupKeyBindingOffset + index] = keyboard_bindings_[index];
  }
  WriteLittleEndian32(&bytes, kSetupMusicVolumeOffset, static_cast<std::uint32_t>(std::clamp(music_volume_, 0, 100)));
  WriteLittleEndian32(&bytes, kSetupSfxVolumeOffset, static_cast<std::uint32_t>(std::clamp(sfx_volume_, 0, 100)));
  WriteLittleEndian32(
      &bytes,
      kSetupTrackIndexOffset,
      static_cast<std::uint32_t>(NormalizeModulo(music_track_index_, static_cast<int>(kMusicTracks.size()))));

  for (size_t index = 0; index < high_scores_.size(); ++index) {
    const size_t offset = kSetupHighScoreOffset + (index * kSetupHighScoreRecordSize);
    const std::string& name = high_scores_[index].name;
    const size_t name_length = std::min(name.size(), static_cast<size_t>(kSetupHighScoreNameLength));
    for (size_t character_index = 0; character_index < name_length; ++character_index) {
      bytes[offset + character_index] = static_cast<Uint8>(name[character_index]);
    }
    WriteLittleEndian32(&bytes, offset + kSetupHighScoreNameLength, static_cast<std::uint32_t>(std::max(0, high_scores_[index].score)));
    WriteLittleEndian32(
        &bytes,
        offset + kSetupHighScoreNameLength + 4,
        static_cast<std::uint32_t>(std::max(0, high_scores_[index].lines)));
  }

  std::ofstream output(ATET_PORT_SETUP_DAT_PATH, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }

  output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

bool MilestoneADemo::RenderGameplayToTexture(SDL_Renderer* renderer, SDL_Texture* texture) {
  if (renderer == nullptr || texture == nullptr) {
    return false;
  }

  SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
  if (!SDL_SetRenderTarget(renderer, texture)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to set gameplay snapshot target: %s", SDL_GetError());
    return false;
  }

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  RenderGameplay(renderer);

  if (!SDL_SetRenderTarget(renderer, previous_target)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to restore renderer target after snapshot: %s", SDL_GetError());
    return false;
  }
  return true;
}

void MilestoneADemo::RenderSplash(SDL_Renderer* renderer) {
  if (splash_page_ == 0) {
    DrawTexture(renderer, ddd_splash_.texture, ddd_splash_.width, ddd_splash_.height);
  } else {
    DrawTexture(renderer, warning_splash_.texture, warning_splash_.width, warning_splash_.height);
  }
  AdvanceSplashTimers();
}

void MilestoneADemo::RenderTopOutBackdrop(SDL_Renderer* renderer) {
  RenderGameplay(renderer);

  const TextureAsset& topout_face = alert_tiles_[6];  // tile06 = shocked / game-over (RE alert ID 6)
  DrawTextureAt(renderer, topout_face.texture, 24, 132, topout_face.width, topout_face.height);

  const int dissolved_rows =
      std::clamp((top_out_frame_ * kBoardHeight) / kTopOutDissolveFrames, 0, kBoardHeight);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  for (int row = 0; row < dissolved_rows; ++row) {
    const SDL_FRect row_rect = {
        static_cast<float>(kGameplayClearX),
        static_cast<float>(kGameplayClearY + (row * kPieceTileSize)),
        static_cast<float>(kGameplayClearWidth),
        static_cast<float>(kPieceTileSize),
    };
    SDL_RenderFillRect(renderer, &row_rect);
  }

  // Draw the dissolve fountain on top of the cleared (black) rows so the upward
  // spray stays visible above the shrinking stack, matching the original.
  RenderDebrisParticles(renderer);
}

bool MilestoneADemo::CaptureTopOutSavedUnder(SDL_Renderer* renderer) {
  if (renderer == nullptr) {
    return false;
  }

  if (top_out_saved_under_texture_ == nullptr) {
    top_out_saved_under_texture_ = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET,
        kScreenWidth,
        kScreenHeight);
    if (top_out_saved_under_texture_ == nullptr) {
      SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to create top-out saved-under texture: %s", SDL_GetError());
      top_out_saved_under_valid_ = false;
      return false;
    }
  }

  SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
  if (!SDL_SetRenderTarget(renderer, top_out_saved_under_texture_)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to set top-out saved-under target: %s", SDL_GetError());
    top_out_saved_under_valid_ = false;
    return false;
  }

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  SDL_RenderClear(renderer);
  RenderTopOutBackdrop(renderer);

  if (!SDL_SetRenderTarget(renderer, previous_target)) {
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed to restore renderer target after top-out saved-under capture: %s", SDL_GetError());
    top_out_saved_under_valid_ = false;
    return false;
  }

  top_out_saved_under_valid_ = true;
  return true;
}

void MilestoneADemo::RenderTopOut(SDL_Renderer* renderer) {
  // The original game-over overlay is a real saved-under phase: the pixels
  // beneath the GAME OVER overlay are saved once when the overlay first
  // appears, the overlay is composited over that frozen backing, and the saved
  // background is restored when the overlay is removed. It is not a live redraw
  // of the dissolving gameplay each frame. Model that by capturing the backdrop
  // into a backing texture on the first overlay-phase frame, then reusing it
  // (frozen) through overlay removal until the high-score handoff.
  const bool overlay_phase_reached = top_out_frame_ >= kTopOutOverlayStartFrame;
  const bool overlay_visible =
      top_out_frame_ >= kTopOutOverlayStartFrame && top_out_frame_ < kTopOutOverlayEndFrame;

  if (!overlay_phase_reached) {
    // Pre-overlay: the gameplay image is still dissolving live. Advance the
    // dissolve fountain (spawn newly-cleared rows' debris + step particles)
    // before rendering so this frame shows the spray.
    top_out_saved_under_valid_ = false;
    UpdateTopOutDissolve();
    RenderTopOutBackdrop(renderer);
    AdvanceTopOutTimers();
    return;
  }

  if (!top_out_saved_under_valid_) {
    CaptureTopOutSavedUnder(renderer);
  }

  if (top_out_saved_under_valid_) {
    DrawTexture(renderer, top_out_saved_under_texture_, kScreenWidth, kScreenHeight);
  } else {
    // Fallback if a render-target texture is unavailable on this backend.
    RenderTopOutBackdrop(renderer);
  }

  if (overlay_visible) {
    DrawTextureAt(
        renderer,
        game_over_overlay_.texture,
        96,
        82,
        game_over_overlay_.width,
        game_over_overlay_.height);
  }

  AdvanceTopOutTimers();
}

void MilestoneADemo::RenderFrontend(SDL_Renderer* renderer) {
  DrawTexture(renderer, title_screen_.texture, title_screen_.width, title_screen_.height);
  RenderFrontendPointfield(renderer);

  const std::vector<std::string> rows = CurrentFrontendRows();
  int selected_row = -1;
  if (frontend_view_ == FrontendView::kMainMenu || frontend_view_ == FrontendView::kOptions ||
      frontend_view_ == FrontendView::kKeyboardSetup || frontend_view_ == FrontendView::kSoundSetup) {
    selected_row = frontend_selected_row_;
  } else if (frontend_view_ == FrontendView::kHighScores && !rows.empty()) {
    selected_row = static_cast<int>(rows.size()) - 1;
  }

  RenderFrontendRows(renderer, rows, selected_row);
  // The chunk-7 objects keep animating during a screen fade (RenderFrontendPointfield
  // above advances them, matching 0x62e0/0x63b8), but the reveal/menu timers are
  // frozen while the fade runs so the rows appear only after the fade-in
  // completes (the original runs the object fade first, then the menu loop).
  if (screen_fade_ == ScreenFade::kNone) {
    AdvanceFrontendTimers();
  }
}

void MilestoneADemo::RenderFrontendPointfield(SDL_Renderer* renderer) {
  if (frontend_banks_[frontend_current_bank_].empty() || frontend_banks_[frontend_next_bank_].empty()) {
    return;
  }

  const std::vector<FrontendPoint>& current_bank = frontend_banks_[frontend_current_bank_];
  const std::vector<FrontendPoint>& next_bank = frontend_banks_[frontend_next_bank_];
  const int morph_progress =
      std::clamp(frontend_cycle_counter_ - kFrontendSteadyFrames, 0, kFrontendMorphFrames);
  std::vector<Uint8> occupied(kScreenWidth * kScreenHeight);
  const std::int64_t sin_xy = sine_table_[frontend_pointfield_state_.angle_xy & 0x7FF] >> 4;
  const std::int64_t cos_xy = sine_table_[(frontend_pointfield_state_.angle_xy + 0x200) & 0x7FF] >> 4;
  const std::int64_t sin_z = sine_table_[frontend_pointfield_state_.angle_z & 0x7FF] >> 4;
  const std::int64_t cos_z = sine_table_[(frontend_pointfield_state_.angle_z + 0x200) & 0x7FF] >> 4;

  for (int point_index = 0; point_index < kFrontendRecordCount; ++point_index) {
    FrontendPoint active_point = current_bank[point_index];
    if (morph_progress > 0) {
      const FrontendPoint& target_point = next_bank[point_index];
      active_point.x += TruncDiv(static_cast<std::int64_t>(target_point.x - active_point.x) * morph_progress, 128);
      active_point.y += TruncDiv(static_cast<std::int64_t>(target_point.y - active_point.y) * morph_progress, 128);
      active_point.z += TruncDiv(static_cast<std::int64_t>(target_point.z - active_point.z) * morph_progress, 128);
    }

    const std::int64_t rotated = (cos_xy * active_point.x - sin_xy * active_point.y) >> 11;
    const std::int64_t depth_y = sin_xy * active_point.x + cos_xy * active_point.y;
    const std::int64_t projected_x_num = cos_z * rotated - sin_z * active_point.z + frontend_pointfield_state_.origin_x;
    const std::int64_t projected_y_num = depth_y + frontend_pointfield_state_.origin_y;
    const std::int64_t depth = active_point.z * cos_z + rotated * sin_z + frontend_pointfield_state_.origin_z;
    const std::int64_t denominator = (depth + 0x14000000) >> 9;
    if (denominator == 0) {
      continue;
    }

    const int screen_x = TruncDiv(projected_x_num, denominator) + 0xA0;
    const int screen_y = TruncDiv(projected_y_num, denominator) + 0x78;
    if (screen_x < 0 || screen_x >= kScreenWidth || screen_y < 0 || screen_y >= kScreenHeight) {
      continue;
    }

    const size_t screen_index = static_cast<size_t>(screen_y) * kScreenWidth + screen_x;
    if (occupied[screen_index] || (!title_indices_.empty() && title_indices_[screen_index] != 0)) {
      continue;
    }

    int shade = static_cast<int>(((denominator << 9) - 0x14000000) >> 23);
    shade = std::clamp(shade, 0, 15);
    const int color_index = std::clamp(kFrontendPointColorBase - shade, 0, 255);
    const SDL_Color color = title_palette_[color_index];
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
    const SDL_FRect point_rect = {
        static_cast<float>(screen_x),
        static_cast<float>(screen_y),
        1.0f,
        1.0f,
    };
    SDL_RenderFillRect(renderer, &point_rect);
    occupied[screen_index] = 1;
  }

  AdvanceFrontendPointfield();
}

void MilestoneADemo::RenderFrontendRows(
    SDL_Renderer* renderer,
    const std::vector<std::string>& rows,
    int selected_row) const {
  const int revealed_rows = VisibleFrontendRowCount(rows.size());
  const bool steady_selection_pulse = revealed_rows >= static_cast<int>(rows.size());
  const bool high_score_table =
      frontend_view_ == FrontendView::kHighScores || frontend_view_ == FrontendView::kHighScoreEntry;
  for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
    if (static_cast<int>(row_index) >= revealed_rows || rows[row_index].empty()) {
      continue;
    }

    const int reveal_amount =
        steady_selection_pulse && static_cast<int>(row_index) == selected_row ? PulseRevealAmount() : 0x30;
    const int y = kFrontendRowY + static_cast<int>(row_index) * kFrontendRowStep;

    // High-score score rows draw as three aligned columns (name left, score and
    // lines right) instead of a single centered string, matching the original.
    // A left-align at X uses center_x = X + w/2; a right-align at X, X - w/2.
    if (high_score_table && row_index < high_scores_.size()) {
      const HighScoreRecord& record = high_scores_[row_index];
      std::string name = record.name.empty() ? "Nobody" : record.name;
      if (frontend_view_ == FrontendView::kHighScoreEntry &&
          static_cast<int>(row_index) == high_score_entry_row_ && HighScoreEntryInputReady()) {
        name = high_score_entry_name_.empty() ? "_" : high_score_entry_name_ + "_";
      }
      const std::string score = std::to_string(record.score);
      const std::string lines = std::to_string(record.lines);
      RenderFrontendText(renderer, name, kHiscoreNameLeftX + MeasureFrontendText(name) / 2, y, reveal_amount);
      RenderFrontendText(renderer, score, kHiscoreScoreRightX - MeasureFrontendText(score) / 2, y, reveal_amount);
      RenderFrontendText(renderer, lines, kHiscoreLinesRightX - MeasureFrontendText(lines) / 2, y, reveal_amount);
      continue;
    }

    RenderFrontendText(renderer, rows[row_index], kFrontendTextCenterX, y, reveal_amount);
  }
}

void MilestoneADemo::RenderFrontendText(
    SDL_Renderer* renderer,
    const std::string& text,
    int center_x,
    int y,
    int reveal_amount) const {
  if (frontend_font_atlas_.texture == nullptr || reveal_amount <= 0) {
    return;
  }

  int cursor_x = center_x - (MeasureFrontendText(text) / 2);
  const int step = 0x300000 / reveal_amount;
  for (const unsigned char raw_byte : text) {
    const int glyph_code = GlyphCodeForCharacter(raw_byte);
    if (glyph_code == 0) {
      cursor_x += 6;
      continue;
    }

    const int glyph_index = glyph_code - 1;
    const int atlas_x = (glyph_index % kFrontendGlyphAtlasColumns) * kFrontendGlyphWidth;
    const int atlas_y = (glyph_index / kFrontendGlyphAtlasColumns) * kFrontendGlyphHeight;
    // 0x60cc renders into a fixed 16-row (kFrontendRenderHeight) window at the row's
    // y, bottom-anchored. The pulse reveal never exceeds 0x40 (a 16px row), so the
    // tallest breath exactly fills this window -- no clipping and, since 16 < the 17px
    // row pitch, no overlap with the neighbouring rows.
    for (int dest_row = 0; dest_row < kFrontendRenderHeight; ++dest_row) {
      const int sample_row = ((kFrontendRenderHeight - 1 - dest_row) * step) >> 16;
      if (sample_row < 0 || sample_row >= kFrontendGlyphHeight) {
        continue;
      }

      const SDL_FRect source = {
          static_cast<float>(atlas_x),
          static_cast<float>(atlas_y + sample_row),
          static_cast<float>(kFrontendGlyphWidth),
          1.0f,
      };
      const SDL_FRect destination = {
          static_cast<float>(cursor_x),
          static_cast<float>(y + dest_row),
          static_cast<float>(kFrontendGlyphWidth),
          1.0f,
      };
      SDL_RenderTexture(renderer, frontend_font_atlas_.texture, &source, &destination);
    }

    cursor_x += frontend_font_widths_[glyph_index] + 2;
  }
}

void MilestoneADemo::AdvanceFrontendPointfield() {
  frontend_pointfield_state_.origin_x += static_cast<std::int64_t>(sine_table_[frontend_pointfield_state_.phase_x & 0x7FF]) << 3;
  frontend_pointfield_state_.origin_y += static_cast<std::int64_t>(sine_table_[frontend_pointfield_state_.phase_y & 0x7FF]) << 2;
  frontend_pointfield_state_.origin_z += static_cast<std::int64_t>(sine_table_[frontend_pointfield_state_.phase_z & 0x7FF]) << 4;

  const int angle_xy_delta = sine_table_[frontend_pointfield_state_.angle_xy & 0x7FF] >> 14;
  const int angle_z_delta = sine_table_[frontend_pointfield_state_.angle_z & 0x7FF] >> 14;
  frontend_pointfield_state_.phase_x += angle_xy_delta + 3;
  frontend_pointfield_state_.phase_y += angle_xy_delta + 2;
  frontend_pointfield_state_.phase_z += angle_z_delta + 4;
  frontend_pointfield_state_.angle_xy += 5;
  frontend_pointfield_state_.angle_z += 2;

  ++frontend_frame_counter_;
  ++frontend_cycle_counter_;
  if (frontend_cycle_counter_ >= kFrontendSteadyFrames + kFrontendMorphFrames) {
    frontend_current_bank_ = frontend_next_bank_;
    frontend_next_bank_ = (frontend_next_bank_ + 1) % kFrontendBankCount;
    if (frontend_next_bank_ == frontend_current_bank_) {
      frontend_next_bank_ = (frontend_next_bank_ + 1) % kFrontendBankCount;
    }
    frontend_cycle_counter_ = 0;
  }
}

void MilestoneADemo::AdvanceFrontendTimers() {
  ++frontend_view_frame_;
  if (high_score_conceal_frames_ > 0) {
    --high_score_conceal_frames_;
    if (high_score_conceal_frames_ == 0) {
      SetFrontendView(FrontendView::kMainMenu, 0);
    }
  }
  if (frontend_view_ == FrontendView::kCredits) {
    ++credits_page_timer_;
    if (credits_page_timer_ >= kCreditsPageFrames) {
      credits_page_ = (credits_page_ + 1) % static_cast<int>(kCreditsPages.size());
      credits_page_timer_ = 0;
      frontend_view_frame_ = 0;
    }
  }
}

void MilestoneADemo::AdvanceSplashTimers() {
  ++splash_frame_;
  if (splash_frame_ < kSplashPageFrames) {
    return;
  }

  splash_frame_ = 0;
  ++splash_page_;
  if (splash_page_ >= 2) {
    CompleteStartupSplashes();
  }
}

void MilestoneADemo::AdvanceTopOutTimers() {
  ++top_out_frame_;
  if (top_out_frame_ >= kTopOutHandoffFrame) {
    BeginHighScoreNameEntry(top_out_score_, top_out_lines_);
  }
}

void MilestoneADemo::ActivateFrontendSelection() {
  if (frontend_view_ == FrontendView::kSoundSetup) {
    switch (frontend_selected_row_) {
      case 0:
        sound_device_index_ = (sound_device_index_ + 1) % static_cast<int>(kSoundDeviceNames.size());
        PersistSetupState();
        return;
      case 1:
        mixing_rate_hz_ = kMixingRates[(MixingRateIndex(mixing_rate_hz_) + 1) % static_cast<int>(kMixingRates.size())];
        PersistSetupState();
        return;
      case 2:
        stereo_flag_ = stereo_flag_ == 0 ? 1 : 0;
        PersistSetupState();
        return;
      case 3:
        bit_depth_flag_ = bit_depth_flag_ == 0 ? 1 : 0;
        PersistSetupState();
        return;
      case 4:
        BeginStartupSplashes();
        return;
      case 5:
        quit_requested_ = true;
        return;
      default:
        return;
    }
  }

  if (frontend_view_ == FrontendView::kMainMenu) {
    switch (frontend_selected_row_) {
      case 0:
        BeginSceneFade(PendingScene::kGameplayNewGame);
        return;
      case 1:
        SetFrontendView(FrontendView::kOptions, 0);
        return;
      case 2:
        music_track_index_ = (music_track_index_ + 1) % static_cast<int>(kMusicTracks.size());
        PersistSetupState();
        StartCurrentMusic("music-track-change");
        return;
      case 3:
        start_level_ = (start_level_ + 1) % 10;
        return;
      case 4:
        SetFrontendView(FrontendView::kHighScores, 0);
        return;
      case 5:
        SetFrontendView(FrontendView::kCredits, 0);
        return;
      case 6:
        ScheduleExitFade();
        quit_requested_ = true;
        return;
      case 7:
        if (live_game_available_) {
          BeginSceneFade(PendingScene::kGameplayResume);
        }
        return;
      default:
        return;
    }
  }

  if (frontend_view_ == FrontendView::kOptions) {
    if (frontend_selected_row_ == 0) {
      music_volume_ = (music_volume_ + 10) % 110;
      PersistSetupState();
      ApplyMusicVolume();
    } else if (frontend_selected_row_ == 1) {
      sfx_volume_ = (sfx_volume_ + 10) % 110;
      PersistSetupState();
    } else if (frontend_selected_row_ == 2) {
      SetFrontendView(FrontendView::kKeyboardSetup, 0);
    } else if (frontend_selected_row_ == 3) {
      SetFrontendView(FrontendView::kMainMenu, 0);
    }
    return;
  }

  if (frontend_view_ == FrontendView::kKeyboardSetup) {
    if (frontend_selected_row_ >= 0 && frontend_selected_row_ < static_cast<int>(keyboard_bindings_.size())) {
      keyboard_capture_row_ = frontend_selected_row_;
    } else if (frontend_selected_row_ == 5) {
      SetFrontendView(FrontendView::kOptions, 2);
      keyboard_capture_row_ = -1;
    }
  }
}

void MilestoneADemo::MoveFrontendSelection(int delta) {
  const int row_count = SelectableFrontendRowCount();
  if (row_count <= 0) {
    return;
  }

  frontend_selected_row_ = (frontend_selected_row_ + delta + row_count) % row_count;
  EmitAudioEvent("menu-row-move", 1);
}

void MilestoneADemo::TrackKeyDown(SDL_Keycode key) {
  const std::uint8_t original_scancode = OriginalScancodeForKey(key);
  if (original_scancode == 0) {
    return;
  }

  if (!held_original_scancodes_[original_scancode]) {
    held_original_scancode_frames_[original_scancode] = 0;
  }
  held_original_scancodes_[original_scancode] = true;
}

void MilestoneADemo::TrackKeyUp(SDL_Keycode key) {
  const std::uint8_t original_scancode = OriginalScancodeForKey(key);
  if (original_scancode == 0) {
    return;
  }

  held_original_scancodes_[original_scancode] = false;
  held_original_scancode_frames_[original_scancode] = 0;
}

void MilestoneADemo::SetFrontendView(FrontendView view, int selected_row) {
  frontend_view_ = view;
  const int row_count = SelectableFrontendRowCount();
  frontend_selected_row_ = row_count <= 0 ? 0 : std::clamp(selected_row, 0, row_count - 1);
  frontend_view_frame_ = 0;
  high_score_bootstrap_frames_ = 0;
  if (view == FrontendView::kCredits) {
    credits_page_ = 0;
    credits_page_timer_ = 0;
  }
  if (view != FrontendView::kKeyboardSetup) {
    keyboard_capture_row_ = -1;
  }
  if (view != FrontendView::kHighScores && view != FrontendView::kHighScoreEntry) {
    high_score_conceal_frames_ = 0;
  }
  if (view != FrontendView::kHighScoreEntry) {
    high_score_entry_commit_pending_ = false;
  }
}

void MilestoneADemo::BeginSceneFade(PendingScene target) {
  pending_scene_ = target;
  if (!fades_enabled_) {
    CompleteSceneSwitch();  // instant switch for --no-fade / demo shortcuts
    return;
  }
  screen_fade_ = ScreenFade::kOut;
  screen_fade_frame_ = 0;
}

void MilestoneADemo::AdvanceScreenFade() {
  ++screen_fade_frame_;
  if (screen_fade_frame_ < kScreenFadeTicks) {
    return;
  }
  screen_fade_frame_ = 0;
  if (screen_fade_ == ScreenFade::kOut) {
    CompleteSceneSwitch();  // swap content at full black, then fade the new scene in
    screen_fade_ = ScreenFade::kIn;
  } else {
    screen_fade_ = ScreenFade::kNone;
  }
}

void MilestoneADemo::CompleteSceneSwitch() {
  switch (pending_scene_) {
    case PendingScene::kFrontendFromGameplay:
      scene_ = Scene::kFrontend;
      SetFrontendView(FrontendView::kMainMenu, 0);
      live_game_available_ = true;
      break;
    case PendingScene::kGameplayNewGame:
      ResetLiveGameplayState();
      live_game_active_ = true;
      live_game_available_ = true;
      scene_ = Scene::kGameplay;
      StartCurrentMusic("music-continue-new-game");
      break;
    case PendingScene::kGameplayResume:
      RestoreLiveSnapshot();
      live_game_active_ = true;
      scene_ = Scene::kGameplay;
      StartCurrentMusic("music-continue-return-to-game");
      break;
    case PendingScene::kNone:
      break;
  }
  pending_scene_ = PendingScene::kNone;
}

int MilestoneADemo::screen_fade_alpha() const {
  if (screen_fade_ == ScreenFade::kOut) {
    return std::clamp(screen_fade_frame_ * 255 / kScreenFadeTicks, 0, 255);
  }
  if (screen_fade_ == ScreenFade::kIn) {
    return std::clamp(255 - (screen_fade_frame_ * 255 / kScreenFadeTicks), 0, 255);
  }
  // Boot splash pages fade in over the first kSplashFadeTicks frames and out
  // over the last, holding fully visible between (RE 0x2998).
  if (fades_enabled_ && scene_ == Scene::kSplash) {
    if (splash_frame_ < kSplashFadeTicks) {
      return std::clamp(255 - (splash_frame_ * 255 / kSplashFadeTicks), 0, 255);
    }
    const int out_start = kSplashPageFrames - kSplashFadeTicks;
    if (splash_frame_ >= out_start) {
      return std::clamp((splash_frame_ - out_start) * 255 / kSplashFadeTicks, 0, 255);
    }
  }
  return 0;
}

void MilestoneADemo::BeginStartupSplashes() {
  scene_ = Scene::kSplash;
  splash_page_ = 0;
  splash_frame_ = 0;
  keyboard_capture_row_ = -1;
}

void MilestoneADemo::CompleteStartupSplashes() {
  scene_ = Scene::kFrontend;
  splash_page_ = 0;
  splash_frame_ = 0;
  SetFrontendView(FrontendView::kMainMenu, 0);
  StartCurrentMusic("music-start-after-startup");
}

bool MilestoneADemo::ApplyFrontendHorizontalEdit(int direction, bool allow_toggle_rows) {
  direction = direction < 0 ? -1 : 1;
  if (frontend_view_ == FrontendView::kOptions && frontend_selected_row_ == 0) {
    music_volume_ = std::clamp(music_volume_ + (direction * 10), 0, 100);
    PersistSetupState();
    ApplyMusicVolume();
    return true;
  }
  if (frontend_view_ == FrontendView::kOptions && frontend_selected_row_ == 1) {
    sfx_volume_ = std::clamp(sfx_volume_ + (direction * 10), 0, 100);
    PersistSetupState();
    return true;
  }
  if (frontend_view_ == FrontendView::kSoundSetup) {
    if (frontend_selected_row_ == 0) {
      sound_device_index_ =
          NormalizeModulo(sound_device_index_ + direction, static_cast<int>(kSoundDeviceNames.size()));
      PersistSetupState();
      return true;
    }
    if (frontend_selected_row_ == 1) {
      const int rate_index =
          NormalizeModulo(MixingRateIndex(mixing_rate_hz_) + direction, static_cast<int>(kMixingRates.size()));
      mixing_rate_hz_ = kMixingRates[rate_index];
      PersistSetupState();
      return true;
    }
    if (allow_toggle_rows && frontend_selected_row_ == 2) {
      stereo_flag_ = stereo_flag_ == 0 ? 1 : 0;
      PersistSetupState();
      return true;
    }
    if (allow_toggle_rows && frontend_selected_row_ == 3) {
      bit_depth_flag_ = bit_depth_flag_ == 0 ? 1 : 0;
      PersistSetupState();
      return true;
    }
  }

  return false;
}

void MilestoneADemo::PersistSetupState() const {
  if (!SaveSetupDat()) {
    SDL_LogWarn(
        SDL_LOG_CATEGORY_APPLICATION,
        "Failed to save SETUP.DAT state: %s",
        ATET_PORT_SETUP_DAT_PATH);
  }
}

void MilestoneADemo::EmitAudioEvent(const char* event_name, int slot, int pan, int voice_class, int sfx_volume_override) {
  if (event_name == nullptr) {
    return;
  }
  audio_events_.push_back(AudioEvent{
      event_name,
      true,
      slot,
      music_track_index_,
      music_volume_,
      sfx_volume_override >= 0 ? sfx_volume_override : sfx_volume_,
      pan,
      voice_class});
}

int MilestoneADemo::PanForPieceColumn(int piece_x) const {
  // Exact slot-0 pan curve recovered from the piece-lock caller at 0x0d4e:
  //   ecx = (pieceX - 5) * 21 + 0x80
  // (centered at column 5, ~21 units of pan bias per column). Clamped to 0..255.
  return std::clamp((piece_x - 5) * 21 + 0x80, 0, 255);
}

void MilestoneADemo::EmitMusicEvent(const char* event_name) {
  if (event_name == nullptr) {
    return;
  }
  audio_events_.push_back(AudioEvent{
      event_name,
      false,
      0,
      music_track_index_,
      music_volume_,
      sfx_volume_});
}

void MilestoneADemo::FlushAudioEvents() {
  for (const AudioEvent& event : audio_events_) {
    if (audio_event_callback_) {
      audio_event_callback_(AudioEventNotification{
          event.name,
          event.is_sfx,
          event.slot,
          event.track,
          event.music_volume,
          event.sfx_volume,
          event.pan,
          event.voice_class});
    }
    if (event.is_sfx) {
      SDL_Log(
          "Audio event: %s -> slot %d at SFX volume %d pan %d class %d",
          event.name.c_str(),
          event.slot,
          event.sfx_volume,
          event.pan,
          event.voice_class);
    } else {
      SDL_Log("Audio event: %s -> track %d at music volume %d", event.name.c_str(), event.track, event.music_volume);
    }
  }
  audio_events_.clear();
}

void MilestoneADemo::StartCurrentMusic(const char* reason) {
  EmitMusicEvent(reason == nullptr ? "music-start-current-track" : reason);
}

void MilestoneADemo::ScheduleExitFade() {
  EmitMusicEvent("exit-fade");
}

void MilestoneADemo::ApplyMusicVolume() {
  EmitMusicEvent("music-volume-apply");
}

void MilestoneADemo::BeginHighScoreConcealToMain() {
  if (high_score_conceal_frames_ <= 0) {
    high_score_conceal_frames_ = kHighScoreConcealFrames;
    EmitMusicEvent("high-score-conceal-to-main");
  }
}

bool MilestoneADemo::HandleHighScoreNameEntry(SDL_Keycode key) {
  if (high_score_entry_row_ < 0 || high_score_entry_row_ >= static_cast<int>(high_scores_.size())) {
    SetFrontendView(FrontendView::kHighScores, 0);
    return true;
  }

  if (key == SDLK_ESCAPE) {
    SetFrontendView(FrontendView::kHighScores, 0);
    high_score_entry_row_ = -1;
    high_score_entry_name_.clear();
    high_score_entry_lowercase_ = false;
    high_score_entry_commit_pending_ = false;
    return true;
  }

  if (!HighScoreEntryInputReady()) {
    return true;
  }

  if (key == SDLK_RETURN) {
    high_score_entry_commit_pending_ = true;
    return true;
  }

  if (key == SDLK_BACKSPACE) {
    if (!high_score_entry_name_.empty()) {
      high_score_entry_name_.pop_back();
    }
    return true;
  }

  if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) {
    high_score_entry_lowercase_ = !high_score_entry_lowercase_;
    return true;
  }

  if (high_score_entry_name_.size() >= 10) {
    return true;
  }

  if (key >= SDLK_A && key <= SDLK_Z) {
    const char base = high_score_entry_lowercase_ ? 'a' : 'A';
    high_score_entry_name_.push_back(static_cast<char>(base + (key - SDLK_A)));
    return true;
  }

  if (key >= SDLK_0 && key <= SDLK_9) {
    high_score_entry_name_.push_back(static_cast<char>('0' + (key - SDLK_0)));
    return true;
  }

  if (key == SDLK_SPACE && !high_score_entry_name_.empty()) {
    high_score_entry_name_.push_back(' ');
    return true;
  }
  if (key == SDLK_SPACE) {
    return true;
  }

  return false;
}

void MilestoneADemo::CommitHighScoreNameEntry() {
  if (high_score_entry_row_ < 0 || high_score_entry_row_ >= static_cast<int>(high_scores_.size())) {
    high_score_entry_commit_pending_ = false;
    return;
  }

  high_scores_[high_score_entry_row_].name = high_score_entry_name_.empty() ? "Player" : high_score_entry_name_;
  PersistSetupState();
  SetFrontendView(FrontendView::kHighScores, 0);
  high_score_entry_row_ = -1;
  high_score_entry_name_.clear();
  high_score_entry_lowercase_ = false;
  high_score_entry_commit_pending_ = false;
}

void MilestoneADemo::CaptureKeyboardBinding(std::uint8_t original_scancode) {
  if (keyboard_capture_row_ < 0 || keyboard_capture_row_ >= static_cast<int>(keyboard_bindings_.size())) {
    keyboard_capture_row_ = -1;
    return;
  }

  keyboard_bindings_[keyboard_capture_row_] = original_scancode;
  keyboard_capture_row_ = -1;
  PersistSetupState();
}

void MilestoneADemo::BeginHighScoreNameEntry(int score, int lines) {
  top_out_handoff_pending_ = false;
  const bool use_state9_bootstrap =
      scene_ == Scene::kTopOut || gameplay_phase_ == GameplayPhase::kTopOutDissolve;
  if (use_state9_bootstrap) {
    StartCurrentMusic("music-continue-topout-highscore");
  }
  int insert_index = -1;
  for (size_t index = 0; index < high_scores_.size(); ++index) {
    if (score > high_scores_[index].score) {
      insert_index = static_cast<int>(index);
      break;
    }
  }

  if (insert_index < 0) {
    scene_ = Scene::kFrontend;
    SetFrontendView(FrontendView::kHighScores, 0);
    high_score_entry_row_ = -1;
    high_score_entry_name_.clear();
    high_score_entry_lowercase_ = false;
    high_score_entry_commit_pending_ = false;
    high_score_bootstrap_frames_ = use_state9_bootstrap ? kState9BootstrapFrames : 0;
    return;
  }

  for (int index = static_cast<int>(high_scores_.size()) - 1; index > insert_index; --index) {
    high_scores_[index] = high_scores_[index - 1];
  }

  high_scores_[insert_index] = HighScoreRecord{"", score, lines};
  high_score_entry_row_ = insert_index;
  high_score_entry_name_.clear();
  high_score_entry_lowercase_ = false;
  high_score_entry_commit_pending_ = false;
  scene_ = Scene::kFrontend;
  SetFrontendView(FrontendView::kHighScoreEntry, 0);
  high_score_bootstrap_frames_ = use_state9_bootstrap ? kState9BootstrapFrames : 0;
}

void MilestoneADemo::BeginTopOutDemo(int score, int lines) {
  top_out_frame_ = 0;
  gameplay_top_out_frame_ = 0;
  top_out_dissolved_rows_ = 0;
  // Clear the gameplay warning band + alert so the backdrop's RenderGameplay
  // stops drawing a mood face; the top-out (shocked, tile06) face takes over.
  warning_band_ = 0;
  alert_effect_id_ = -1;
  alert_lifetime_ = 0;
  alert_reveal_frame_ = 0;
  top_out_saved_under_valid_ = false;
  top_out_score_ = score;
  top_out_lines_ = lines;
  top_out_handoff_pending_ = false;
  gameplay_phase_ = GameplayPhase::kTopOutDissolve;
  live_game_available_ = false;
  EmitAudioEvent("top-out", 5);
  scene_ = Scene::kTopOut;
}

void MilestoneADemo::BeginLiveGameplayDemo(int simulated_clear_count) {
  ResetLiveGameplayState();
  if (simulated_clear_count > 0) {
    InjectFilledRowsForSmoke(std::clamp(simulated_clear_count, 1, 4));
  }
  scene_ = Scene::kGameplay;
}

bool MilestoneADemo::TryMoveLivePiece(int delta_x, int delta_y) {
  if (gameplay_phase_ != GameplayPhase::kFalling) {
    return false;
  }
  LivePiece moved = live_piece_;
  moved.x += delta_x;
  moved.y += delta_y;
  if (!CanPlaceLivePiece(moved)) {
    return false;
  }

  live_piece_ = moved;
  if (delta_x != 0 || delta_y < 0) {
    lock_frames_ = 0;
  }
  return true;
}

bool MilestoneADemo::TryRotateLivePiece(int delta_rotation) {
  if (gameplay_phase_ != GameplayPhase::kFalling) {
    return false;
  }
  LivePiece rotated = live_piece_;
  rotated.rotation = NormalizeModulo(rotated.rotation + delta_rotation, 4);
  if (!CanPlaceLivePiece(rotated)) {
    return false;
  }

  live_piece_ = rotated;
  lock_frames_ = 0;
  return true;
}

bool MilestoneADemo::CanPlaceLivePiece(const LivePiece& piece) const {
  const std::array<CellOffset, 4> cells = RotatedCells(piece.type, piece.rotation);
  for (const CellOffset& cell : cells) {
    const int board_x = piece.x + cell.x;
    const int board_y = piece.y + cell.y;
    if (board_x < 0 || board_x >= kBoardWidth || board_y >= kBoardHeight) {
      return false;
    }
    if (board_y >= 0 && live_board_cells_[static_cast<size_t>(board_y * kBoardWidth + board_x)] != 0) {
      return false;
    }
  }

  return true;
}

void MilestoneADemo::ResetLiveGameplayState() {
  live_game_active_ = true;
  live_game_available_ = true;
  live_board_cells_.fill(0);
  live_piece_stats_.fill(0);
  live_score_ = 0;
  live_lines_ = 0;
  live_level_ = start_level_;
  gameplay_random_state_ = piece_seed_;
  gameplay_random_table_.fill(0);
  gameplay_random_index_ = 0;
  gameplay_phase_ = GameplayPhase::kFalling;
  gravity_accumulator_ = 0;
  lock_frames_ = 0;
  down_lockout_frames_ = 0;
  pending_clear_rows_.clear();
  collapse_frame_ = 0;
  gameplay_top_out_frame_ = 0;
  warning_band_ = 0;
  warning_cooldowns_.fill(0);
  debris_particles_.clear();
  live_snapshot_ = LiveSnapshot{};
  last_clear_was_four_ = false;
  sleepy_ = false;
  frames_since_clear_ = 0;
  alert_effect_id_ = -1;
  alert_lifetime_ = 0;
  alert_reveal_frame_ = 0;
  live_piece_ = LivePiece{0, 0, 4, 0};
  RefillGameplayRandomTable();
  live_piece_ = LivePiece{RollNextPieceType(), 0, 4, 0};
  live_next_piece_type_ = RollNextPieceType();
  line_clear_demo_count_ = 0;
  ++live_piece_stats_[static_cast<size_t>(live_piece_.type)];
}

bool MilestoneADemo::SpawnNextLivePiece() {
  live_piece_ = LivePiece{live_next_piece_type_, 0, 4, 0};
  live_next_piece_type_ = RollNextPieceType();
  gravity_accumulator_ = 0;
  lock_frames_ = 0;
  down_lockout_frames_ = kGameplayDownLockoutFrames;
  ++live_piece_stats_[static_cast<size_t>(live_piece_.type)];
  if (!CanPlaceLivePiece(live_piece_)) {
    BeginTopOutDemo(live_score_, live_lines_);
    return false;
  }
  return true;
}

void MilestoneADemo::CommitLivePieceToBoard() {
  const std::array<CellOffset, 4> cells = RotatedCells(live_piece_.type, live_piece_.rotation);
  for (const CellOffset& cell : cells) {
    const int board_x = live_piece_.x + cell.x;
    const int board_y = live_piece_.y + cell.y;
    if (board_x < 0 || board_x >= kBoardWidth || board_y < 0 || board_y >= kBoardHeight) {
      continue;
    }
    live_board_cells_[static_cast<size_t>(board_y * kBoardWidth + board_x)] =
        NormalizeModulo(live_piece_.type, static_cast<int>(kTetrominoShapes.size())) + 1;
  }
}

void MilestoneADemo::StepLiveGameplay() {
  if (!live_game_active_) {
    return;
  }

  StepDebrisParticles();
  ++frames_since_clear_;
  if (!sleepy_ && frames_since_clear_ >= kSleepyNoClearFrames) {
    sleepy_ = true;
    EmitAudioEvent("sleepy-no-clear-timeout", 6);
  }
  if (down_lockout_frames_ > 0) {
    --down_lockout_frames_;
  }
  for (int& cooldown : warning_cooldowns_) {
    if (gameplay_phase_ != GameplayPhase::kCollapse && cooldown > 0) {
      --cooldown;
    }
  }

  switch (gameplay_phase_) {
    case GameplayPhase::kFalling:
      StepFallingGameplay();
      break;
    case GameplayPhase::kCollapse:
      StepCollapseGameplay();
      break;
    case GameplayPhase::kTopOutDissolve:
      StepTopOutGameplay();
      break;
  }

  UpdateAlert();
}

void MilestoneADemo::StepFallingGameplay() {
  gravity_accumulator_ += CurrentDropIncrement();
  bool moved_down = false;
  while (gravity_accumulator_ >= kGameplayGravityThreshold) {
    gravity_accumulator_ -= kGameplayGravityThreshold;
    if (TryMoveLivePiece(0, 1)) {
      moved_down = true;
    } else {
      break;
    }
  }

  LivePiece below = live_piece_;
  ++below.y;
  if (CanPlaceLivePiece(below)) {
    if (moved_down) {
      lock_frames_ = 0;
    }
    UpdateStackWarning(true);
    return;
  }

  ++lock_frames_;
  if (lock_frames_ < kGameplayLockFrames) {
    UpdateStackWarning(true);
    return;
  }

  // Piece lock / board impact -> slot 0, spatialized by board column, and played
  // at half the SFX volume (0x6817 caller at 0x0d4e: pan = (col-5)*21+0x80,
  // volume arg = [0x2c6e7] / 2, class 0).
  EmitAudioEvent("piece-lock", 0, PanForPieceColumn(live_piece_.x), 0, sfx_volume_ / 2);
  CommitLivePieceToBoard();
  const std::vector<int> full_rows = DetectFullRows();
  if (!full_rows.empty()) {
    BeginLineCollapse(full_rows);
    return;
  }

  UpdateStackWarning(true);
  last_clear_was_four_ = false;
  SpawnNextLivePiece();
}

void MilestoneADemo::StepCollapseGameplay() {
  ++collapse_frame_;
  if (collapse_frame_ < static_cast<int>(pending_clear_rows_.size()) * kGameplayCollapseFramesPerRow) {
    UpdateStackWarning(false);
    return;
  }

  CollapseMarkedRows();
  ApplyLineClearScore(static_cast<int>(pending_clear_rows_.size()));
  pending_clear_rows_.clear();
  collapse_frame_ = 0;
  gameplay_phase_ = GameplayPhase::kFalling;
  UpdateStackWarning(true);
  SpawnNextLivePiece();
}

void MilestoneADemo::StepTopOutGameplay() {
  ++gameplay_top_out_frame_;
  if (gameplay_top_out_frame_ >= kTopOutHandoffFrame) {
    BeginHighScoreNameEntry(live_score_, live_lines_);
  }
}

int MilestoneADemo::CurrentDropIncrement() const {
  // RE gameplay-input-timing-pass: natural gravity is (level<<9)+0x200 per frame, but
  // while Down is held -- and the post-line-clear/spawn Down-lockout (0x2c783) has
  // expired -- the per-frame increment is OVERRIDDEN to 0x8000, then clamped to the
  // 0x10000 descent threshold. 0x8000 is half the threshold, so soft-drop descends one
  // row every two frames (~35 rows/s at 70Hz). The prior port moved a whole row every
  // frame (~70/s) ON TOP OF gravity, i.e. ~2x too fast -- the "way too fast" report.
  const std::uint8_t down_scan_code = keyboard_bindings_[0];
  const bool down_held =
      down_scan_code != 0 && held_original_scancodes_[down_scan_code] && down_lockout_frames_ == 0;
  const int increment = down_held ? kSoftDropIncrement : CurrentGravityRate();
  return std::min(increment, kGameplayGravityThreshold);
}

void MilestoneADemo::BeginLineCollapse(const std::vector<int>& rows) {
  pending_clear_rows_ = rows;
  collapse_frame_ = 0;
  gameplay_phase_ = GameplayPhase::kCollapse;
  SeedLineClearDebris(rows);
  const int clear_count = static_cast<int>(rows.size());
  // Capture the pre-clear state that selects the reward/wake mood face.
  const bool was_sleepy = sleepy_;
  const bool was_four = last_clear_was_four_;
  if (sleepy_ && clear_count == 1) {
    EmitAudioEvent("sleepy-wake-1-line", 8);
  }
  sleepy_ = false;
  frames_since_clear_ = 0;
  if (clear_count == 1) {
    EmitAudioEvent("line-clear-1", 9);
  } else if (clear_count == 4) {
    EmitAudioEvent(last_clear_was_four_ ? "line-clear-4-back-to-back" : "line-clear-4", last_clear_was_four_ ? 11 : 10);
  } else {
    EmitAudioEvent("line-clear-2-or-3", 7);
  }
  last_clear_was_four_ = clear_count == 4;
  // Reward/wake mood face (chunk-6 alert tile), overriding the ambient warning
  // for its lifetime (RE alert-id-correlation table).
  TriggerAlert(AlertIdForClear(clear_count, was_four, was_sleepy), kAlertEventLifetime);
}

std::vector<int> MilestoneADemo::DetectFullRows() const {
  std::vector<int> rows;
  for (int y = 0; y < kBoardHeight; ++y) {
    bool full = true;
    for (int x = 0; x < kBoardWidth; ++x) {
      if (live_board_cells_[static_cast<size_t>(y * kBoardWidth + x)] == 0) {
        full = false;
        break;
      }
    }
    if (full) {
      rows.push_back(y);
    }
  }
  return rows;
}

void MilestoneADemo::ApplyLineClearScore(int clear_count) {
  static constexpr std::array<int, 5> kBaseClearScores = {0, 100, 300, 600, 1200};
  clear_count = std::clamp(clear_count, 1, 4);
  live_score_ += (live_level_ + 1) * kBaseClearScores[clear_count];
  live_lines_ += clear_count;
  live_level_ = start_level_ + (live_lines_ / 10);
}

void MilestoneADemo::CollapseMarkedRows() {
  std::array<bool, kBoardHeight> clear_mask{};
  for (const int row : pending_clear_rows_) {
    if (row >= 0 && row < kBoardHeight) {
      clear_mask[static_cast<size_t>(row)] = true;
    }
  }

  int write_y = kBoardHeight - 1;
  for (int read_y = kBoardHeight - 1; read_y >= 0; --read_y) {
    if (clear_mask[static_cast<size_t>(read_y)]) {
      continue;
    }
    for (int x = 0; x < kBoardWidth; ++x) {
      live_board_cells_[static_cast<size_t>(write_y * kBoardWidth + x)] =
          live_board_cells_[static_cast<size_t>(read_y * kBoardWidth + x)];
    }
    --write_y;
  }
  while (write_y >= 0) {
    for (int x = 0; x < kBoardWidth; ++x) {
      live_board_cells_[static_cast<size_t>(write_y * kBoardWidth + x)] = 0;
    }
    --write_y;
  }
}

void MilestoneADemo::SeedLineClearDebris(const std::vector<int>& rows) {
  const int style = NormalizeModulo(live_level_, 10);
  for (const int row : rows) {
    if (row < 0 || row >= kBoardHeight) {
      continue;
    }
    // The original sweeps the cleared row's 8px band as 8 rows x 80 columns
    // (0x1414 helpers), sampling each on-screen pixel. The port samples the
    // matching tile pixel of the ten filled cells (10 * 8 = 80 columns) in the
    // same order, so the per-column RNG consumption matches.
    for (int row8 = 0; row8 < kPieceTileSize; ++row8) {
      for (int col = 0; col < kGameplayClearWidth; ++col) {
        const int board_col = col / kPieceTileSize;
        const int pixel_x = col % kPieceTileSize;
        const int cell = live_board_cells_[static_cast<size_t>(row * kBoardWidth + board_col)];
        int source_palette_index = 0;
        if (cell != 0) {
          const int tile = std::clamp(cell - 1, 0, 6);
          const size_t atlas_index = static_cast<size_t>(row8 * 56 + (tile * kPieceTileSize) + pixel_x);
          if (atlas_index < piece_atlas_indices_.size()) {
            source_palette_index = piece_atlas_indices_[atlas_index];
          }
        }
        EmitLineClearParticle(
            style,
            col,
            row8,
            kGameplayClearX + col,
            kGameplayClearY + (row * kPieceTileSize) + row8,
            source_palette_index);
      }
    }
  }
}

void MilestoneADemo::EmitLineClearParticle(int style, int col, int row8, int screen_x, int screen_y, int color) {
  // Draw the family's RNG for every swept column (matching the original per-pixel
  // consumption), then spawn only for a visible sampled pixel. The RNG-burst
  // families (0/1/2) draw from the certified shared gameplay RNG.
  const bool visible =
      color != 0 && particle_ramps_[static_cast<size_t>(std::clamp(color, 0, 255)) * 4] != 0;

  switch (style) {
    case 0: {  // 0x14b4: random polar burst (3 draws: life, angle, speed)
      const int life = static_cast<int>(NextGameplayRandomU32() & 0x3f) + 0x10;
      const int angle = static_cast<int>(NextGameplayRandomU32() & 0x7ff);
      const int speed = static_cast<int>(NextGameplayRandomU32() & 0x1ff) + 0x80;
      if (visible) SpawnDebrisPolar(screen_x, screen_y, speed, angle, life, color);
      break;
    }
    case 1: {  // 0x158c: horizontal spray, direction by band-row parity (2 draws: life, speed)
      const int life = static_cast<int>(NextGameplayRandomU32() & 0x3f) + 0x10;
      const int speed = static_cast<int>(NextGameplayRandomU32() & 0x1ff) + 0x80;
      if (visible) SpawnDebrisPolar(screen_x, screen_y, speed, (row8 & 1) ? 0x400 : 0, life, color);
      break;
    }
    case 2: {  // 0x1694: column-swept fan (2 draws: life, speed)
      const int life = static_cast<int>(NextGameplayRandomU32() & 0x3f) + 0x10;
      const int speed = static_cast<int>(NextGameplayRandomU32() & 0x1ff) + 0x80;
      if (visible) SpawnDebrisPolar(screen_x, screen_y, speed, 0x188 + col * 3, life, color);
      break;
    }
    case 3:  // 0x1770: deterministic fan
      if (visible) SpawnDebrisPolar(screen_x, screen_y, 0x100, 0xc0 + col * 8, 0x40, color);
      break;
    case 4:  // 0x1830: target sweep (targetY = 8*row+0xf; screen_y = 21 + 8*row + row8)
      if (visible) SpawnDebrisTarget(screen_x, screen_y, 0x1f + col * 3, screen_y - 6 - row8, 0x40, color);
      break;
    case 5:  // 0x192c: converge to a fixed point
      if (visible) SpawnDebrisTarget(screen_x, screen_y, 0x95, 0xd5, 0x30, color);
      break;
    case 6:  // 0x19dc: fixed direction, column-ramped speed
      if (visible) SpawnDebrisPolar(screen_x, screen_y, 0x214 + col * 4, 0x600, 0x30, color);
      break;
    case 7:  // 0x1a9c: converge to a vertical line (targetY = 8*row+0x18)
      if (visible) SpawnDebrisTarget(screen_x, screen_y, 0x94, screen_y + 3 - row8, 0x30, color);
      break;
    case 8:  // 0x1b54: horizontal split at column 0x28
      if (visible) SpawnDebrisPolar(screen_x, screen_y, 0x80, col < 0x28 ? 0 : 0x400, 0x30, color);
      break;
    default:  // 9, 0x1c40: two-band split at column 0x28
      if (visible) SpawnDebrisPolar(screen_x, screen_y, 0xa0, col < 0x28 ? 0x100 : 0x500, 0x30, color);
      break;
  }
}

void MilestoneADemo::SpawnDebrisPolar(int screen_x, int screen_y, int speed, int angle, int lifetime, int color) {
  if (debris_particles_.size() >= kGameplayMaxDebrisParticles) {
    return;
  }
  // Polar velocity (0x2f78): v = -(sine[idx] * speed) >> 7 with the original's
  // 0x8001 sine amplitude. sine_table_ here has amplitude 0x10000 (2x), so shift
  // by 8 to reproduce the same velocity scale. Position/velocity are 16.16.
  const int cos_index = (angle + 0x200) & 0x7FF;
  const int sin_index = angle & 0x7FF;
  const int vx = -static_cast<int>((static_cast<std::int64_t>(sine_table_[cos_index]) * speed) >> 8);
  const int vy = -static_cast<int>((static_cast<std::int64_t>(sine_table_[sin_index]) * speed) >> 8);
  const int decay = kDebrisAgeFull / std::max(1, lifetime);
  debris_particles_.push_back(DebrisParticle{screen_x << 16, screen_y << 16, vx, vy, 0, decay, color});
}

void MilestoneADemo::SpawnDebrisTarget(int screen_x, int screen_y, int target_x, int target_y, int lifetime, int color) {
  if (debris_particles_.size() >= kGameplayMaxDebrisParticles) {
    return;
  }
  // Target velocity (0x3034): v = (target - start) * 65536 / lifetime.
  const int life = std::max(1, lifetime);
  const int vx = ((target_x - screen_x) << 16) / life;
  const int vy = ((target_y - screen_y) << 16) / life;
  const int decay = kDebrisAgeFull / life;
  debris_particles_.push_back(DebrisParticle{screen_x << 16, screen_y << 16, vx, vy, 0, decay, color});
}

void MilestoneADemo::StepDebrisParticles() {
  for (DebrisParticle& particle : debris_particles_) {
    particle.x += particle.vx;
    particle.y += particle.vy;
    particle.age += particle.decay;
  }
  debris_particles_.erase(
      std::remove_if(
          debris_particles_.begin(),
          debris_particles_.end(),
          [](const DebrisParticle& particle) { return particle.age >= kDebrisAgeFull; }),
      debris_particles_.end());
}

void MilestoneADemo::RenderDebrisParticles(SDL_Renderer* renderer) const {
  for (const DebrisParticle& particle : debris_particles_) {
    const int stage = std::clamp(particle.age >> 8, 0, 3);  // 4 stages of 0x100
    const Uint8 ramp_index = particle_ramps_[static_cast<size_t>(std::clamp(particle.color, 0, 255)) * 4 +
                                              static_cast<size_t>(stage)];
    if (ramp_index == 0) {
      continue;
    }
    const SDL_Color color = gameplay_palette_[ramp_index];
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
    const SDL_FRect debris_rect = {
        static_cast<float>(particle.x >> 16),
        static_cast<float>(particle.y >> 16),
        1.0f,
        1.0f,
    };
    SDL_RenderFillRect(renderer, &debris_rect);
  }
}

// GAP A: the original's top-out dissolve is not a silent black-out. As the well
// is cleared top-down, each row erupts into an upward particle fountain of its
// own cell colors (the line-clear/alert helper family), then the well empties.
// Drive that off the same dissolve front the black-out uses: for each row that
// newly crosses into the dissolved region, spray the certified debris pool
// upward from that row's filled cells.
void MilestoneADemo::UpdateTopOutDissolve() {
  // RE 0x1fc6: the dissolve front advances ONE pixel-row per frame down the whole
  // gameplay area (kBoardHeight*8 pixel rows), spraying that row's debris as it passes.
  // A pixel-row cascade (not board-row bursts) is what makes the next row start
  // exploding while the previous is still mid-air -- the continuous teardown the
  // original shows, instead of "one chunk at a time".
  const int total_pixel_rows = kBoardHeight * kPieceTileSize;  // 160
  const int dissolved =
      std::clamp((top_out_frame_ * total_pixel_rows) / kTopOutDissolveFrames, 0, total_pixel_rows);
  for (int pixel_row = top_out_dissolved_rows_; pixel_row < dissolved; ++pixel_row) {
    SpawnTopOutPixelRow(pixel_row);
  }
  top_out_dissolved_rows_ = dissolved;
  StepDebrisParticles();
}

void MilestoneADemo::SpawnTopOutPixelRow(int pixel_row) {
  const int board_row = pixel_row / kPieceTileSize;
  const int row8 = pixel_row % kPieceTileSize;
  if (board_row < 0 || board_row >= kBoardHeight) {
    return;
  }
  // RE 0x1edc (the dissolve worker, called once per pixel-row): scan EVERY OTHER pixel
  // across the 80px well, sample the on-screen colour, and for non-zero pixels spawn
  // ONE polar particle (0x2f78) at a FULL 360-degree random launch angle -- a wide,
  // fast omnidirectional scatter, not an upward fountain. Three RNG draws per particle
  // in order lifetime, angle, speed: life=(rand&0x3f)+0x10, angle=rand&0x7ff,
  // speed=(rand&0x1ff)+0x80 (the sampled pixel is the colour). Exactly line-clear
  // emitter family #0.
  {
    for (int col = 0; col < kGameplayClearWidth; col += 2) {  // every other pixel (40/row)
      const int board_col = col / kPieceTileSize;
      const int pixel_x = col % kPieceTileSize;
      const int cell = live_board_cells_[static_cast<size_t>(board_row * kBoardWidth + board_col)];
      if (cell == 0) {
        continue;
      }
      const int tile = std::clamp(cell - 1, 0, 6);
      const size_t atlas_index = static_cast<size_t>(row8 * 56 + (tile * kPieceTileSize) + pixel_x);
      const int color =
          atlas_index < piece_atlas_indices_.size() ? static_cast<int>(piece_atlas_indices_[atlas_index]) : 0;
      if (color == 0) {  // 0x1edc skips zero pixels before drawing any RNG
        continue;
      }
      const int screen_x = kGameplayClearX + col;
      const int screen_y = kGameplayClearY + (board_row * kPieceTileSize) + row8;
      const int life = static_cast<int>(NextGameplayRandomU32() & 0x3f) + 0x10;
      const int angle = static_cast<int>(NextGameplayRandomU32() & 0x7ff);  // full 360 degrees
      const int speed = static_cast<int>(NextGameplayRandomU32() & 0x1ff) + 0x80;
      SpawnDebrisPolar(screen_x, screen_y, speed, angle, life, color);
    }
  }
}

// Gameplay alert/mood-face controller (RE board-alert-pass 0x2008/0x206c). One-shot
// events (line-clear rewards, wake) arm a tile for a lifetime via TriggerAlert;
// UpdateAlert ages that event and otherwise reflects the ambient state (stack
// warning band, then sleepy). Any change of tile restarts the staged reveal-in.
void MilestoneADemo::TriggerAlert(int effect_id, int lifetime) {
  if (effect_id < 0 || effect_id >= static_cast<int>(alert_tiles_.size())) {
    return;
  }
  if (effect_id != alert_effect_id_) {
    alert_reveal_frame_ = 0;
  }
  alert_effect_id_ = effect_id;
  alert_lifetime_ = std::max(1, lifetime);
}

void MilestoneADemo::UpdateAlert() {
  if (alert_lifetime_ > 0) {
    --alert_lifetime_;
    ++alert_reveal_frame_;
    if (alert_lifetime_ == 0) {
      alert_effect_id_ = -1;
    }
    return;  // a timed event holds over the ambient state until it expires
  }
  int ambient = -1;
  if (warning_band_ > 0) {
    ambient = 2 + warning_band_;  // bands 1/2/3 -> alert tiles 3/4/5
  } else if (sleepy_) {
    ambient = 11;
  }
  if (ambient != alert_effect_id_) {
    alert_reveal_frame_ = 0;
  } else {
    ++alert_reveal_frame_;
  }
  alert_effect_id_ = ambient;
}

int MilestoneADemo::AlertIdForClear(int cleared_rows, bool was_four, bool was_sleepy) const {
  // Matches the recovered 0x09c8 line-clear alert table 0x980 = [2,0,9,7] plus its
  // two overrides: a single-line clear out of the sleepy state wakes to tile 12,
  // and a four-line clear following another four-line is the back-to-back tile 8.
  // (There is no fast-follow-up path in the shipped code; tiles 1/10 are unused.)
  if (was_sleepy && cleared_rows == 1) {
    return 12;
  }
  if (cleared_rows == 4 && was_four) {
    return 8;
  }
  switch (cleared_rows) {
    case 1:
      return 2;
    case 2:
      return 0;
    case 3:
      return 9;
    case 4:
      return 7;
    default:
      return 2;
  }
}

void MilestoneADemo::RenderAlertTile(SDL_Renderer* renderer) const {
  if (scene_ != Scene::kGameplay || alert_effect_id_ < 0 ||
      alert_effect_id_ >= static_cast<int>(alert_tiles_.size())) {
    return;
  }
  const TextureAsset& tile = alert_tiles_[static_cast<size_t>(alert_effect_id_)];
  if (tile.texture == nullptr) {
    return;
  }
  // Staged vertical reveal-in: RE board-alert-pass 0x206c feeds the row table
  // {50,40,30,20,10,1} to 0x17983 as the six-step reveal counter counts down,
  // i.e. rows-from-frame grow 1,10,20,30,40,50 -> full. Top-anchored wipe.
  static constexpr int kRevealRows[6] = {1, 10, 20, 30, 40, 50};
  const int step = std::clamp(alert_reveal_frame_, 0, 6);
  const int revealed = step >= 6 ? tile.height : std::min(kRevealRows[step], tile.height);
  if (revealed <= 0) {
    return;
  }
  const SDL_FRect src = {0.0f, 0.0f, static_cast<float>(tile.width), static_cast<float>(revealed)};
  const SDL_FRect dst = {24.0f, 132.0f, static_cast<float>(tile.width), static_cast<float>(revealed)};
  SDL_RenderTexture(renderer, tile.texture, &src, &dst);
}

void MilestoneADemo::UpdateStackWarning(bool allow_sound) {
  const int highest_row = HighestOccupiedRow();
  int new_band = 0;
  if (highest_row >= 0 && highest_row <= kWarningHighRow) {
    new_band = 3;
  } else if (highest_row >= 0 && highest_row <= kWarningMediumRow) {
    new_band = 2;
  } else if (highest_row >= 0 && highest_row <= kWarningLowRow) {
    new_band = 1;
  }

  warning_band_ = new_band;
  if (!allow_sound || new_band == 0) {
    return;
  }

  const size_t cooldown_index = static_cast<size_t>(new_band - 1);
  if (warning_cooldowns_[cooldown_index] > 0) {
    return;
  }

  // Warnings are centered and routed through the class-1 playback offset (0x6817 EDX=1).
  EmitAudioEvent(new_band == 1 ? "stack-warning-low" : "stack-warning-medium-high", new_band == 1 ? 3 : 4, 0x80, 1);
  warning_cooldowns_[cooldown_index] = kWarningCooldownFrames;
}

int MilestoneADemo::HighestOccupiedRow() const {
  for (int y = 0; y < kBoardHeight; ++y) {
    for (int x = 0; x < kBoardWidth; ++x) {
      if (live_board_cells_[static_cast<size_t>(y * kBoardWidth + x)] != 0) {
        return y;
      }
    }
  }
  return -1;
}

int MilestoneADemo::CurrentGravityRate() const {
  return (std::max(0, live_level_) << 9) + 0x200;
}

// Reproduces the original gameplay RNG, certified bit-exact against the
// decompilation (RE: sfx/rng findings). The table fill mirrors `0x32b0`:
// srand(seed) via `0xed0f`, then fill an 8192-dword table (allocated 0x8000
// bytes at `0x32f0`) with `(rand()<<16)|rand()` using the Watcom C `rand`
// `0xeceb` (LCG 0x41C64E6D/0x3039, returning bits 16..30). The seed source is
// the shared entropy global `0x2d2a3` (no recovered deterministic write, so
// effectively timer-like); the port models it with a settable/default seed.
void MilestoneADemo::RefillGameplayRandomTable() {
  gameplay_random_state_ = piece_seed_;
  gameplay_random_index_ = 0;
  for (std::uint32_t& value : gameplay_random_table_) {
    const std::uint32_t high = NextGameplayRandom15();
    const std::uint32_t low = NextGameplayRandom15();
    value = (high << 16) | low;
  }
}

std::uint32_t MilestoneADemo::NextGameplayRandom15() {
  gameplay_random_state_ = (gameplay_random_state_ * 0x41C64E6DU) + 0x3039U;
  return (gameplay_random_state_ >> 16) & 0x7FFFU;
}

// Consumption mirrors `0x3280`: pre-increment the index, wrap at 0x1FFE (only
// 8190 of the 8192 filled entries are ever cycled), then return that table
// entry. Piece selection is this value modulo 7 (RollNextPieceType).
std::uint32_t MilestoneADemo::NextGameplayRandomU32() {
  ++gameplay_random_index_;
  if (gameplay_random_index_ >= 0x1FFE) {
    gameplay_random_index_ = 0;
  }

  return gameplay_random_table_[static_cast<size_t>(gameplay_random_index_)];
}

void MilestoneADemo::SaveLiveSnapshot() {
  live_snapshot_.valid = live_game_active_;
  live_snapshot_.phase = gameplay_phase_;
  live_snapshot_.board_cells = live_board_cells_;
  live_snapshot_.piece_stats = live_piece_stats_;
  live_snapshot_.piece = live_piece_;
  live_snapshot_.next_piece_type = live_next_piece_type_;
  live_snapshot_.score = live_score_;
  live_snapshot_.lines = live_lines_;
  live_snapshot_.level = live_level_;
  live_snapshot_.gravity_accumulator = gravity_accumulator_;
  live_snapshot_.lock_frames = lock_frames_;
  live_snapshot_.down_lockout_frames = down_lockout_frames_;
  live_snapshot_.collapse_frame = collapse_frame_;
  live_snapshot_.top_out_frame = gameplay_top_out_frame_;
  live_snapshot_.warning_band = warning_band_;
  live_snapshot_.warning_cooldowns = warning_cooldowns_;
  live_snapshot_.last_clear_was_four = last_clear_was_four_;
  live_snapshot_.sleepy = sleepy_;
  live_snapshot_.frames_since_clear = frames_since_clear_;
}

void MilestoneADemo::RestoreLiveSnapshot() {
  if (!live_snapshot_.valid) {
    return;
  }
  gameplay_phase_ = live_snapshot_.phase;
  live_board_cells_ = live_snapshot_.board_cells;
  live_piece_stats_ = live_snapshot_.piece_stats;
  live_piece_ = live_snapshot_.piece;
  live_next_piece_type_ = live_snapshot_.next_piece_type;
  live_score_ = live_snapshot_.score;
  live_lines_ = live_snapshot_.lines;
  live_level_ = live_snapshot_.level;
  gravity_accumulator_ = live_snapshot_.gravity_accumulator;
  lock_frames_ = live_snapshot_.lock_frames;
  down_lockout_frames_ = live_snapshot_.down_lockout_frames;
  collapse_frame_ = live_snapshot_.collapse_frame;
  gameplay_top_out_frame_ = live_snapshot_.top_out_frame;
  warning_band_ = live_snapshot_.warning_band;
  warning_cooldowns_ = live_snapshot_.warning_cooldowns;
  last_clear_was_four_ = live_snapshot_.last_clear_was_four;
  sleepy_ = live_snapshot_.sleepy;
  frames_since_clear_ = live_snapshot_.frames_since_clear;
}

int MilestoneADemo::RollNextPieceType() {
  return static_cast<int>(NextGameplayRandomU32() % kTetrominoShapes.size());
}

void MilestoneADemo::SimulateLiveLineClear(int clear_count) {
  InjectFilledRowsForSmoke(clear_count);
}

void MilestoneADemo::InjectFilledRowsForSmoke(int row_count) {
  if (!live_game_active_) {
    return;
  }
  row_count = std::clamp(row_count, 1, 4);
  for (int row = 0; row < row_count; ++row) {
    const int board_y = kBoardHeight - 1 - row;
    for (int x = 0; x < kBoardWidth; ++x) {
      live_board_cells_[static_cast<size_t>(board_y * kBoardWidth + x)] = ((x + row) % 7) + 1;
    }
  }
  BeginLineCollapse(DetectFullRows());
}

int MilestoneADemo::CurrentRunScore() const {
  if (live_game_active_) {
    return live_score_;
  }

  return 12500 + (gameplay_preset_index_ * 2500);
}

int MilestoneADemo::CurrentRunLines() const {
  if (live_game_active_) {
    return live_lines_;
  }

  return 20 + (gameplay_preset_index_ * 8);
}

void MilestoneADemo::StepGameplayPreset(int delta) {
  gameplay_preset_index_ =
      NormalizeModulo(gameplay_preset_index_ + delta, static_cast<int>(kGameplayPresets.size()));
}

bool MilestoneADemo::ApplyGameplayBinding(std::uint8_t original_scancode, bool allow_down) {
  if (original_scancode == 0) {
    return false;
  }

  if (live_game_active_) {
    if (allow_down && original_scancode == keyboard_bindings_[0]) {
      // Down is consumed here but does not move a row on press; the soft-drop is
      // driven per frame by the gravity accumulator (CurrentDropIncrement) off the
      // held-key state, matching the original's polled 0x8000-per-frame model.
      return true;
    }
    if (original_scancode == keyboard_bindings_[1]) {
      return TryMoveLivePiece(-1, 0);
    }
    if (original_scancode == keyboard_bindings_[2]) {
      return TryMoveLivePiece(1, 0);
    }
    if (original_scancode == keyboard_bindings_[3]) {
      return TryRotateLivePiece(-1);
    }
    if (original_scancode == keyboard_bindings_[4]) {
      return TryRotateLivePiece(1);
    }
    return false;
  }

  if (original_scancode == keyboard_bindings_[1] || original_scancode == keyboard_bindings_[3]) {
    StepGameplayPreset(-1);
    return true;
  }

  if ((allow_down && original_scancode == keyboard_bindings_[0]) || original_scancode == keyboard_bindings_[2] ||
      original_scancode == keyboard_bindings_[4]) {
    StepGameplayPreset(1);
    return true;
  }

  return false;
}

bool MilestoneADemo::HandleGameplayBinding(SDL_Keycode key) {
  return ApplyGameplayBinding(OriginalScancodeForKey(key), true);
}

void MilestoneADemo::ResetSetupStateToDefaults() {
  sound_device_index_ = 0;
  mixing_rate_hz_ = 44100;
  stereo_flag_ = 1;
  bit_depth_flag_ = 1;
  music_track_index_ = 0;
  start_level_ = 0;
  music_volume_ = 100;
  sfx_volume_ = 90;
  keyboard_bindings_ = kDefaultKeyboardBindings;
  for (size_t index = 0; index < high_scores_.size(); ++index) {
    high_scores_[index] = HighScoreRecord{
        kDefaultHighScores[index].name,
        kDefaultHighScores[index].score,
        kDefaultHighScores[index].lines};
  }
}

std::vector<std::string> MilestoneADemo::CurrentFrontendRows() const {
  if (frontend_view_ == FrontendView::kMainMenu) {
    return {
        "New Game",
        "Options",
        std::string("Music:`") + kMusicTracks[music_track_index_] + "'",
        "Level " + std::to_string(start_level_),
        "High Scores",
        "Credits",
        "Exit Game",
        "Return to Game",
    };
  }

  if (frontend_view_ == FrontendView::kOptions) {
    return {
        FormatPercentRow("Music Volume:", music_volume_),
        FormatPercentRow("Sound FX Volume:", sfx_volume_),
        "Keyboard Setup",
        "Back To Main Menu",
        "",
        "",
        "",
        "",
    };
  }

  if (frontend_view_ == FrontendView::kKeyboardSetup) {
    return {
        KeyboardBindingRowText(0),
        KeyboardBindingRowText(1),
        KeyboardBindingRowText(2),
        KeyboardBindingRowText(3),
        KeyboardBindingRowText(4),
        "Back to Options Menu",
        "",
        "",
    };
  }

  if (frontend_view_ == FrontendView::kSoundSetup) {
    return {
        SoundSetupRowText(0),
        SoundSetupRowText(1),
        SoundSetupRowText(2),
        SoundSetupRowText(3),
        "Play Game",
        "Exit to Dos",
        "",
        "",
    };
  }

  if (frontend_view_ == FrontendView::kHighScores || frontend_view_ == FrontendView::kHighScoreEntry) {
    // The original's high-score screen has no "High Scores" heading -- the five
    // score rows sit directly under the shared logo (verified against the running
    // original), starting at the same row Y as the menu's first row.
    return {
        HighScoreRowText(0),
        HighScoreRowText(1),
        HighScoreRowText(2),
        HighScoreRowText(3),
        HighScoreRowText(4),
        "Return to Main Menu",
    };
  }

  std::vector<std::string> rows;
  std::string page = kCreditsPages[credits_page_];
  size_t start = 0;
  while (start <= page.size()) {
    const size_t newline = page.find('\n', start);
    rows.push_back(page.substr(start, newline == std::string::npos ? std::string::npos : newline - start));
    if (newline == std::string::npos) {
      break;
    }
    start = newline + 1;
  }
  return rows;
}

std::string MilestoneADemo::KeyboardBindingRowText(int row_index) const {
  if (row_index < 0 || row_index >= static_cast<int>(keyboard_bindings_.size())) {
    return {};
  }

  std::string row = std::string(kKeyboardBindingLabels[row_index]) + ":";
  if (keyboard_capture_row_ == row_index) {
    row += "_";
  } else {
    row += KeyNameForBinding(keyboard_bindings_[row_index]);
  }
  return row;
}

std::string MilestoneADemo::HighScoreRowText(int row_index) const {
  if (row_index < 0 || row_index >= static_cast<int>(high_scores_.size())) {
    return {};
  }

  const HighScoreRecord& record = high_scores_[row_index];
  std::string name = record.name.empty() ? "Nobody" : record.name;
  if (frontend_view_ == FrontendView::kHighScoreEntry && row_index == high_score_entry_row_ &&
      HighScoreEntryInputReady()) {
    name = high_score_entry_name_.empty() ? "_" : high_score_entry_name_ + "_";
  }
  return name + " " + std::to_string(record.score) + " " + std::to_string(record.lines);
}

std::string MilestoneADemo::SoundSetupRowText(int row_index) const {
  switch (row_index) {
    case 0:
      return std::string("Sound Device:") + kSoundDeviceNames[std::clamp(sound_device_index_, 0, static_cast<int>(kSoundDeviceNames.size()) - 1)];
    case 1:
      return "Mixing Rate:" + std::to_string(mixing_rate_hz_);
    case 2:
      return std::string("Output:") + (stereo_flag_ == 0 ? "Mono" : "Stereo");
    case 3:
      return std::string("Sample Size:") + (bit_depth_flag_ == 0 ? "8 Bit" : "16 Bit");
    default:
      return {};
  }
}

int MilestoneADemo::MeasureFrontendText(const std::string& text) const {
  int width = 0;
  for (const unsigned char raw_byte : text) {
    const int glyph_code = GlyphCodeForCharacter(raw_byte);
    if (glyph_code == 0) {
      width += 6;
    } else {
      width += frontend_font_widths_[glyph_code - 1] + 2;
    }
  }
  return width;
}

int MilestoneADemo::PulseRevealAmount() const {
  // Generic menu-row pulse, decoded exactly from RE 0x3fd8: reveal = sine[phase]/0x800
  // + 0x30, phase advancing 0x20 per frame (period 0x800/0x20 = 64 frames, ~0.91s at
  // 70Hz). The original sine table amplitude is 0x8001, so sine/0x800 spans +/-0x10 =>
  // reveal 0x20..0x40, symmetric around the settled 0x30. Peak 0x40 renders a 16px row
  // (0x40/4) that exactly fills 0x60cc's 16-row window, so it never clips and never
  // exceeds the 17px row pitch -> NO neighbour overlap. IMPORTANT: this port's
  // sine_table_ has amplitude 0x10000 (2x the original's 0x8001), so divide by 0x1000
  // (not 0x800) to reproduce the same +/-0x10 swing. (Earlier passes used 0x40/frame
  // with /0x800 -> a 32-frame 0x10..0x50 swing, 2x too fast AND 2x too tall = clipped
  // + overlapping; then over-corrected to a grow-up-only 0x30..0x48 that still
  // overshot the pitch on middle rows -- the "overlapping everywhere" report.)
  return TruncDiv(sine_table_[(frontend_frame_counter_ * 0x20) & 0x7FF], 0x1000) + 0x30;
}

int MilestoneADemo::FrontendRowRevealDelayFrames() const {
  const bool high_score_view =
      frontend_view_ == FrontendView::kHighScores || frontend_view_ == FrontendView::kHighScoreEntry;
  return kFrontendObjectWarmupFrames + (high_score_view ? high_score_bootstrap_frames_ : 0);
}

int MilestoneADemo::HighScoreBootstrapRemainingFrames() const {
  const bool high_score_view =
      frontend_view_ == FrontendView::kHighScores || frontend_view_ == FrontendView::kHighScoreEntry;
  if (!high_score_view || high_score_bootstrap_frames_ <= 0) {
    return 0;
  }

  return std::max(0, high_score_bootstrap_frames_ - frontend_view_frame_);
}

int MilestoneADemo::RevealedFrontendRowCount(size_t row_count) const {
  if (row_count == 0) {
    return 0;
  }

  const int active_frame = frontend_view_frame_ - FrontendRowRevealDelayFrames();
  if (active_frame < 0) {
    return 0;
  }

  return std::clamp((active_frame / kFrontendRowRevealIntervalFrames) + 1, 0, static_cast<int>(row_count));
}

int MilestoneADemo::VisibleFrontendRowCount(size_t row_count) const {
  const int revealed_rows = RevealedFrontendRowCount(row_count);
  if (high_score_conceal_frames_ <= 0 ||
      (frontend_view_ != FrontendView::kHighScores && frontend_view_ != FrontendView::kHighScoreEntry)) {
    return revealed_rows;
  }

  const int conceal_elapsed = kHighScoreConcealFrames - high_score_conceal_frames_;
  const int concealed_rows =
      std::clamp(conceal_elapsed / kFrontendRowRevealIntervalFrames, 0, static_cast<int>(row_count));
  return std::max(0, revealed_rows - concealed_rows);
}

bool MilestoneADemo::HighScoreEntryInputReady() const {
  return frontend_view_ == FrontendView::kHighScoreEntry &&
         RevealedFrontendRowCount(kHighScoreTableRevealRows) >= kHighScoreTableRevealRows;
}

int MilestoneADemo::SelectableFrontendRowCount() const {
  switch (frontend_view_) {
    case FrontendView::kMainMenu:
      return 8;
    case FrontendView::kOptions:
      return 4;
    case FrontendView::kKeyboardSetup:
      return 6;
    case FrontendView::kSoundSetup:
      return 6;
    case FrontendView::kHighScores:
    case FrontendView::kHighScoreEntry:
    case FrontendView::kCredits:
      return 0;
  }

  return 0;
}

void MilestoneADemo::RenderGameplay(SDL_Renderer* renderer) const {
  DrawTexture(renderer, gameplay_screen_.texture, gameplay_screen_.width, gameplay_screen_.height);

  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  const SDL_FRect preview_clear = {
      static_cast<float>(kPreviewX),
      static_cast<float>(kPreviewY),
      static_cast<float>(kPreviewWidth),
      static_cast<float>(kPreviewHeight),
  };
  SDL_RenderFillRect(renderer, &preview_clear);

  const SDL_FRect well_clear = {
      static_cast<float>(kGameplayClearX),
      static_cast<float>(kGameplayClearY),
      static_cast<float>(kGameplayClearWidth),
      static_cast<float>(kGameplayClearHeight),
  };
  SDL_RenderFillRect(renderer, &well_clear);

  if (live_game_active_) {
    const int high_score_value = high_scores_.empty() ? 0 : high_scores_[0].score;
    DrawCounter(renderer, digit_atlas_.texture, {36, 89, 7, high_score_value});
    DrawCounter(renderer, digit_atlas_.texture, {36, 101, 7, live_score_});
    DrawCounter(renderer, digit_atlas_.texture, {60, 112, 3, live_level_});
    DrawCounter(renderer, digit_atlas_.texture, {144, 195, 4, live_lines_});

    for (size_t i = 0; i < live_piece_stats_.size(); ++i) {
      DrawCounter(renderer, digit_atlas_.texture, {kPieceStatX, kPieceStatRows[i], 4, live_piece_stats_[i]});
    }

    DrawTetromino(
        renderer,
        piece_atlas_.texture,
        live_next_piece_type_,
        live_next_piece_type_,
        kPreviewX,
        kPreviewY);

    for (int y = 0; y < kBoardHeight; ++y) {
      bool collapse_row_hidden = false;
      for (size_t clear_index = 0; clear_index < pending_clear_rows_.size(); ++clear_index) {
        if (pending_clear_rows_[clear_index] == y &&
            collapse_frame_ >= static_cast<int>(clear_index + 1) * kGameplayCollapseFramesPerRow) {
          collapse_row_hidden = true;
          break;
        }
      }
      if (collapse_row_hidden) {
        continue;
      }
      for (int x = 0; x < kBoardWidth; ++x) {
        const int cell = live_board_cells_[static_cast<size_t>(y * kBoardWidth + x)];
        if (cell <= 0) {
          continue;
        }
        DrawPieceTile(
            renderer,
            piece_atlas_.texture,
            cell - 1,
            kGameplayClearX + (x * kPieceTileSize),
            kGameplayClearY + (y * kPieceTileSize));
      }
    }

    DrawLiveTetromino(
        renderer,
        piece_atlas_.texture,
        live_piece_.type,
        live_piece_.rotation,
        live_piece_.x,
        live_piece_.y);

    RenderDebrisParticles(renderer);
    RenderAlertTile(renderer);
    return;
  }

  const GameplayPreset& preset = kGameplayPresets[gameplay_preset_index_];
  DrawCounter(renderer, digit_atlas_.texture, preset.high_score);
  DrawCounter(renderer, digit_atlas_.texture, preset.score);
  CounterLayout level_counter = preset.level;
  level_counter.value = start_level_;
  DrawCounter(renderer, digit_atlas_.texture, level_counter);
  DrawCounter(renderer, digit_atlas_.texture, preset.lines);

  for (size_t i = 0; i < preset.piece_stats.size(); ++i) {
    DrawCounter(renderer, digit_atlas_.texture, {kPieceStatX, kPieceStatRows[i], 4, preset.piece_stats[i]});
  }

  // Milestone A keeps geometry and tile-style selection separate so we can prove chunk-3
  // atlas usage without pretending the exact runtime piece-ID ordering is already ported.
  DrawTetromino(
      renderer,
      piece_atlas_.texture,
      preset.preview_piece.atlas_index,
      preset.preview_piece.shape_index,
      preset.preview_piece.x,
      preset.preview_piece.y);

  for (const PiecePlacement& piece : preset.board_pieces) {
    DrawTetromino(
        renderer,
        piece_atlas_.texture,
        piece.atlas_index,
        piece.shape_index,
        kGameplayClearX + (piece.x * kPieceTileSize),
        kGameplayClearY + (piece.y * kPieceTileSize));
  }
}

}  // namespace atet
