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
#include "audit_instrument.h"

// Audit instrumentation: count the SDL draw API calls the demo issues per frame.
// A function-like macro does not re-expand inside its own replacement, so the
// inner call reaches the real SDL function.
#define SDL_RenderTexture(...) (++::atet::g_audit.render_texture_calls, SDL_RenderTexture(__VA_ARGS__))
#define SDL_RenderFillRect(...) (++::atet::g_audit.fill_rect_calls, SDL_RenderFillRect(__VA_ARGS__))
#define SDL_SetRenderDrawColor(...) (++::atet::g_audit.draw_color_calls, SDL_SetRenderDrawColor(__VA_ARGS__))

namespace atet {

namespace {
// Scoped perf-counter accumulator for the audit sub-phase timers.
struct AuditScopedTicks {
  explicit AuditScopedTicks(std::uint64_t* sink) : sink_(sink), start_(SDL_GetPerformanceCounter()) {}
  ~AuditScopedTicks() { *sink_ += SDL_GetPerformanceCounter() - start_; }
  std::uint64_t* sink_;
  std::uint64_t start_;
};
}  // namespace

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
// High-score table (0x50b0): name, score and lines are each CENTRED (0x60cc with
// ECX = 1) at x 0x69, 0xdc and 0x118; row 5 is empty and the footer is row 6 at
// y 0xca (the generic row y for index 6).
constexpr int kHiscoreNameCenterX = 0x69;
constexpr int kHiscoreScoreCenterX = 0xDC;
constexpr int kHiscoreLinesCenterX = 0x118;
constexpr int kHiscoreFooterRow = 6;
constexpr int kFrontendSteadyFrames = 0x258;   // 0x39c4: next bank picked at counter 600
constexpr int kFrontendMorphFrames = 0x80;     // ... morph 128 steps, switch at 728
constexpr int kFrontendRowCount = 8;
// Frontend timing, all in logic ticks of the PIT clock (STATIC-EXACT, see
// audit-2026-10 frontend-flow / timing-reconciled):
constexpr int kRevealSteps = 0x30;             // 0x3df4 / 0x3ee4: 48 steps
constexpr int kFadeTicks = 0x3F;               // 0x64f0 / 0x63b8 / 0x62e0 / 0x6498 exit at e >= 63
constexpr int kFrontendRestLevel = kFadeTicks - 1;  // 0x63b8's last upload is level 62
constexpr int kPulseStep = 0x20;               // 0x3fd8: +0x20 per steady tick
constexpr int kPulsePhaseMask = 0x7FF;
constexpr int kStalePacerBurst = 6;            // 0x24d0 caps the catch-up at 6 ticks
constexpr int kMusicChangeWaitTicks = 0x20;    // 0x6544: 0x699e(2, 0x20) and its busy wait
constexpr int kExitMusicRampTicks = 0x40;      // state 3: 0x699e(2, 0x40)
// Ticks between the WARNING's last upload and the 0x6544 call (chunk decode and
// page clears at main 0x2eb-0x3cb). The Unicorn oracle (device None, zero-time
// disk) measures 1; DOSBox-X at Pentium-133 speed takes about 5 more. Disk and
// CPU time are not part of the code, so the zero-disk oracle value is used.
constexpr int kBootLoadAllowanceTicks = 1;
constexpr int kSplashFadeInEnd = 0x7F;         // 0x2998: fade-in e = 1..127
constexpr int kSplashHoldTicks = 0x78;         // hold 120 ticks
constexpr int kSplashFadeOutUploads = 0x80;    // fade-out e = 0..127 (128 uploads)
constexpr int kCreditsHoldEnd = 0x12C;         // 0x4f38: conceal staged when the counter reaches 300
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
// (The old 120-frame splash/screen fade constants were fitted to a capture; every
// frontend fade is now the original's tick-exact palette level, see kFadeTicks.)
constexpr int kTopOutTimingFps = 60;
// RE 0x1f8c/0x1fc6: the top-out dissolve sweeps exactly ONE pixel-row per frame down
// the whole gameplay area (kBoardHeight*8 = 160 pixel rows), so the spray completes in
// ~160 frames (~2.3s at 70Hz), not the earlier 11s guess. With each particle living
// 16..79 frames, dozens of rows are mid-explosion at once = a continuous cascade.
constexpr int kTopOutDissolveFrames = kBoardHeight * kPieceTileSize;  // 160
constexpr int kTopOutOverlayStartFrame = kTopOutDissolveFrames;       // GAME OVER right after
constexpr int kTopOutOverlayEndFrame = kTopOutOverlayStartFrame + 10 * kTopOutTimingFps;  // ~hold
constexpr int kTopOutHandoffFrame = kTopOutOverlayEndFrame + 20;
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

// Track names exactly as the original's Music row strings (0x17b2b..0x17bab);
// track 1 is "Tearing up Spacetime" there (FE-18).
const std::array<const char*, 6> kMusicTracks = {
    "Continuum",
    "Tearing up Spacetime",
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

// Credits: six pages of eight fixed row slots (0x4f38 copies 8 x 40 bytes per
// page), blank slots included, and the copyright line keeps its trailing space.
using CreditsPage = std::array<const char*, 8>;
const std::array<CreditsPage, 6> kCreditsPages = {{
    {"", "Acid Tetris", "", "Copyright 1997 ", "", "Dungeon Dwellers Design", "", ""},
    {"", "Programming:", "Jason Pimble", "", "Graphics:", "Scott Emerle", "", ""},
    {"", "Music System:", "Jean Paul Mikkers", "", "Orignal Concept", "Design and Program By:", "Alexey Pazhitnov", ""},
    {"", "", "Protected Mode Extender By:", "", "Charles Scheffold", "Thomas Pytel", "", ""},
    {"", "Music:", "Chris Emerle", "Scott Emerle", "Liam Hesse", "Jon Dal Kristbjornsson", "Bobby Tamburrino", ""},
    {"", "Special Thanks To:", "", "Paul Mason", "Cathleen Crandall", "John Hood", "", ""},
}};

// Keyboard Setup key names: the original prints a 4-character name per scancode
// (spaces included), indexed by the ISR code (E0 keys +0x80). Codes not listed
// print four spaces.
const char* KeyNameForCode(std::uint8_t code) {
  switch (code) {
    case 0x01: return "Esc ";
    case 0x02: return " 1  ";
    case 0x03: return " 2  ";
    case 0x04: return " 3  ";
    case 0x05: return " 4  ";
    case 0x06: return " 5  ";
    case 0x07: return " 6  ";
    case 0x08: return " 7  ";
    case 0x09: return " 8  ";
    case 0x0A: return " 9  ";
    case 0x0B: return " 0  ";
    case 0x0C: return " -  ";
    case 0x0D: return " =  ";
    case 0x0E: return "BkSp";
    case 0x0F: return "Tab ";
    case 0x10: return " Q  ";
    case 0x11: return " W  ";
    case 0x12: return " E  ";
    case 0x13: return " R  ";
    case 0x14: return " T  ";
    case 0x15: return " Y  ";
    case 0x16: return " U  ";
    case 0x17: return " I  ";
    case 0x18: return " O  ";
    case 0x19: return " P  ";
    case 0x1A: return " [  ";
    case 0x1B: return " ]  ";
    case 0x1C: return "Entr";
    case 0x1D: return "CtLt";
    case 0x1E: return " A  ";
    case 0x1F: return " S  ";
    case 0x20: return " D  ";
    case 0x21: return " F  ";
    case 0x22: return " G  ";
    case 0x23: return " H  ";
    case 0x24: return " J  ";
    case 0x25: return " K  ";
    case 0x26: return " L  ";
    case 0x27: return " ;  ";
    case 0x28: return " '  ";
    case 0x29: return " `  ";
    case 0x2A: return "ShLt";
    case 0x2B: return "   ";
    case 0x2C: return " Z  ";
    case 0x2D: return " X  ";
    case 0x2E: return " C  ";
    case 0x2F: return " V  ";
    case 0x30: return " B  ";
    case 0x31: return " N  ";
    case 0x32: return " M  ";
    case 0x33: return " ,  ";
    case 0x34: return " .  ";
    case 0x35: return " /  ";
    case 0x36: return "ShRt";
    case 0x37: return "* KP";
    case 0x38: return "AtLt";
    case 0x39: return "Spce";
    case 0x3A: return "CapL";
    case 0x3B: return " F1 ";
    case 0x3C: return " F2 ";
    case 0x3D: return " F3 ";
    case 0x3E: return " F4 ";
    case 0x3F: return " F5 ";
    case 0x40: return " F6 ";
    case 0x41: return " F7 ";
    case 0x42: return " F8 ";
    case 0x43: return " F9 ";
    case 0x44: return "F10 ";
    case 0x45: return "NmLk";
    case 0x46: return "ScLk";
    case 0x47: return "HmKP";
    case 0x48: return "UpKP";
    case 0x49: return "PUKP";
    case 0x4A: return "- KP";
    case 0x4B: return "LtKP";
    case 0x4C: return "CnKP";
    case 0x4D: return "RtKP";
    case 0x4E: return "+ KP";
    case 0x4F: return "EdKP";
    case 0x50: return "DnKP";
    case 0x51: return "PDKP";
    case 0x52: return "InKP";
    case 0x53: return "DlKP";
    case 0x57: return "F11 ";
    case 0x58: return "F12 ";
    case 0xAA: return "PrSr";
    case 0xB5: return "/ KP";
    case 0xB8: return "AtRt";
    case 0xC7: return "Home";
    case 0xC8: return " Up ";
    case 0xC9: return "PgUp";
    case 0xCB: return "Left";
    case 0xCD: return "Rght";
    case 0xCF: return "End ";
    case 0xD0: return "Down";
    case 0xD1: return "PgDn";
    case 0xD2: return "Inst";
    case 0xD3: return "Del ";
    case 0xDC: return "EnKP";
    case 0xDD: return "CtRt";
    default: return "    ";
  }
}

// High-score name entry character tables (0x50b0 at 0x5502-0x55a1): T0 gates
// which released keys type anything (lowercase), T1/T2 uppercase, T3 lowercase.
// T1-T3 also map '.', which T0 never lets through.
char NameEntryChar(int table, std::uint8_t code) {
  static constexpr char kRow1[] = "1234567890";   // 0x02..0x0b
  static constexpr char kRow2[] = "qwertyuiop";   // 0x10..0x19
  static constexpr char kRow3[] = "asdfghjkl";    // 0x1e..0x26
  static constexpr char kRow4[] = "zxcvbnm";      // 0x2c..0x32
  char ch = 0;
  if (code >= 0x02 && code <= 0x0B) {
    ch = kRow1[code - 0x02];
  } else if (code >= 0x10 && code <= 0x19) {
    ch = kRow2[code - 0x10];
  } else if (code >= 0x1E && code <= 0x26) {
    ch = kRow3[code - 0x1E];
  } else if (code >= 0x2C && code <= 0x32) {
    ch = kRow4[code - 0x2C];
  } else if (code == 0x39) {
    ch = ' ';
  } else if (code == 0x34 && table != 0) {
    ch = '.';
  }
  if ((table == 1 || table == 2) && ch >= 'a' && ch <= 'z') {
    ch = static_cast<char>(ch - 'a' + 'A');
  }
  return ch;
}

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

// The original's table, built at startup by 0x27b8: chop(sin(i * 0.0030679616) *
// 32769.0) for i < 0x200 (the two doubles at 0x17ae8 / 0x17af0, truncated toward
// zero by __CHP + FISTP), mirrored into 2048 entries; peak 32768. The pointfield
// 0x39c4 and the row pulses 0x3fd8 / 0x5868 read this amplitude.
std::array<int, 2048> BuildOriginalSineTable() {
  std::array<int, 2048> table{};
  constexpr double kStep = 0.0030679616;
  constexpr double kAmplitude = 32769.0;
  for (int index = 0; index < 0x200; ++index) {
    const int value = static_cast<int>(std::trunc(std::sin(static_cast<double>(index) * kStep) * kAmplitude));
    table[index] = value;
    table[0x3FF - index] = value;
    table[0x400 + index] = -value;
    table[0x7FF - index] = -value;
  }
  return table;
}

// 32-bit helpers matching the original's IMUL / SAR / IDIV.
inline std::int32_t Wrap32(std::int64_t value) {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value)));
}
inline std::int32_t Mul32(std::int32_t a, std::int32_t b) {
  return Wrap32(static_cast<std::int64_t>(a) * static_cast<std::int64_t>(b));
}
inline std::int32_t Add32(std::int32_t a, std::int32_t b) {
  return Wrap32(static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b));
}
inline std::int32_t Sub32(std::int32_t a, std::int32_t b) {
  return Wrap32(static_cast<std::int64_t>(a) - static_cast<std::int64_t>(b));
}
inline std::int32_t Shl32(std::int32_t a, int bits) {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) << bits);
}

// VGA 6-bit DAC value to the port's 8-bit colour; must stay identical to
// ScaleVga6 in atet_data.cpp (the palettes the textures were built with).
inline Uint8 Expand6To8(int component6) {
  return static_cast<Uint8>(std::lround((static_cast<double>(std::clamp(component6, 0, 63)) / 63.0) * 255.0));
}
// 0x284c: every 6-bit palette byte becomes (c * level) >> 6 (level = 64 - d).
inline int ScaleDac6(int component6, int level) {
  return (component6 * std::clamp(level, 0, 64)) >> 6;
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
      return 0x2A;
    case SDLK_RSHIFT:
      return 0x36;  // the ISR keeps right Shift as its own code (name entry case rule)
    case SDLK_CAPSLOCK:
      return 0x3A;  // released Caps Lock toggles the name-entry case flag
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
    // Keypad arrows are distinct codes (no E0 prefix); the menus accept 0x48/0x50
    // and Options 0x4B/0x4D besides the grey arrows.
    case SDLK_KP_8:
      return 0x48;
    case SDLK_KP_2:
      return 0x50;
    case SDLK_KP_4:
      return 0x4B;
    case SDLK_KP_6:
      return 0x4D;
    default:
      return 0;
  }
}

std::string KeyNameForBinding(std::uint8_t scan_code) {
  return KeyNameForCode(scan_code);
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
  const Uint64 audit_t_data = SDL_GetPerformanceCounter();
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
        g_audit_startup.data_source = dir + dat_name;
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
        g_audit_startup.data_source = dir + archive_name;
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
      g_audit_startup.data_source = std::string("compiled-fallback:") + ATET_PORT_ATET_DAT_PATH;
    }
  }
  g_audit_startup.data_ticks = SDL_GetPerformanceCounter() - audit_t_data;
  const Uint64 audit_t_textures = SDL_GetPerformanceCounter();
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

  g_audit_startup.textures_ticks = SDL_GetPerformanceCounter() - audit_t_textures;
  const Uint64 audit_t_tables = SDL_GetPerformanceCounter();
  if (!LoadFrontendFont(error_out)) {
    return false;
  }

  if (!LoadFrontendPointfield(error_out)) {
    return false;
  }

  if (!LoadGameplayParticleAssets(error_out)) {
    return false;
  }
  g_audit_startup.tables_ticks = SDL_GetPerformanceCounter() - audit_t_tables;
  const Uint64 audit_t_setup = SDL_GetPerformanceCounter();

  scene_ = Scene::kFrontend;
  frontend_view_ = FrontendView::kMainMenu;
  frontend_selected_row_ = 0;
  gameplay_preset_index_ = 0;
  keyboard_capture_row_ = -1;
  held_original_scancodes_.fill(false);
  held_original_scancode_frames_.fill(0);
  // Frontend flow state. ticks_ is the PIT tick counter [0x2d2a3]; the timer is
  // installed just before the first splash, so the splash t0 is tick 0.
  ticks_ = 0;
  release_fifo_.fill(0);
  flow_phase_ = FlowPhase::kNone;
  flow_exit_ = FlowExit::kNone;
  menu_pending_ = 0;
  screen_exit_ = false;
  main_menu_row_ = 0;      // 0x5f78 (0x5fcb): [0x1886f] = 0 at boot
  pulse_phase_ = 0;        // [0x1990f] and [0x19913] are BSS
  entry_pulse_phase_ = 0;
  row_text_.fill(std::string());
  row_amount_.fill(0);
  frontend_burst_ = 0;
  state9_pending_ = false;
  music_freeze_left_ = 0;
  esc_hold_ticks_ = 0;
  setup_dirty_ = false;
  title_level_ = 0;
  hs_mode_ = 0;
  hs_slot_ = -1;
  hs_stage_ = 0;
  hs_buffer_.clear();
  hs_len_ = 0;
  hs_caps_ = false;
  hs_commit_ = false;
  hs_done_ = false;
  // main 0x150: rng_table_fill runs srand([0x2d2a3]) before the PIT is installed
  // (0x16d), so the table is always seeded with 0 at boot; 0x5fd8 -> 0x5ef4 then
  // picks the first pointfield bank with rng_next() % 8 (= 5).
  SeedBootRandomTable();
  InitFrontendPointfield();
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
    // main 0x172-0x17c: 0x3830(5) right after the timer starts, before sound
    // init and the splashes: the dispatcher entry runs over a black screen.
    BeginDispatcherEntry(5, FrozenSource::kBlack);
  } else {
    BeginStartupSplashes();
  }
  if (error_out != nullptr) {
    error_out->clear();
  }
  g_audit_startup.setup_ticks = SDL_GetPerformanceCounter() - audit_t_setup;

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
  // The keyboard ISR 0x800 records every make code whatever the game is doing
  // (splashes, palette fades, reveals), so the held table is always updated.
  TrackKeyDown(key);

  // Developer scene shortcuts (not in the original): opt-in with --debug.
  if (debug_keys_enabled_ && scene_ != Scene::kTopOut && frontend_view_ != FrontendView::kHighScoreEntry) {
    if (key == SDLK_TAB) {
      if (scene_ == Scene::kGameplay) {
        scene_ = Scene::kFrontend;
        StartScreen(1);
      } else {
        scene_ = Scene::kGameplay;
        flow_phase_ = FlowPhase::kNone;
      }
      return true;
    }
    if (key == SDLK_T) {
      scene_ = Scene::kFrontend;
      StartScreen(1);
      return true;
    }
    if (key == SDLK_G) {
      if (!live_game_available_) {
        ResetLiveGameplayState();
        live_game_available_ = true;
      }
      live_game_active_ = true;
      scene_ = Scene::kGameplay;
      flow_phase_ = FlowPhase::kNone;
      return true;
    }
    if (key >= SDLK_1 && key <= SDLK_3) {
      gameplay_preset_index_ = static_cast<int>(key - SDLK_1);
      live_game_active_ = false;
      scene_ = Scene::kGameplay;
      flow_phase_ = FlowPhase::kNone;
      return true;
    }
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

  if (scene_ == Scene::kGameplay && flow_phase_ == FlowPhase::kNone) {
    // In play Esc acts on its RELEASE (main 0x4a3: newest release == 0x01),
    // checked once per tick in Tick(); the press itself does nothing.
    if (key == SDLK_ESCAPE) {
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

  // Splashes and every frontend screen read the release history and the held
  // table once per logic tick (Tick); a key-down never acts by itself.
  return true;
}

void MilestoneADemo::HandleKeyUp(SDL_Keycode key) {
  TrackKeyUp(key);
  PushRelease(OriginalScancodeForKey(key));
  if (scene_ == Scene::kTopOut && IsTopOutAdvanceKey(key) && top_out_handoff_pending_) {
    top_out_handoff_pending_ = false;
    BeginHighScoreNameEntry(top_out_score_, top_out_lines_);
  }
}

void MilestoneADemo::PushRelease(std::uint8_t original_scancode) {
  // ISR 0x800: a break code (0xAA, the fake shift, is dropped) shifts the
  // 32-byte history down and stores the code at the newest slot [31].
  if (original_scancode == 0 || original_scancode == 0xAA) {
    return;
  }
  std::copy(release_fifo_.begin() + 1, release_fifo_.end(), release_fifo_.begin());
  release_fifo_[31] = original_scancode;
}

void MilestoneADemo::Tick() {
  // One logic tick of the PIT clock ([0x2d2a3] = step + 1).
  ++ticks_;

  if (state9_pending_) {
    // Game over -> 0x3830(9), requested by the top-out code since the last tick.
    state9_pending_ = false;
    BeginDispatcherEntry(9, state9_source_);
  }

  for (size_t scan_code = 0; scan_code < held_original_scancodes_.size(); ++scan_code) {
    if (!held_original_scancodes_[scan_code]) {
      continue;
    }

    ++held_original_scancode_frames_[scan_code];
  }

  if (scene_ == Scene::kSplash || scene_ == Scene::kFrontend) {
    // A stale pacer base (0x24d0) makes the frame after a busy wait run the
    // capped 6 logic ticks; the music-row freeze and the high-score Esc hold
    // arm frontend_burst_ for the step that follows them.
    const int runs = std::max(1, frontend_burst_);
    frontend_burst_ = 0;
    for (int run = 0; run < runs; ++run) {
      TickFrontendOnce(run == 0);
      // A screen loop that sets its exit flag (a gate returned 1) is left at once:
      // the rest of the catch-up batch is not run.
      if (scene_ == Scene::kGameplay || quit_requested_ || screen_exit_) {
        break;
      }
    }
    if (scene_ != Scene::kGameplay) {
      FlushAudioEvents();
      return;
    }
    // The dispatcher exit finished in this tick: main's session loop runs its
    // first gameplay step in the same tick (0x488 / 0x4db), so fall through.
  }

  if (scene_ != Scene::kGameplay) {
    FlushAudioEvents();
    return;
  }

  // main 0x4a3-0x4d6: a released Esc (the newest entry of the release history)
  // clears the history and enters the frontend (state 1) before any game logic
  // of this tick runs.
  if (PeekRelease() == 0x01) {
    ClearReleases();
    SaveLiveSnapshot();
    BeginDispatcherEntry(1, FrozenSource::kGameplay);
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
  if (scene_ == Scene::kTopOut) {
    RenderTopOut(renderer);
    return;
  }

  if (scene_ == Scene::kGameplay) {
    RenderGameplay(renderer);
    return;
  }

  switch (flow_phase_) {
    case FlowPhase::kSplash:
      RenderSplash(renderer);
      return;
    case FlowPhase::kBootGap:
      return;  // VRAM cleared: black
    case FlowPhase::kEntryPaletteOut:
    case FlowPhase::kExitPaletteIn:
      if (frozen_capture_pending_) {
        CaptureFrozenPicture(renderer);
      }
      RenderFrozenPicture(renderer, CurrentFrozenLevel());
      return;
    default:
      RenderFrontend(renderer);
      return;
  }
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
  // Edits are saved where the original saves (frontend exit, Exit Game); a window
  // close has no DOS counterpart, so pending edits are flushed here instead of lost.
  if (setup_dirty_) {
    setup_dirty_ = false;
    PersistSetupState();
  }
  if (frozen_texture_ != nullptr) {
    SDL_DestroyTexture(frozen_texture_);
    frozen_texture_ = nullptr;
  }
  frozen_texture_level_ = -1;
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
         << " hsboot=0"
         << " rows=" << (scene_ == Scene::kFrontend ? VisibleRowCount() : 0)
         << " conceal=" << (flow_phase_ == FlowPhase::kConceal ? kRevealSteps - flow_step_ : 0)
         << " commit=" << (hs_commit_ ? 1 : 0)
         << " warning=" << warning_band_
         << " alert=" << alert_effect_id_
         << " pulse=" << PulseRevealAmount()
         << " debris=" << debris_particles_.size()
         << " sleepy=" << (sleepy_ ? 1 : 0)
         << " snapshot=" << (gameplay_snapshot_texture_valid_ ? 1 : 0)
         << " savedunder=" << (top_out_saved_under_valid_ ? 1 : 0)
         << " fade="
         << (flow_phase_ == FlowPhase::kEntryPaletteOut ? "pal-out"
             : flow_phase_ == FlowPhase::kEntryTitleIn  ? "title-in"
             : flow_phase_ == FlowPhase::kExitTitleOut  ? "title-out"
             : flow_phase_ == FlowPhase::kExitPaletteIn ? "pal-in"
             : flow_phase_ == FlowPhase::kQuitTitleOut  ? "quit-out"
                                                        : "none")
         << ":" << FlowFadeE()
         << " flow=" << FlowPhaseName()
         << " sel=" << (frontend_view_ == FrontendView::kMainMenu ? main_menu_row_ : frontend_selected_row_)
         << " pend=" << menu_pending_
         << " phase0x=" << std::hex << pulse_phase_ << std::dec
         << " tlevel=" << CurrentTitleLevel()
         << " tick=" << ticks_;
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
  original_sine_table_ = BuildOriginalSineTable();
  scaled_title_palette_ = title_palette_;
  rendered_title_level_ = 64;  // the textures were built at the full palette
  rendered_splash_page_ = -1;
  rendered_splash_level_ = -1;
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

  // The 0x5ef4 state init (bank choice from the RNG) runs in InitFrontendPointfield.
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
  // 0x2998 copies the picture into the displayed page while the palette is black
  // and then only reprograms the DAC: the picture at the current palette level.
  const int level = CurrentSplashLevel();
  if (level <= 0) {
    return;
  }
  EnsureSplashLevel(splash_page_, level);
  if (splash_page_ == 0) {
    DrawTexture(renderer, ddd_splash_.texture, ddd_splash_.width, ddd_splash_.height);
  } else {
    DrawTexture(renderer, warning_splash_.texture, warning_splash_.width, warning_splash_.height);
  }
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
  // Work buffer = title base + text rows; the chunk-7 points are drawn with
  // set-if-empty after the text. All of it uses the title palette at the level
  // the last object fade left (62/64 at rest, 0x63b8's final upload).
  EnsureTitleLevel(CurrentTitleLevel());
  DrawTexture(renderer, title_screen_.texture, title_screen_.width, title_screen_.height);
  RenderFrontendPointfield(renderer);
  RenderFrontendRows(renderer);
}

void MilestoneADemo::RenderFrontendPointfield(SDL_Renderer* renderer) {
  AuditScopedTicks audit_ticks(&g_audit.pointfield_ticks);
  if (frontend_frame_counter_ <= 0) {
    return;  // 0x39c4 has not run yet: nothing projected
  }
  std::vector<Uint8> occupied(kScreenWidth * kScreenHeight);
  for (const ProjectedPoint& point : frontend_projected_) {
    if (!point.valid) {
      continue;
    }
    // pix_set_if_empty (0x175c5): clipped to 320x240, drawn only on a pixel that is
    // still 0 (title background or an earlier point); text drawn afterwards covers
    // points exactly where a glyph pixel is non-zero, as in the work buffer.
    if (point.x < 0 || point.x >= kScreenWidth || point.y < 0 || point.y >= kScreenHeight) {
      continue;
    }
    const size_t screen_index = static_cast<size_t>(point.y) * kScreenWidth + point.x;
    if (occupied[screen_index] || (!title_indices_.empty() && title_indices_[screen_index] != 0)) {
      continue;
    }
    const SDL_Color color = scaled_title_palette_[static_cast<size_t>(std::clamp(point.color, 0, 255))];
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, SDL_ALPHA_OPAQUE);
    const SDL_FRect point_rect = {
        static_cast<float>(point.x),
        static_cast<float>(point.y),
        1.0f,
        1.0f,
    };
    SDL_RenderFillRect(renderer, &point_rect);
    occupied[screen_index] = 1;
    ++g_audit.pointfield_points;
  }
}

void MilestoneADemo::RenderFrontendRows(SDL_Renderer* renderer) const {
  AuditScopedTicks audit_ticks(&g_audit.frontend_rows_ticks);
  const bool high_score_table =
      frontend_view_ == FrontendView::kHighScores || frontend_view_ == FrontendView::kHighScoreEntry;
  for (int row = 0; row < kFrontendRowCount; ++row) {
    const int amount = row_amount_[static_cast<size_t>(row)];
    if (amount <= 0) {
      continue;  // 0x60cc with amount 0 clears the row cell
    }
    const int y = kFrontendRowY + row * kFrontendRowStep;
    if (high_score_table && row < 5) {
      // 0x50b0 / 0x5868: name, score and lines, each centred on its column.
      RenderFrontendText(renderer, row_text_[static_cast<size_t>(row)], kHiscoreNameCenterX, y, amount);
      RenderFrontendText(renderer, hs_score_text_[static_cast<size_t>(row)], kHiscoreScoreCenterX, y, amount);
      RenderFrontendText(renderer, hs_lines_text_[static_cast<size_t>(row)], kHiscoreLinesCenterX, y, amount);
      continue;
    }
    RenderFrontendText(renderer, row_text_[static_cast<size_t>(row)], kFrontendTextCenterX, y, amount);
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
    // 0x60cc 0x617a: the '_' cursor glyph (0x42) is skipped while bit 4 of the
    // tick counter is set (16 ticks shown, 16 hidden); x still advances.
    if (glyph_code == 0x42 && (ticks_ & 0x10U) != 0) {
      cursor_x += frontend_font_widths_[glyph_index] + 2;
      continue;
    }
    const int atlas_x = (glyph_index % kFrontendGlyphAtlasColumns) * kFrontendGlyphWidth;
    const int atlas_y = (glyph_index / kFrontendGlyphAtlasColumns) * kFrontendGlyphHeight;
    // 0x60cc renders into a fixed 16-row (kFrontendRenderHeight) window at the row's
    // y, bottom-anchored (0x178b0 sampling). The pulse never exceeds 0x40 (16 px).
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
      ++g_audit.glyph_row_draws;
    }

    cursor_x += frontend_font_widths_[glyph_index] + 2;
  }
}

// ---------------------------------------------------------------------------
// Palette levels (0x284c scaling) for the title, the splashes and the frozen
// gameplay picture.

std::array<SDL_Color, 256> MilestoneADemo::ScaledPalette(int chunk_id, int level) const {
  std::array<SDL_Color, 256> palette{};
  const std::vector<Uint8>& chunk = atet_data_.chunk(chunk_id);
  for (size_t index = 0; index < palette.size(); ++index) {
    const size_t offset = index * 3;
    if (offset + 2 >= chunk.size()) {
      break;
    }
    palette[index] = SDL_Color{
        Expand6To8(ScaleDac6(chunk[offset + 0], level)),
        Expand6To8(ScaleDac6(chunk[offset + 1], level)),
        Expand6To8(ScaleDac6(chunk[offset + 2], level)),
        SDL_ALPHA_OPAQUE};
  }
  return palette;
}

void MilestoneADemo::EnsureTitleLevel(int level) {
  level = std::clamp(level, 0, 64);
  if (level == rendered_title_level_) {
    return;
  }
  rendered_title_level_ = level;
  scaled_title_palette_ = ScaledPalette(4, level);  // chunk 4 = title palette
  if (title_screen_.texture != nullptr) {
    const IndexedImage title = BuildIndexedImage(title_indices_, kScreenWidth, kScreenHeight, scaled_title_palette_, -1);
    SDL_UpdateTexture(title_screen_.texture, nullptr, title.rgba_pixels.data(), title.width * 4);
  }
  if (frontend_font_atlas_.texture != nullptr) {
    const AtetData::Indexed& font = atet_data_.font_atlas();
    const IndexedImage atlas = BuildIndexedImage(font.indices, font.width, font.height, scaled_title_palette_, 0);
    SDL_UpdateTexture(frontend_font_atlas_.texture, nullptr, atlas.rgba_pixels.data(), atlas.width * 4);
  }
}

void MilestoneADemo::EnsureSplashLevel(int page, int level) {
  level = std::clamp(level, 0, 64);
  if (page == rendered_splash_page_ && level == rendered_splash_level_) {
    return;
  }
  rendered_splash_page_ = page;
  rendered_splash_level_ = level;
  const bool ddd = page == 0;
  TextureAsset& asset = ddd ? ddd_splash_ : warning_splash_;
  if (asset.texture == nullptr) {
    return;
  }
  const AtetData::Indexed& image = ddd ? atet_data_.ddd_splash() : atet_data_.warning_splash();
  const IndexedImage scaled =
      BuildIndexedImage(image.indices, image.width, image.height, ScaledPalette(ddd ? 0 : 2, level), -1);
  SDL_UpdateTexture(asset.texture, nullptr, scaled.rgba_pixels.data(), scaled.width * 4);
}

int MilestoneADemo::CurrentTitleLevel() const {
  return std::clamp(title_level_, 0, 64);
}

int MilestoneADemo::CurrentSplashLevel() const {
  return fades_enabled_ ? std::clamp(splash_level_, 0, 64) : 64;
}

int MilestoneADemo::FlowFadeE() const {
  switch (flow_phase_) {
    case FlowPhase::kEntryPaletteOut:
    case FlowPhase::kEntryTitleIn:
    case FlowPhase::kExitTitleOut:
    case FlowPhase::kExitPaletteIn:
    case FlowPhase::kQuitTitleOut:
      return static_cast<int>(ticks_ - flow_t0_);
    default:
      return 0;
  }
}

int MilestoneADemo::CurrentFrozenLevel() const {
  const int e = std::clamp(static_cast<int>(ticks_ - flow_t0_), 0, kFadeTicks);
  if (flow_phase_ == FlowPhase::kEntryPaletteOut) {
    return 64 - e;  // 0x64f0: (c * (64 - e)) >> 6, e = 0 shows the picture untouched
  }
  if (flow_phase_ == FlowPhase::kExitPaletteIn) {
    return e;  // 0x6498: (c * e) >> 6; e = 0 is the tick 0x62e0 ended on (black)
  }
  return 64;
}

void MilestoneADemo::CaptureFrozenPicture(SDL_Renderer* renderer) {
  frozen_capture_pending_ = false;
  frozen_texture_level_ = -1;
  frozen_rgba_.assign(static_cast<size_t>(kScreenWidth) * kScreenHeight * 4, 0);
  for (size_t i = 3; i < frozen_rgba_.size(); i += 4) {
    frozen_rgba_[i] = SDL_ALPHA_OPAQUE;
  }
  const bool entry = flow_phase_ == FlowPhase::kEntryPaletteOut;
  if (frozen_source_ == FrozenSource::kBlack || renderer == nullptr) {
    if (entry) {
      // 0x3830 saves the work buffer at its entry (0x3835-0x3847). Before the first
      // game it holds the game screen main decoded at start-up, with no counters:
      // the first New Game fades in on the bare game screen (oracle ff-ngfade).
      const AtetData::Indexed& game = atet_data_.gameplay_screen();
      saved_screen_rgba_ =
          BuildIndexedImage(game.indices, game.width, game.height, atet_data_.gameplay_palette(), -1).rgba_pixels;
    }
    return;
  }
  SDL_Texture* target =
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, kScreenWidth, kScreenHeight);
  if (target == nullptr) {
    return;
  }
  SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
  if (SDL_SetRenderTarget(renderer, target)) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);
    if (frozen_source_ == FrozenSource::kTopOut) {
      RenderTopOutFrozen(renderer);
    } else {
      RenderGameplay(renderer);
    }
    if (SDL_Surface* raw = SDL_RenderReadPixels(renderer, nullptr)) {
      if (SDL_Surface* rgba = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32)) {
        for (int y = 0; y < std::min(rgba->h, kScreenHeight); ++y) {
          const auto* src = static_cast<const Uint8*>(rgba->pixels) + static_cast<size_t>(y) * rgba->pitch;
          std::copy(src, src + static_cast<size_t>(std::min(rgba->w, kScreenWidth)) * 4,
                    frozen_rgba_.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * kScreenWidth * 4));
        }
        SDL_DestroySurface(rgba);
      }
      SDL_DestroySurface(raw);
    }
    SDL_SetRenderTarget(renderer, previous_target);
  }
  SDL_DestroyTexture(target);
  if (entry) {
    saved_screen_rgba_ = frozen_rgba_;  // [0x2c69f]: restored by the exit tail
  }
}

void MilestoneADemo::RenderTopOutFrozen(SDL_Renderer* renderer) {
  // The game-over picture exactly as RenderTopOut last drew it, without
  // advancing the top-out timers.
  if (top_out_frame_ < kTopOutOverlayStartFrame || !top_out_saved_under_valid_ ||
      top_out_saved_under_texture_ == nullptr) {
    RenderTopOutBackdrop(renderer);
  } else {
    DrawTexture(renderer, top_out_saved_under_texture_, kScreenWidth, kScreenHeight);
  }
  if (top_out_frame_ >= kTopOutOverlayStartFrame && top_out_frame_ < kTopOutOverlayEndFrame) {
    DrawTextureAt(renderer, game_over_overlay_.texture, 96, 82, game_over_overlay_.width, game_over_overlay_.height);
  }
}

void MilestoneADemo::RenderFrozenPicture(SDL_Renderer* renderer, int level) {
  level = std::clamp(level, 0, 64);
  if (level <= 0 || frozen_rgba_.empty()) {
    return;
  }
  if (frozen_texture_ == nullptr) {
    frozen_texture_ =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, kScreenWidth, kScreenHeight);
    if (frozen_texture_ == nullptr) {
      return;
    }
    SDL_SetTextureScaleMode(frozen_texture_, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(frozen_texture_, SDL_BLENDMODE_NONE);
    frozen_texture_level_ = -1;
  }
  if (level != frozen_texture_level_) {
    // Every colour of these pictures is a palette colour expanded from 6 bits, so
    // the 6-bit value is recovered exactly and scaled as the DAC would be.
    std::array<Uint8, 256> lut{};
    for (int c8 = 0; c8 < 256; ++c8) {
      const int c6 = static_cast<int>(std::lround(static_cast<double>(c8) * 63.0 / 255.0));
      lut[static_cast<size_t>(c8)] = Expand6To8(ScaleDac6(c6, level));
    }
    std::vector<Uint8> scaled(frozen_rgba_.size());
    for (size_t i = 0; i + 3 < frozen_rgba_.size(); i += 4) {
      scaled[i + 0] = lut[frozen_rgba_[i + 0]];
      scaled[i + 1] = lut[frozen_rgba_[i + 1]];
      scaled[i + 2] = lut[frozen_rgba_[i + 2]];
      scaled[i + 3] = SDL_ALPHA_OPAQUE;
    }
    SDL_UpdateTexture(frozen_texture_, nullptr, scaled.data(), kScreenWidth * 4);
    frozen_texture_level_ = level;
  }
  DrawTexture(renderer, frozen_texture_, kScreenWidth, kScreenHeight);
}

// ---------------------------------------------------------------------------
// Pointfield: 0x5ef4 init and 0x39c4 step, 32-bit like the original.

void MilestoneADemo::SeedBootRandomTable() {
  // rng_table_fill 0x32b0 with srand(0): (rand() << 16) | rand() into the table.
  gameplay_random_state_ = 0;
  gameplay_random_index_ = 0;
  for (std::uint32_t& value : gameplay_random_table_) {
    const std::uint32_t high = NextGameplayRandom15();
    const std::uint32_t low = NextGameplayRandom15();
    value = (high << 16) | low;
  }
}

namespace {
int SignedMod8(std::uint32_t value) {
  // IDIV by 8 after CDQ: the remainder keeps the dividend's sign.
  const std::int32_t v = static_cast<std::int32_t>(value);
  return v % 8;
}
}  // namespace

void MilestoneADemo::InitFrontendPointfield() {
  frontend_pointfield_state_ = FrontendPointfieldState{};
  frontend_next_bank_ = 0;
  frontend_current_bank_ = std::clamp(SignedMod8(NextGameplayRandomU32()), 0, kFrontendBankCount - 1);
  for (int i = 0; i < kFrontendRecordCount; ++i) {
    frontend_active_points_[static_cast<size_t>(i)] =
        frontend_banks_[static_cast<size_t>(frontend_current_bank_)].empty()
            ? FrontendPoint{}
            : frontend_banks_[static_cast<size_t>(frontend_current_bank_)][static_cast<size_t>(i)];
  }
  frontend_cycle_counter_ = 0;
  frontend_frame_counter_ = 0;
  for (ProjectedPoint& point : frontend_projected_) {
    point = ProjectedPoint{};
  }
}

void MilestoneADemo::StepFrontendPointfield() {
  const auto& S = original_sine_table_;
  FrontendPointfieldState& st = frontend_pointfield_state_;
  const auto& cur_bank = frontend_banks_[static_cast<size_t>(frontend_current_bank_)];
  if (cur_bank.empty()) {
    return;
  }
  if (frontend_cycle_counter_ == kFrontendSteadyFrames) {
    // Keep the queued bank unless it equals the current one; then draw from the
    // shared game RNG until it differs. 0x3c70 stores the morph deltas.
    while (frontend_next_bank_ == frontend_current_bank_) {
      frontend_next_bank_ = std::clamp(SignedMod8(NextGameplayRandomU32()), 0, kFrontendBankCount - 1);
    }
    const auto& next_bank = frontend_banks_[static_cast<size_t>(frontend_next_bank_)];
    for (int i = 0; i < kFrontendRecordCount; ++i) {
      const FrontendPoint& c = cur_bank[static_cast<size_t>(i)];
      const FrontendPoint& n = next_bank[static_cast<size_t>(i)];
      frontend_morph_delta_[static_cast<size_t>(i)] = FrontendPoint{Sub32(n.x, c.x), Sub32(n.y, c.y), Sub32(n.z, c.z)};
    }
  }
  if (frontend_cycle_counter_ > kFrontendSteadyFrames) {
    // 0x3cdc: active = current + trunc(delta * p / 128).
    const std::int32_t p = frontend_cycle_counter_ - kFrontendSteadyFrames;
    for (int i = 0; i < kFrontendRecordCount; ++i) {
      const FrontendPoint& c = cur_bank[static_cast<size_t>(i)];
      const FrontendPoint& d = frontend_morph_delta_[static_cast<size_t>(i)];
      frontend_active_points_[static_cast<size_t>(i)] = FrontendPoint{
          Add32(c.x, TruncDiv(Mul32(d.x, p), 128)),
          Add32(c.y, TruncDiv(Mul32(d.y, p), 128)),
          Add32(c.z, TruncDiv(Mul32(d.z, p), 128))};
    }
  }
  if (frontend_cycle_counter_ >= kFrontendSteadyFrames + kFrontendMorphFrames) {
    frontend_cycle_counter_ = 0;
    frontend_current_bank_ = frontend_next_bank_;
  }
  ++frontend_cycle_counter_;

  const std::int32_t sin_a = S[static_cast<size_t>(st.angle_xy & 0x7FF)] >> 4;
  const std::int32_t cos_a = S[static_cast<size_t>((st.angle_xy + 0x200) & 0x7FF)] >> 4;
  const std::int32_t sin_b = S[static_cast<size_t>(st.angle_z & 0x7FF)] >> 4;
  const std::int32_t cos_b = S[static_cast<size_t>((st.angle_z + 0x200) & 0x7FF)] >> 4;
  for (int i = 0; i < kFrontendRecordCount; ++i) {
    const FrontendPoint& p = frontend_active_points_[static_cast<size_t>(i)];
    ProjectedPoint& out = frontend_projected_[static_cast<size_t>(i)];
    const std::int32_t rot = Sub32(Mul32(cos_a, p.x), Mul32(sin_a, p.y)) >> 11;
    std::int32_t num_y = Add32(Mul32(p.x, sin_a), Mul32(p.y, cos_a));
    std::int32_t num_x = Sub32(Mul32(cos_b, rot), Mul32(sin_b, p.z));
    std::int32_t den = Add32(Mul32(rot, sin_b), Mul32(p.z, cos_b));
    num_x = Add32(num_x, st.origin_x);
    num_y = Add32(num_y, st.origin_y);
    den = Add32(den, st.origin_z);
    den = Add32(den, 0x14000000) >> 9;
    if (den == 0) {
      out.valid = false;  // the original would fault (#DE) here; never reached in practice
      continue;
    }
    int shade = Sub32(Shl32(den, 9), 0x14000000) >> 23;
    shade = std::clamp(shade, 0, 15);
    out.x = TruncDiv(num_x, den) + 0xA0;
    out.y = TruncDiv(num_y, den) + 0x78;
    out.color = kFrontendPointColorBase - shade;
    out.valid = true;
  }

  st.origin_x = Add32(st.origin_x, Shl32(S[static_cast<size_t>(st.phase_x & 0x7FF)], 3));
  st.origin_y = Add32(st.origin_y, Shl32(S[static_cast<size_t>(st.phase_y & 0x7FF)], 2));
  st.origin_z = Add32(st.origin_z, Shl32(S[static_cast<size_t>(st.phase_z & 0x7FF)], 4));
  const std::int32_t angle_xy_delta = S[static_cast<size_t>(st.angle_xy & 0x7FF)] >> 14;
  const std::int32_t angle_z_delta = S[static_cast<size_t>(st.angle_z & 0x7FF)] >> 14;
  st.phase_x = Add32(st.phase_x, angle_xy_delta + 3);
  st.phase_y = Add32(st.phase_y, angle_xy_delta + 2);
  st.phase_z = Add32(st.phase_z, angle_z_delta + 4);
  st.angle_xy = Add32(st.angle_xy, 5);
  st.angle_z = Add32(st.angle_z, 2);
  ++frontend_frame_counter_;
}

// ---------------------------------------------------------------------------
// Frontend flow.

void MilestoneADemo::TickFrontendOnce(bool frame_start) {
  switch (flow_phase_) {
    case FlowPhase::kSplash:
      TickSplash();
      break;
    case FlowPhase::kBootGap:
      TickBootGap();
      break;
    case FlowPhase::kEntryPaletteOut:
      TickEntryPaletteOut();
      break;
    case FlowPhase::kEntryTitleIn:
      TickEntryTitleIn();
      break;
    case FlowPhase::kReveal:
      TickReveal();
      break;
    case FlowPhase::kScreen:
      if (frame_start && screen_exit_) {
        // The loop left after the frame in which the gate (or Esc in Credits)
        // set the exit flag; the 48-step conceal starts with the next frame.
        screen_exit_ = false;
        flow_phase_ = FlowPhase::kConceal;
        flow_step_ = 0;
        TickConceal();
      } else {
        TickScreen();
      }
      break;
    case FlowPhase::kConceal:
      TickConceal();
      break;
    case FlowPhase::kExitTitleOut:
      TickExitTitleOut();
      break;
    case FlowPhase::kExitPaletteIn:
      TickExitPaletteIn();
      break;
    case FlowPhase::kQuitTitleOut:
      TickQuitTitleOut();
      break;
    case FlowPhase::kMusicFreeze:
      TickMusicFreeze();
      break;
    case FlowPhase::kNone:
      break;
  }
  if (frame_start) {
    ++frontend_view_frame_;
  }
}

void MilestoneADemo::BeginStartupSplashes() {
  scene_ = Scene::kSplash;
  flow_phase_ = FlowPhase::kSplash;
  keyboard_capture_row_ = -1;
  BeginSplashPage(0);
}

void MilestoneADemo::BeginSplashPage(int page) {
  // 0x2998: black palette, decode, copy to the shown page, then t0 = tick.
  splash_page_ = page;
  splash_stage_ = 0;
  splash_frame_ = 0;
  splash_hold_t1_ = 0;
  splash_level_ = 0;
  flow_t0_ = ticks_;
}

void MilestoneADemo::TickSplash() {
  const int e = static_cast<int>(ticks_ - flow_t0_);
  splash_frame_ = e;
  bool cut = false;
  bool done = false;
  switch (splash_stage_) {
    case 0: {
      // Fade-in: after each tick change e = tick - t0, level 64 - (128 - e) / 2,
      // for e = 1..127 (first visible at e = 3, full at e = 127).
      splash_level_ = 64 - ((0x80 - e) / 2);
      cut = PeekRelease() == 0x01;
      if (!cut && e >= kSplashFadeInEnd) {
        splash_stage_ = 1;
        splash_hold_t1_ = e;
      }
      break;
    }
    case 1: {
      // Hold: polls the release history until 120 ticks have passed.
      cut = PeekRelease() == 0x01;
      if (!cut && e - splash_hold_t1_ >= kSplashHoldTicks) {
        splash_stage_ = 2;
        splash_hold_t1_ = e;  // t2: the fade-out uploads at t2+1 .. t2+128
      }
      break;
    }
    default: {
      // Fade-out: the tick is read BEFORE each wait, so the upload at tick
      // t2+1+k uses k = 0..127: level 64 - k / 2 (two full uploads first).
      const int k = e - 1 - splash_hold_t1_;
      splash_level_ = 64 - (std::clamp(k, 0, 0x7F) / 2);
      cut = PeekRelease() == 0x01;
      if (!cut && k >= kSplashFadeOutUploads - 1) {
        done = true;  // last upload (k = 127) then VRAM is cleared
      }
      break;
    }
  }
  if (cut) {
    // Only an Esc release cuts a page: 0x94c, black palette, VRAM cleared; other
    // releases stay in the history (an Enter starts a New Game at the menu).
    ClearReleases();
    splash_level_ = 0;
    done = true;
  }
  if (!done) {
    return;
  }
  splash_level_ = 0;
  if (splash_page_ == 0) {
    BeginSplashPage(1);  // the WARNING page starts in the same tick (zero-time load)
    return;
  }
  // After the WARNING: chunk loads and page clears, then 0x6544 with no track
  // playing yet still runs its 32-tick fade-out wait before starting the music.
  scene_ = Scene::kFrontend;
  frontend_view_ = FrontendView::kMainMenu;
  flow_phase_ = FlowPhase::kBootGap;
  flow_t0_ = ticks_;
}

void MilestoneADemo::TickBootGap() {
  const int e = static_cast<int>(ticks_ - flow_t0_);
  if (e < kBootLoadAllowanceTicks + kMusicChangeWaitTicks) {
    return;
  }
  // 0x6544: module loaded and started with its 0x20-tick fade-in, then 0x3830(1).
  StartCurrentMusic("music-start-after-startup");
  BeginDispatcherEntry(1, FrozenSource::kBlack);
}

void MilestoneADemo::BeginDispatcherEntry(int state, FrozenSource source) {
  // 0x3830 entry: save the work buffer, 0x64f0 over the displayed picture, copy the
  // title to the work buffer and all three pages, 0x63b8, then dispatch.
  scene_ = Scene::kFrontend;
  dispatch_state_ = state;
  frontend_view_ = state == 5 ? FrontendView::kSoundSetup
                   : state == 9 ? FrontendView::kHighScoreEntry
                                : FrontendView::kMainMenu;
  frozen_source_ = source;
  frozen_capture_pending_ = true;
  frozen_texture_level_ = -1;
  row_amount_.fill(0);
  menu_pending_ = 0;
  screen_exit_ = false;
  flow_t0_ = ticks_;
  if (!fades_enabled_) {
    title_level_ = kFrontendRestLevel;
    StartScreen(state);
    TickReveal();
    return;
  }
  flow_phase_ = FlowPhase::kEntryPaletteOut;
}

void MilestoneADemo::TickEntryPaletteOut() {
  const int e = static_cast<int>(ticks_ - flow_t0_);
  if (e < kFadeTicks) {
    return;  // level 64 - e, read by Render; no game logic, no pointfield
  }
  // Last upload (e = 63) done: the title copy and 0x63b8 start in this tick.
  flow_phase_ = FlowPhase::kEntryTitleIn;
  flow_t0_ = ticks_;
  title_level_ = 0;
  TickEntryTitleIn();
}

void MilestoneADemo::TickEntryTitleIn() {
  const int e = static_cast<int>(ticks_ - flow_t0_);
  if (e >= kFadeTicks) {
    // 0x63b8 leaves after the frame with e = 62: the title palette stays at 62/64.
    StartScreen(dispatch_state_);
    TickReveal();
    return;
  }
  title_level_ = e;
  // 0x63b8 does not re-base the pacer, whose base predates the 63-tick busy wait,
  // so the fade's second frame runs the capped 6 catch-up ticks (6 pointfield steps).
  const int steps = e == 1 ? kStalePacerBurst : 1;
  for (int i = 0; i < steps; ++i) {
    StepFrontendPointfield();
  }
}

void MilestoneADemo::SetAllRowAmounts(int amount) {
  for (int row = 0; row < kFrontendRowCount; ++row) {
    row_amount_[static_cast<size_t>(row)] = amount;
  }
}

void MilestoneADemo::SetMainMenuRowText(int row) {
  switch (row) {
    case 0:
      row_text_[0] = "New Game";
      break;
    case 1:
      row_text_[1] = "Options";
      break;
    case 2:
      row_text_[2] = std::string("Music:`") + kMusicTracks[static_cast<size_t>(
                                                   NormalizeModulo(music_track_index_, static_cast<int>(kMusicTracks.size())))] +
                     "'";
      break;
    case 3:
      row_text_[3] = "Level " + std::to_string(start_level_);
      break;
    case 4:
      row_text_[4] = "High Scores";
      break;
    case 5:
      row_text_[5] = "Credits";
      break;
    case 6:
      row_text_[6] = "Exit Game";
      break;
    case 7:
      row_text_[7] = "Return to Game";
      break;
    default:
      break;
  }
}

void MilestoneADemo::BuildOptionsRows() {
  row_text_[0] = "Music Volume:" + std::to_string(music_volume_);
  row_text_[1] = "Sound FX Volume:" + std::to_string(sfx_volume_);
  row_text_[2] = "Keyboard Setup";
  row_text_[3] = "Back To Main Menu";
  for (int row = 4; row < kFrontendRowCount; ++row) {
    row_text_[static_cast<size_t>(row)].clear();
  }
}

std::string MilestoneADemo::KeyboardBindingRowText(int row_index) const {
  if (row_index < 0 || row_index >= static_cast<int>(keyboard_bindings_.size())) {
    return {};
  }
  return std::string(kKeyboardBindingLabels[static_cast<size_t>(row_index)]) + ":" +
         KeyNameForBinding(keyboard_bindings_[static_cast<size_t>(row_index)]);
}

void MilestoneADemo::BuildKeyboardSetupRows() {
  for (int row = 0; row < 5; ++row) {
    row_text_[static_cast<size_t>(row)] = KeyboardBindingRowText(row);
  }
  row_text_[5] = "Back to Options Menu";
  row_text_[6].clear();
  row_text_[7].clear();
}

void MilestoneADemo::BuildSoundSetupRows() {
  row_text_[0] = kSoundDeviceNames[static_cast<size_t>(
      std::clamp(sound_device_index_, 0, static_cast<int>(kSoundDeviceNames.size()) - 1))];
  row_text_[1] = "Mixing Rate:" + std::to_string(mixing_rate_hz_);
  row_text_[2] = stereo_flag_ != 0 ? "Stereo" : "Mono";
  row_text_[3] = bit_depth_flag_ != 0 ? "16 Bit" : "8 Bit";
  row_text_[4] = "Play Game";
  row_text_[5] = "Exit to Dos";
  row_text_[6].clear();
  row_text_[7].clear();
}

void MilestoneADemo::BuildCreditsRows() {
  const CreditsPage& page =
      kCreditsPages[static_cast<size_t>(NormalizeModulo(credits_page_, static_cast<int>(kCreditsPages.size())))];
  for (int row = 0; row < kFrontendRowCount; ++row) {
    row_text_[static_cast<size_t>(row)] = page[static_cast<size_t>(row)];
  }
}

void MilestoneADemo::BuildHighScoreRows() {
  for (size_t row = 0; row < high_scores_.size(); ++row) {
    row_text_[row] = high_scores_[row].name;
    hs_score_text_[row] = std::to_string(high_scores_[row].score);
    hs_lines_text_[row] = std::to_string(high_scores_[row].lines);
  }
  row_text_[5].clear();
  row_text_[6] = "Return to Main Menu";
  row_text_[7].clear();
}

void MilestoneADemo::StartScreen(int state) {
  scene_ = Scene::kFrontend;
  flow_phase_ = FlowPhase::kReveal;
  flow_step_ = 0;
  menu_pending_ = 0;
  screen_exit_ = false;
  menu_return_state_ = 1;
  keyboard_capture_row_ = -1;
  frontend_view_frame_ = 0;
  dispatch_state_ = state;
  row_text_.fill(std::string());
  row_amount_.fill(0);
  switch (state) {
    case 5: {
      // 0x5a94 snaps the stored mixing rate to the first table entry >= it; a rate
      // above 44100 indexes past the 7-entry copy and reads the index itself (7).
      frontend_view_ = FrontendView::kSoundSetup;
      int index = 0;
      while (index < static_cast<int>(kMixingRates.size()) && mixing_rate_hz_ > kMixingRates[static_cast<size_t>(index)]) {
        ++index;
      }
      sound_rate_index_ = index;
      mixing_rate_hz_ = index < static_cast<int>(kMixingRates.size()) ? kMixingRates[static_cast<size_t>(index)] : index;
      frontend_selected_row_ = 0;
      BuildSoundSetupRows();
      break;
    }
    case 6:
      frontend_view_ = FrontendView::kOptions;
      frontend_selected_row_ = 0;  // 0x4584 always starts on row 0
      BuildOptionsRows();
      break;
    case 7:
      frontend_view_ = FrontendView::kKeyboardSetup;
      frontend_selected_row_ = 0;
      BuildKeyboardSetupRows();
      break;
    case 8:
    case 9: {
      // 0x50b0(mode): mode 1 (a game just ended) qualifies the run first.
      hs_mode_ = state == 9 ? 1 : 0;
      hs_slot_ = -1;
      if (hs_mode_ == 1) {
        int slot = 0;
        while (slot < static_cast<int>(high_scores_.size()) &&
               top_out_score_ <= high_scores_[static_cast<size_t>(slot)].score) {
          ++slot;
        }
        if (slot >= static_cast<int>(high_scores_.size())) {
          hs_mode_ = 0;
        } else {
          for (int index = static_cast<int>(high_scores_.size()) - 1; index > slot; --index) {
            high_scores_[static_cast<size_t>(index)] = high_scores_[static_cast<size_t>(index - 1)];
          }
          high_scores_[static_cast<size_t>(slot)] = HighScoreRecord{"", top_out_score_, top_out_lines_};
          hs_slot_ = slot;
          MarkSetupDirty();
        }
      }
      frontend_view_ = hs_mode_ == 1 ? FrontendView::kHighScoreEntry : FrontendView::kHighScores;
      frontend_selected_row_ = kHiscoreFooterRow;
      hs_stage_ = hs_mode_ == 1 ? 0 : 1;
      BuildHighScoreRows();
      break;
    }
    case 10:
      frontend_view_ = FrontendView::kCredits;
      credits_page_ = 0;
      credits_counter_ = 0;
      credits_esc_latch_ = false;
      credits_mode_ = 1;
      BuildCreditsRows();
      break;
    case 1:
    default:
      frontend_view_ = FrontendView::kMainMenu;
      for (int row = 0; row < kFrontendRowCount; ++row) {
        SetMainMenuRowText(row);
      }
      frontend_selected_row_ = main_menu_row_;
      break;
  }
}

void MilestoneADemo::TickReveal() {
  // 0x3df4 (and 0x50b0's reveal): every row at min(step, 0x30) each tick, no
  // input read; the loop is left after the frame whose step reached 0x30, so the
  // screen's own loop starts with the next tick.
  if (flow_step_ >= kRevealSteps) {
    flow_phase_ = FlowPhase::kScreen;
    if (frontend_view_ == FrontendView::kCredits) {
      credits_mode_ = 0;
      credits_counter_ = 1;  // 0x4fa1: EBX++ after the reveal
    }
    if (frontend_view_ == FrontendView::kHighScores || frontend_view_ == FrontendView::kHighScoreEntry) {
      hs_buffer_.clear();
      hs_len_ = 0;
      hs_caps_ = false;
      hs_commit_ = false;
      hs_done_ = false;
      menu_pending_ = 0;
    }
    TickScreen();
    return;
  }
  SetAllRowAmounts(std::min(flow_step_, kRevealSteps));
  StepFrontendPointfield();
  ++flow_step_;
}

void MilestoneADemo::TickConceal() {
  if (flow_step_ >= kRevealSteps) {
    // 0x3ee4 returned: the screen hands its state to the dispatcher (or Credits
    // starts its next page).
    if (frontend_view_ == FrontendView::kCredits && credits_mode_ == 2) {
      credits_page_ = (credits_page_ + 1) % static_cast<int>(kCreditsPages.size());
      credits_counter_ = 0;
      credits_mode_ = 0;
      flow_phase_ = FlowPhase::kScreen;
      TickScreen();
      return;
    }
    DispatchState(menu_return_state_);
    return;
  }
  SetAllRowAmounts(std::max(kRevealSteps - 1 - flow_step_, 0));
  StepFrontendPointfield();
  ++flow_step_;
}

void MilestoneADemo::DispatchState(int state) {
  switch (state) {
    case 2:
      // Return to Game (or Play Game / Esc in the start-up Sound Setup): [0x1886f]
      // = 7 (0x38d0), then the exit tail; main resets the row to 0 after the setup
      // detour returns (0x181).
      main_menu_row_ = 7;
      BeginDispatcherExit(dispatch_state_ == 5 ? FlowExit::kSplashes : FlowExit::kResume);
      return;
    case 3:
      BeginQuitFade();
      return;
    case 4:
      BeginDispatcherExit(FlowExit::kNewGame);
      return;
    default:
      // The dispatcher loop calls the next screen directly: no palette fades.
      StartScreen(state);
      TickReveal();
      return;
  }
}

void MilestoneADemo::BeginDispatcherExit(FlowExit exit) {
  ClearReleases();  // 0x3941
  flow_exit_ = exit;
  if (!fades_enabled_) {
    PersistSetupState();
    setup_dirty_ = false;
    FinishDispatcherExit();
    return;
  }
  flow_phase_ = FlowPhase::kExitTitleOut;
  flow_t0_ = ticks_;
  row_amount_.fill(0);
  TickExitTitleOut();
}

void MilestoneADemo::TickExitTitleOut() {
  const int e = static_cast<int>(ticks_ - flow_t0_);
  if (e >= kFadeTicks) {
    // 0x3604 saves SETUP.DAT, the saved gameplay picture is restored (New Game
    // also runs its init, 0x2d88 / 0x5e0), then 0x6498 starts in this tick.
    PersistSetupState();
    setup_dirty_ = false;
    frozen_capture_pending_ = true;
    switch (flow_exit_) {
      case FlowExit::kNewGame:
        ResetLiveGameplayState();
        live_game_active_ = true;
        live_game_available_ = true;
        StartCurrentMusic("music-continue-new-game");
        frozen_source_ = FrozenSource::kGameplay;
        UseSavedScreenForNewGame();
        break;
      case FlowExit::kResume:
        // The original restores the saved screen; nothing in the frontend changes the
        // parked game, so rendering the restored game gives the same picture.
        RestoreLiveSnapshot();
        live_game_active_ = true;
        StartCurrentMusic("music-continue-return-to-game");
        frozen_source_ = FrozenSource::kGameplay;
        break;
      case FlowExit::kSplashes:
      case FlowExit::kNone:
        frozen_source_ = FrozenSource::kBlack;
        break;
    }
    frozen_texture_level_ = -1;
    flow_phase_ = FlowPhase::kExitPaletteIn;
    flow_t0_ = ticks_;
    return;
  }
  // 0x62e0: title palette 64 - e (the first frame is at full brightness), the
  // pointfield keeps moving; the rows were concealed before.
  title_level_ = 64 - e;
  StepFrontendPointfield();
}

void MilestoneADemo::UseSavedScreenForNewGame() {
  // The exit tail copies the screen saved at the entry ([0x2c69f], 0x395a-0x3995) back
  // to the work buffer and the pages; 0x6498 then only ramps the palette. For New
  // Game, 0x05e0 first zeroes the well and the next box (0x6274) and copies the work
  // buffer to the pages again; the new counters, the first piece, the preview and the
  // face reset are drawn later and appear with the first gameplay frame (measured on
  // the oracle: ff-ngfade, ff-ngfade2; AL-VO-01). The old counters, statistics and
  // face therefore stay visible through the fade-in.
  if (saved_screen_rgba_.size() != static_cast<size_t>(kScreenWidth) * kScreenHeight * 4) {
    return;  // no entry picture (fades were off): render the game instead
  }
  frozen_rgba_ = saved_screen_rgba_;
  const SDL_Color black = atet_data_.gameplay_palette()[0];
  const auto zero_rect = [&](int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h && y < kScreenHeight; ++y) {
      for (int x = x0; x < x0 + w && x < kScreenWidth; ++x) {
        const size_t i = (static_cast<size_t>(y) * kScreenWidth + static_cast<size_t>(x)) * 4;
        frozen_rgba_[i + 0] = black.r;
        frozen_rgba_[i + 1] = black.g;
        frozen_rgba_[i + 2] = black.b;
        frozen_rgba_[i + 3] = SDL_ALPHA_OPAQUE;
      }
    }
  };
  zero_rect(kGameplayClearX, kGameplayClearY, kGameplayClearWidth, kGameplayClearHeight);
  zero_rect(kPreviewX, kPreviewY, kPreviewWidth, kPreviewHeight);
  frozen_capture_pending_ = false;
}

void MilestoneADemo::TickExitPaletteIn() {
  const int e = static_cast<int>(ticks_ - flow_t0_);
  if (e < kFadeTicks) {
    return;  // level e, read by Render; nothing else runs during the busy wait
  }
  // e = 63 uploaded, followed by the full palette in the same tick (0x39b8).
  FinishDispatcherExit();
}

void MilestoneADemo::FinishDispatcherExit() {
  flow_phase_ = FlowPhase::kNone;
  const FlowExit exit = flow_exit_;
  flow_exit_ = FlowExit::kNone;
  switch (exit) {
    case FlowExit::kNewGame:
      if (!fades_enabled_) {
        ResetLiveGameplayState();
        live_game_active_ = true;
        live_game_available_ = true;
        StartCurrentMusic("music-continue-new-game");
      }
      scene_ = Scene::kGameplay;
      break;
    case FlowExit::kResume:
      if (!fades_enabled_) {
        RestoreLiveSnapshot();
        live_game_active_ = true;
        StartCurrentMusic("music-continue-return-to-game");
      }
      scene_ = Scene::kGameplay;
      break;
    case FlowExit::kSplashes:
    case FlowExit::kNone:
      // Back in main after the start-up Sound Setup: sound init, then the splashes.
      main_menu_row_ = 0;
      BeginStartupSplashes();
      break;
  }
}

void MilestoneADemo::BeginQuitFade() {
  // State 3: wait for a running music ramp (none can be pending here: the
  // Music-row change already waited), start the 0x40-tick music fade-out, run the
  // title fade-out, save SETUP.DAT and exit.
  ScheduleExitFade();
  if (!fades_enabled_) {
    PersistSetupState();
    setup_dirty_ = false;
    quit_requested_ = true;
    return;
  }
  flow_phase_ = FlowPhase::kQuitTitleOut;
  flow_t0_ = ticks_;
  row_amount_.fill(0);
  TickQuitTitleOut();
}

void MilestoneADemo::TickQuitTitleOut() {
  const int e = static_cast<int>(ticks_ - flow_t0_);
  title_level_ = 64 - std::min(e, kFadeTicks - 1);
  if (!quit_requested_) {
    StepFrontendPointfield();
  }
  if (e >= kFadeTicks - 1 && !quit_requested_) {
    // The frame with e = 62 (level 2) is the last one presented; 0x62e0 then
    // returns, 0x3604 saves and 0x5cc restores text mode and exits.
    PersistSetupState();
    setup_dirty_ = false;
    quit_requested_ = true;
  }
}

void MilestoneADemo::TickMusicFreeze() {
  if (--music_freeze_left_ > 0) {
    return;  // inside 0x6544's busy wait: no logic, no presents
  }
  // The new module is loaded and fading in; the rest of the Enter tick runs now,
  // and the pacer then returns min(elapsed, 6) for the next frame.
  flow_phase_ = FlowPhase::kScreen;
  TickMainMenu(true);
  frontend_burst_ = kStalePacerBurst;
}

int MilestoneADemo::PulseRow(int row, int mode) {
  // 0x3fd8: in gate mode (1) a phase already at 0 or 0x400 ends the gate without
  // drawing; otherwise draw at trunc(sine[phase] / 2048) + 0x30 and advance 0x20,
  // twice in gate mode unless the first step landed on 0 or 0x400.
  if (mode == 1 && (pulse_phase_ == 0 || pulse_phase_ == 0x400)) {
    return 1;
  }
  if (row >= 0 && row < kFrontendRowCount) {
    row_amount_[static_cast<size_t>(row)] =
        TruncDiv(original_sine_table_[static_cast<size_t>(pulse_phase_ & kPulsePhaseMask)], 0x800) + 0x30;
  }
  pulse_phase_ = (pulse_phase_ + kPulseStep) & kPulsePhaseMask;
  if (mode == 1 && pulse_phase_ != 0 && pulse_phase_ != 0x400) {
    pulse_phase_ = (pulse_phase_ + kPulseStep) & kPulsePhaseMask;
  }
  return 0;
}

int MilestoneADemo::PulseEntryRow(int slot, int mode) {
  // 0x5868: like 0x3fd8 on its own phase [0x19913] for the three columns of the
  // inserted row, except that a gate ending on 0 / 0x400 still draws (settled 0x30).
  if (slot < 0 || slot >= 5) {
    return 1;
  }
  const int amount =
      TruncDiv(original_sine_table_[static_cast<size_t>(entry_pulse_phase_ & kPulsePhaseMask)], 0x800) + 0x30;
  row_amount_[static_cast<size_t>(slot)] = amount;
  if (mode == 1 && (entry_pulse_phase_ == 0 || entry_pulse_phase_ == 0x400)) {
    return 1;
  }
  entry_pulse_phase_ = (entry_pulse_phase_ + kPulseStep) & kPulsePhaseMask;
  if (mode == 1 && entry_pulse_phase_ != 0 && entry_pulse_phase_ != 0x400) {
    entry_pulse_phase_ = (entry_pulse_phase_ + kPulseStep) & kPulsePhaseMask;
  }
  return 0;
}

void MilestoneADemo::TickScreen() {
  switch (frontend_view_) {
    case FrontendView::kMainMenu:
      TickMainMenu(false);
      break;
    case FrontendView::kOptions:
      TickOptions();
      break;
    case FrontendView::kKeyboardSetup:
      TickKeyboardSetup();
      break;
    case FrontendView::kSoundSetup:
      TickSoundSetup();
      break;
    case FrontendView::kCredits:
      TickCredits();
      break;
    case FrontendView::kHighScores:
    case FrontendView::kHighScoreEntry:
      TickHighScores();
      break;
  }
}

void MilestoneADemo::TickMainMenu(bool resume_after_music) {
  // 0x40c0 steady loop, one logic tick.
  int& row = main_menu_row_;
  if (!resume_after_music) {
    // Esc counts only while HELD, only with a game in progress, and only when no
    // action is pending: Return to Game through the gate (0x4232-0x425b).
    if (KeyHeld(0x01) && menu_pending_ == 0 && live_game_available_) {
      menu_pending_ = 1;
      menu_return_state_ = 2;
      ClearReleases();
    }
    if (PeekRelease() == 0x1C && menu_pending_ == 0) {
      ClearReleases();
      switch (row) {
        case 0:
          menu_return_state_ = 4;
          menu_pending_ = 1;
          break;
        case 1:
          menu_return_state_ = 6;
          menu_pending_ = 1;
          break;
        case 2:
          // 0x61c8 clears the row, the track advances (wrap after 5) and 0x6544
          // blocks inside this tick: 32-tick fade-out, load, start, fade-in.
          music_track_index_ = music_track_index_ + 1 > 5 ? 0 : music_track_index_ + 1;
          MarkSetupDirty();
          StartCurrentMusic("music-track-change");
          flow_phase_ = FlowPhase::kMusicFreeze;
          music_freeze_left_ = kMusicChangeWaitTicks;
          return;
        case 3:
          start_level_ = start_level_ + 1 > 9 ? 0 : start_level_ + 1;
          SetMainMenuRowText(3);
          break;
        case 4:
          menu_return_state_ = 8;
          menu_pending_ = 1;
          break;
        case 5:
          menu_return_state_ = 10;
          menu_pending_ = 1;
          break;
        case 6:
          menu_return_state_ = 3;
          menu_pending_ = 1;
          break;
        case 7:
          if (live_game_available_) {
            menu_return_state_ = 2;
            menu_pending_ = 1;
          }
          break;
        default:
          break;
      }
    }
  } else {
    SetMainMenuRowText(2);  // re-printed after 0x6544 returned
  }
  const std::uint8_t up = PeekRelease();
  if ((up == 0xC8 || up == 0x48) && menu_pending_ == 0) {
    EmitAudioEvent("menu-row-move", 1);
    menu_pending_ = 2;
    ClearReleases();
  }
  const std::uint8_t down = PeekRelease();
  if ((down == 0xD0 || down == 0x50) && menu_pending_ == 0) {
    EmitAudioEvent("menu-row-move", 1);
    menu_pending_ = 3;
    ClearReleases();
  }
  StepFrontendPointfield();
  if (menu_pending_ == 0) {
    PulseRow(row, 0);
  }
  if (menu_pending_ == 1 && PulseRow(row, 1) == 1) {
    screen_exit_ = true;
  }
  if (menu_pending_ == 2 && PulseRow(row, 1) == 1) {
    row = row - 1 < 0 ? 7 : row - 1;
    menu_pending_ = 0;
  }
  if (menu_pending_ == 3 && PulseRow(row, 1) == 1) {
    row = row + 1 > 7 ? 0 : row + 1;
    menu_pending_ = 0;
  }
  frontend_selected_row_ = row;
}

void MilestoneADemo::TickOptions() {
  // 0x4584 steady loop.
  int& row = frontend_selected_row_;
  if (KeyHeld(0x01) && menu_pending_ == 0) {
    ClearReleases();
    menu_return_state_ = 1;
    menu_pending_ = 1;
  }
  if (PeekRelease() == 0x1C && menu_pending_ == 0) {
    ClearReleases();  // rows 0 and 1 ignore Enter
    if (row == 2) {
      menu_return_state_ = 7;
      menu_pending_ = 1;
    } else if (row == 3) {
      menu_return_state_ = 1;
      menu_pending_ = 1;
    }
  }
  // Left/Right are read from the held table: one unit per tick, clamped 0..100,
  // the two directions tested independently; music volume applied each change.
  if ((KeyHeld(0xCB) || KeyHeld(0x4B)) && menu_pending_ == 0) {
    if (row == 0) {
      if (music_volume_ > 0) {
        --music_volume_;
        ApplyMusicVolume();
        MarkSetupDirty();
      }
      row_text_[0] = "Music Volume:" + std::to_string(music_volume_);
    } else if (row == 1) {
      if (sfx_volume_ > 0) {
        --sfx_volume_;
        MarkSetupDirty();
      }
      row_text_[1] = "Sound FX Volume:" + std::to_string(sfx_volume_);
    }
  }
  if ((KeyHeld(0xCD) || KeyHeld(0x4D)) && menu_pending_ == 0) {
    if (row == 0) {
      if (music_volume_ < 100) {
        ++music_volume_;
        ApplyMusicVolume();
        MarkSetupDirty();
      }
      row_text_[0] = "Music Volume:" + std::to_string(music_volume_);
    } else if (row == 1) {
      if (sfx_volume_ < 100) {
        ++sfx_volume_;
        MarkSetupDirty();
      }
      row_text_[1] = "Sound FX Volume:" + std::to_string(sfx_volume_);
    }
  }
  const std::uint8_t up = PeekRelease();
  if ((up == 0xC8 || up == 0x48) && menu_pending_ == 0) {
    EmitAudioEvent("menu-row-move", 1);
    menu_pending_ = 2;
    ClearReleases();
  }
  const std::uint8_t down = PeekRelease();
  if ((down == 0xD0 || down == 0x50) && menu_pending_ == 0) {
    EmitAudioEvent("menu-row-move", 1);
    menu_pending_ = 3;
    ClearReleases();
  }
  StepFrontendPointfield();
  if (menu_pending_ == 0) {
    PulseRow(row, 0);
  }
  if (menu_pending_ == 1 && PulseRow(row, 1) == 1) {
    screen_exit_ = true;
  }
  if (menu_pending_ == 2 && PulseRow(row, 1) == 1) {
    row = row - 1 < 0 ? 3 : row - 1;
    menu_pending_ = 0;
  }
  if (menu_pending_ == 3 && PulseRow(row, 1) == 1) {
    row = row + 1 > 3 ? 0 : row + 1;
    menu_pending_ = 0;
  }
}

void MilestoneADemo::TickKeyboardSetup() {
  // 0x49bc steady loop; pending 4 = waiting for the key to bind.
  int& row = frontend_selected_row_;
  if (KeyHeld(0x01) && menu_pending_ == 0) {
    ClearReleases();
    menu_return_state_ = 1;  // Esc goes back to the main menu
    menu_pending_ = 1;
  }
  if (PeekRelease() == 0x1C && menu_pending_ == 0) {
    ClearReleases();
    static constexpr std::array<const char*, 5> kPrompts = {
        "Down:_", "Left:_", "Right:_", "Rotate Left:_", "Rotate Right:_"};
    if (row >= 0 && row <= 4) {
      row_text_[static_cast<size_t>(row)] = kPrompts[static_cast<size_t>(row)];
      menu_pending_ = 4;
      keyboard_capture_row_ = row;
    } else if (row == 5) {
      menu_return_state_ = 6;  // Back to Options Menu (Options starts on row 0)
      menu_pending_ = 1;
    }
  }
  const std::uint8_t up = PeekRelease();
  if ((up == 0xC8 || up == 0x48) && menu_pending_ == 0) {
    EmitAudioEvent("menu-row-move", 1);
    menu_pending_ = 2;
    ClearReleases();
  }
  const std::uint8_t down = PeekRelease();
  if ((down == 0xD0 || down == 0x50) && menu_pending_ == 0) {
    EmitAudioEvent("menu-row-move", 1);
    menu_pending_ = 3;
    ClearReleases();
  }
  StepFrontendPointfield();
  if (menu_pending_ == 0) {
    PulseRow(row, 0);
  }
  if (menu_pending_ == 1 && PulseRow(row, 1) == 1) {
    screen_exit_ = true;
  }
  if (menu_pending_ == 2 && PulseRow(row, 1) == 1) {
    row = row - 1 < 0 ? 5 : row - 1;
    menu_pending_ = 0;
  }
  if (menu_pending_ == 3 && PulseRow(row, 1) == 1) {
    row = row + 1 > 5 ? 0 : row + 1;
    menu_pending_ = 0;
  }
  if (menu_pending_ == 4) {
    // The prompt row keeps pulsing; the binding is the newest released key of any
    // kind except none and Esc (which is left in the history).
    PulseRow(row, 0);
    const std::uint8_t code = PeekRelease();
    if (code != 0 && code != 0x01) {
      ClearReleases();
      menu_pending_ = 0;
      if (row >= 0 && row < static_cast<int>(keyboard_bindings_.size())) {
        keyboard_bindings_[static_cast<size_t>(row)] = code;
        MarkSetupDirty();
        row_text_[static_cast<size_t>(row)] = KeyboardBindingRowText(row);
        // 0x61c8 clears the row cell again after the capture; the new name is first
        // drawn by the next tick's pulse, so this frame shows the row blank.
        row_amount_[static_cast<size_t>(row)] = 0;
      }
      keyboard_capture_row_ = -1;
    }
  }
}

void MilestoneADemo::TickSoundSetup() {
  // 0x5a94 steady loop (runs before sound init: no SFX here).
  int& row = frontend_selected_row_;
  if (KeyHeld(0x01) && menu_pending_ == 0) {
    menu_return_state_ = 2;
    menu_pending_ = 1;
    ClearReleases();
  }
  if (PeekRelease() == 0x1C && menu_pending_ == 0) {
    ClearReleases();
    switch (row) {
      case 0:
        sound_device_index_ = sound_device_index_ + 1 > 5 ? 0 : sound_device_index_ + 1;
        MarkSetupDirty();
        BuildSoundSetupRows();
        break;
      case 1:
        sound_rate_index_ = sound_rate_index_ + 1 >= static_cast<int>(kMixingRates.size()) ? 0 : sound_rate_index_ + 1;
        mixing_rate_hz_ = kMixingRates[static_cast<size_t>(sound_rate_index_)];
        MarkSetupDirty();
        BuildSoundSetupRows();
        break;
      case 2:
        stereo_flag_ = stereo_flag_ != 0 ? 0 : 1;
        MarkSetupDirty();
        BuildSoundSetupRows();
        break;
      case 3:
        bit_depth_flag_ = bit_depth_flag_ != 0 ? 0 : 1;
        MarkSetupDirty();
        BuildSoundSetupRows();
        break;
      case 4:
        menu_return_state_ = 2;
        menu_pending_ = 1;
        break;
      case 5:
        menu_return_state_ = 3;
        menu_pending_ = 1;
        break;
      default:
        break;
    }
  }
  const std::uint8_t up = PeekRelease();
  if ((up == 0xC8 || up == 0x48) && menu_pending_ == 0) {
    menu_pending_ = 2;
    ClearReleases();
  }
  const std::uint8_t down = PeekRelease();
  if ((down == 0xD0 || down == 0x50) && menu_pending_ == 0) {
    menu_pending_ = 3;
    ClearReleases();
  }
  StepFrontendPointfield();
  if (menu_pending_ == 0) {
    PulseRow(row, 0);
  }
  if (menu_pending_ == 1 && PulseRow(row, 1) == 1) {
    screen_exit_ = true;
  }
  if (menu_pending_ == 2 && PulseRow(row, 1) == 1) {
    row = row - 1 < 0 ? 5 : row - 1;
    menu_pending_ = 0;
  }
  if (menu_pending_ == 3 && PulseRow(row, 1) == 1) {
    row = row + 1 > 5 ? 0 : row + 1;
    menu_pending_ = 0;
  }
}

void MilestoneADemo::TickCredits() {
  // 0x4f38 hold loop. Mode 1 = build the next page and reveal it (one staging
  // tick first), mode 2 = conceal (staged at counter 300), no pulse, no gate.
  if (credits_mode_ == 1) {
    BuildCreditsRows();
    flow_phase_ = FlowPhase::kReveal;
    flow_step_ = 0;
    TickReveal();
    return;
  }
  if (credits_mode_ == 2) {
    flow_phase_ = FlowPhase::kConceal;
    flow_step_ = 0;
    TickConceal();
    return;
  }
  if (credits_counter_ == 0) {
    credits_mode_ = 1;  // staging tick: no pointfield step
    return;
  }
  if (credits_counter_ == kCreditsHoldEnd) {
    credits_mode_ = 2;  // staging tick
    return;
  }
  // The only key: Esc held during a hold tick, latched once; the page then
  // conceals and Credits returns to the main menu.
  if (KeyHeld(0x01) && !credits_esc_latch_) {
    menu_return_state_ = 1;
    credits_esc_latch_ = true;
    screen_exit_ = true;
  }
  StepFrontendPointfield();
  ++credits_counter_;
}

void MilestoneADemo::TickHighScores() {
  // 0x50b0 after its reveal: name entry (state 9 with a qualifying score), then the
  // footer loop.
  if (hs_stage_ == 0 && hs_done_) {
    // The entry loop presented its settled frame last tick and left; the footer loop
    // starts with this tick.
    hs_stage_ = 1;
    frontend_view_ = FrontendView::kHighScores;
    menu_pending_ = 0;
  }
  if (hs_stage_ == 0 && hs_slot_ >= 0) {
    const size_t slot = static_cast<size_t>(hs_slot_);
    if (PeekRelease() == 0x1C && !hs_commit_) {
      ClearReleases();
      hs_commit_ = true;
    }
    // 0x61c8 clears the name cell, the row is printed as "%s_".
    row_text_[slot] = hs_buffer_ + "_";
    StepFrontendPointfield();
    if (!hs_commit_) {
      PulseEntryRow(hs_slot_, 0);
    } else {
      // From the commit on, the name is copied (no cursor) into the row and the
      // record every tick while the one-shot gate settles the row.
      row_text_[slot] = hs_buffer_;
      high_scores_[slot].name = hs_buffer_;
      MarkSetupDirty();
      if (PulseEntryRow(hs_slot_, 1) == 1) {
        hs_done_ = true;
      }
    }
    // Edits come from the release history, one released key per tick; every key
    // but Enter clears the history (a second Enter is left for the footer).
    const std::uint8_t code = PeekRelease();
    if (code != 0) {
      if (code != 0x1C) {
        ClearReleases();
      }
      if (code == 0x0E && hs_len_ > 0) {
        --hs_len_;
        hs_buffer_.resize(static_cast<size_t>(hs_len_));
      }
      if (code == 0x3A) {
        hs_caps_ = !hs_caps_;
      }
      char ch = NameEntryChar(0, code);
      if (ch != 0) {
        const bool left_shift = KeyHeld(0x2A);
        const bool right_shift = KeyHeld(0x36);
        if (!hs_caps_) {
          if (left_shift || right_shift) {
            ch = NameEntryChar(1, code);
          }
        } else {
          ch = (left_shift || !right_shift) ? NameEntryChar(2, code) : NameEntryChar(3, code);
        }
        if (((left_shift || right_shift) ? 1 : 0) != (hs_caps_ ? 1 : 0)) {
          ch = NameEntryChar(1, code);
        }
        // buffer[len] = ch; the 10th character keeps being overwritten.
        hs_buffer_.resize(static_cast<size_t>(hs_len_));
        hs_buffer_.push_back(ch);
        if (hs_len_ < 9) {
          ++hs_len_;
        }
      }
    }
    return;
  }

  // Footer loop: Enter release or a held Esc leave through the 0x3fd8 gate on the
  // shared menu phase. While Esc is held nothing runs and nothing is presented.
  if (KeyHeld(0x01)) {
    ClearReleases();
    menu_return_state_ = 1;
    menu_pending_ = 1;
    ++esc_hold_ticks_;
    return;
  }
  if (esc_hold_ticks_ > 0) {
    frontend_burst_ = std::min(esc_hold_ticks_ + 1, kStalePacerBurst);
    esc_hold_ticks_ = 0;
  }
  if (PeekRelease() == 0x1C && menu_pending_ == 0) {
    ClearReleases();
    menu_return_state_ = 1;
    menu_pending_ = 1;
  }
  StepFrontendPointfield();
  if (menu_pending_ == 0) {
    PulseRow(kHiscoreFooterRow, 0);
  }
  if (menu_pending_ == 1 && PulseRow(kHiscoreFooterRow, 1) == 1) {
    screen_exit_ = true;
  }
}

void MilestoneADemo::AdvanceTopOutTimers() {
  ++top_out_frame_;
  if (top_out_frame_ >= kTopOutHandoffFrame) {
    BeginHighScoreNameEntry(top_out_score_, top_out_lines_);
  }
}

void MilestoneADemo::TrackKeyDown(SDL_Keycode key) {
  const std::uint8_t original_scancode = OriginalScancodeForKey(key);
  if (original_scancode == 0) {
    return;
  }

  if (!held_original_scancodes_[original_scancode]) {
    held_original_scancode_frames_[original_scancode] = 0;
    audit_pressed_[original_scancode] = true;
  }
  held_original_scancodes_[original_scancode] = true;
}

void MilestoneADemo::TrackKeyUp(SDL_Keycode key) {
  const std::uint8_t original_scancode = OriginalScancodeForKey(key);
  if (original_scancode == 0) {
    return;
  }

  if (held_original_scancodes_[original_scancode]) {
    audit_released_[original_scancode] = true;
  }
  held_original_scancodes_[original_scancode] = false;
  held_original_scancode_frames_[original_scancode] = 0;
}

int MilestoneADemo::screen_fade_alpha() const {
  return 0;
}

const char* MilestoneADemo::FlowPhaseName() const {
  switch (flow_phase_) {
    case FlowPhase::kNone: return "none";
    case FlowPhase::kSplash: return "splash";
    case FlowPhase::kBootGap: return "boot-gap";
    case FlowPhase::kEntryPaletteOut: return "pal-out";
    case FlowPhase::kEntryTitleIn: return "title-in";
    case FlowPhase::kReveal: return "reveal";
    case FlowPhase::kScreen: return "screen";
    case FlowPhase::kConceal: return "conceal";
    case FlowPhase::kExitTitleOut: return "title-out";
    case FlowPhase::kExitPaletteIn: return "pal-in";
    case FlowPhase::kQuitTitleOut: return "quit-out";
    case FlowPhase::kMusicFreeze: return "music-freeze";
  }
  return "?";
}

void MilestoneADemo::PersistSetupState() const {
  if (!SaveSetupDat()) {
    SDL_LogWarn(
        SDL_LOG_CATEGORY_APPLICATION,
        "Failed to save SETUP.DAT state: %s",
        ATET_PORT_SETUP_DAT_PATH);
    return;
  }
  // The original writes SETUP.DAT only at the frontend exits (0x3604 from the
  // dispatcher's state 3 and exit tail) and when it creates the defaults; the log
  // line lets the suites check that an edit alone does not save (IN-14, HS-26).
  if (debug_state_logging_) {
    SDL_Log("Setup: SETUP.DAT saved at tick %u", static_cast<unsigned>(ticks_));
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
  audit_emitted_.push_back(AuditEmitted{audit_stage_, event_name, true, slot, music_track_index_, music_volume_,
                                        sfx_volume_override >= 0 ? sfx_volume_override : sfx_volume_, pan,
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
  audit_emitted_.push_back(
      AuditEmitted{audit_stage_, event_name, false, 0, music_track_index_, music_volume_, sfx_volume_, 0x80, 0});
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

void MilestoneADemo::BeginHighScoreNameEntry(int score, int lines) {
  // Game over -> 0x3830(9): the session loop clears the release history (0x4ba),
  // restores the area under GAME OVER only in the saved work buffer (0x23ac), and
  // the dispatcher entry fades the displayed picture out, fades the title in and
  // runs 0x50b0(1), which qualifies the run and starts the name entry.
  top_out_handoff_pending_ = false;
  const bool from_top_out = scene_ == Scene::kTopOut || gameplay_phase_ == GameplayPhase::kTopOutDissolve;
  if (from_top_out) {
    StartCurrentMusic("music-continue-topout-highscore");
  }
  top_out_score_ = score;
  top_out_lines_ = lines;
  ClearReleases();
  // Callers run from key events and Render (outside Tick); the original's session loop
  // reacts inside the tick that reads the release, so the entry starts with the next
  // Tick (its 63-tick fade then counts from that tick).
  state9_pending_ = true;
  state9_source_ = scene_ == Scene::kTopOut ? FrozenSource::kTopOut : FrozenSource::kGameplay;
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
  flow_phase_ = FlowPhase::kNone;  // --topout-demo starts here from the splash state
  scene_ = Scene::kTopOut;
}

void MilestoneADemo::BeginLiveGameplayDemo(int simulated_clear_count) {
  ResetLiveGameplayState();
  if (simulated_clear_count > 0) {
    InjectFilledRowsForSmoke(std::clamp(simulated_clear_count, 1, 4));
  }
  flow_phase_ = FlowPhase::kNone;  // --live-demo starts here from the splash state
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
  audit_gravity_active_ = true;
  audit_gravity_rate_ = CurrentDropIncrement();
  audit_gravity_acc_before_ = gravity_accumulator_;
  gravity_accumulator_ += CurrentDropIncrement();
  bool moved_down = false;
  while (gravity_accumulator_ >= kGameplayGravityThreshold) {
    gravity_accumulator_ -= kGameplayGravityThreshold;
    ++audit_descent_attempts_;
    if (TryMoveLivePiece(0, 1)) {
      moved_down = true;
      ++audit_descent_successes_;
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
  AuditScopedTicks audit_ticks(&g_audit.line_clear_seed_ticks);
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
    ++audit_particles_dropped_;
    return;
  }
  ++audit_particles_spawned_;
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
    ++audit_particles_dropped_;
    return;
  }
  ++audit_particles_spawned_;
  // Target velocity (0x3034): v = (target - start) * 65536 / lifetime.
  const int life = std::max(1, lifetime);
  const int vx = ((target_x - screen_x) << 16) / life;
  const int vy = ((target_y - screen_y) << 16) / life;
  const int decay = kDebrisAgeFull / life;
  debris_particles_.push_back(DebrisParticle{screen_x << 16, screen_y << 16, vx, vy, 0, decay, color});
}

void MilestoneADemo::StepDebrisParticles() {
  AuditScopedTicks audit_ticks(&g_audit.debris_step_ticks);
  const size_t audit_before = debris_particles_.size();
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
  audit_particles_retired_ += static_cast<std::uint32_t>(audit_before - debris_particles_.size());
  g_audit.debris_active = static_cast<std::uint32_t>(debris_particles_.size());
}

void MilestoneADemo::RenderDebrisParticles(SDL_Renderer* renderer) const {
  AuditScopedTicks audit_ticks(&g_audit.debris_render_ticks);
  g_audit.debris_active = static_cast<std::uint32_t>(debris_particles_.size());
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
    ++g_audit.debris_drawn;
  }
}

// GAP A: the original's top-out dissolve is not a silent black-out. As the well
// is cleared top-down, each row erupts into an upward particle fountain of its
// own cell colors (the line-clear/alert helper family), then the well empties.
// Drive that off the same dissolve front the black-out uses: for each row that
// newly crosses into the dissolved region, spray the certified debris pool
// upward from that row's filled cells.
void MilestoneADemo::UpdateTopOutDissolve() {
  AuditScopedTicks audit_ticks(&g_audit.topout_update_ticks);
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

  const std::uint32_t audit_value = gameplay_random_table_[static_cast<size_t>(gameplay_random_index_)];
  ++audit_rng_draws_;
  audit_rng_hash_ = Fnv1a64Update(audit_rng_hash_, &audit_value, sizeof(audit_value));
  if (audit_rng_values_.size() < 16384) {
    audit_rng_values_.push_back(audit_value);
  }
  return audit_value;
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
  // The amount the selected (pulsing) row was last drawn with: 0x3fd8 computes
  // trunc(sine[phase] / 2048) + 0x30 on the original's peak-32768 table.
  int row = frontend_selected_row_;
  if (frontend_view_ == FrontendView::kMainMenu) {
    row = main_menu_row_;
  } else if (frontend_view_ == FrontendView::kHighScoreEntry && hs_slot_ >= 0) {
    row = hs_slot_;
  } else if (frontend_view_ == FrontendView::kHighScores) {
    row = kHiscoreFooterRow;
  }
  if (row < 0 || row >= kFrontendRowCount) {
    return 0;
  }
  return row_amount_[static_cast<size_t>(row)];
}

int MilestoneADemo::VisibleRowCount() const {
  const bool high_score_table =
      frontend_view_ == FrontendView::kHighScores || frontend_view_ == FrontendView::kHighScoreEntry;
  int visible = 0;
  for (int row = 0; row < kFrontendRowCount; ++row) {
    const bool has_text = !row_text_[static_cast<size_t>(row)].empty() || (high_score_table && row < 5);
    if (row_amount_[static_cast<size_t>(row)] > 0 && has_text) {
      ++visible;
    }
  }
  return visible;
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

// ============================================================================
// Audit instrumentation (audit/instrument worktree only). Read-only views of the
// demo state plus per-step accumulators reset by AuditBeginStep.
// ============================================================================
namespace {

const char* AuditActionName(size_t index) {
  static constexpr const char* kNames[8] = {"up",          "down",         "left",    "right",
                                            "rotate_left", "rotate_right", "confirm", "escape"};
  return index < 8 ? kNames[index] : "?";
}

}  // namespace

void MilestoneADemo::AuditBeginStep(std::uint64_t step) {
  audit_step_ = step;
  audit_stage_ = "input";
  audit_emitted_.clear();
  audit_rng_index_before_ = gameplay_random_index_;
  audit_rng_draws_ = 0;
  audit_rng_hash_ = kFnvOffsetBasis;
  audit_rng_values_.clear();
  audit_gravity_active_ = false;
  audit_gravity_rate_ = 0;
  audit_gravity_acc_before_ = gravity_accumulator_;
  audit_descent_attempts_ = 0;
  audit_descent_successes_ = 0;
  audit_score_before_ = live_score_;
  audit_particles_spawned_ = 0;
  audit_particles_dropped_ = 0;
  audit_particles_retired_ = 0;
  audit_pressed_.fill(false);
  audit_released_.fill(false);
  audit_held_at_step_start_ = held_original_scancodes_;
}

void MilestoneADemo::AuditWritePresentation(JsonOut& json) const {
  json.BeginObject()
      .Key("splash_frame").Int(splash_frame_)
      .Key("frontend_frame_counter").Int(frontend_frame_counter_)
      .Key("frontend_view_frame").Int(frontend_view_frame_)
      .Key("pulse").Int(PulseRevealAmount())
      .Key("top_out_frame").Int(top_out_frame_)
      .Key("top_out_dissolved_pixel_rows").Int(top_out_dissolved_rows_)
      .Key("debris").UInt(debris_particles_.size())
      .Key("fade_alpha").Int(screen_fade_alpha())
      .Key("tick").UInt(ticks_)
      .Key("flow").Str(FlowPhaseName())
      .EndObject();
}

void MilestoneADemo::AuditWriteEmittedEvents(JsonOut& json) const {
  json.BeginArray();
  for (const AuditEmitted& event : audit_emitted_) {
    json.BeginObject().Key("stage").Str(event.stage).Key("kind").Str(event.is_sfx ? "sfx" : "music").Key("name").Str(
        event.name);
    if (event.is_sfx) {
      json.Key("slot").Int(event.slot).Key("sfx_volume").Int(event.sfx_volume).Key("pan").Int(event.pan).Key(
          "voice_class").Int(event.voice_class);
    } else {
      json.Key("track").Int(event.track).Key("music_volume").Int(event.music_volume);
    }
    json.EndObject();
  }
  json.EndArray();
}

void MilestoneADemo::AuditWriteState(JsonOut& json, bool include_rng_values) const {
  const char* scene = "frontend";
  switch (scene_) {
    case Scene::kSplash: scene = "splash"; break;
    case Scene::kFrontend: scene = "frontend"; break;
    case Scene::kGameplay: scene = "gameplay"; break;
    case Scene::kTopOut: scene = "topout"; break;
  }
  const char* view = "main_menu";
  switch (frontend_view_) {
    case FrontendView::kMainMenu: view = "main_menu"; break;
    case FrontendView::kOptions: view = "options"; break;
    case FrontendView::kKeyboardSetup: view = "keyboard_setup"; break;
    case FrontendView::kHighScores: view = "high_scores"; break;
    case FrontendView::kHighScoreEntry: view = "high_score_entry"; break;
    case FrontendView::kCredits: view = "credits"; break;
    case FrontendView::kSoundSetup: view = "sound_setup"; break;
  }
  const char* phase = "falling";
  if (gameplay_phase_ == GameplayPhase::kCollapse) {
    phase = "collapse";
  } else if (gameplay_phase_ == GameplayPhase::kTopOutDissolve) {
    phase = "topout_dissolve";
  }
  // Fade fields keep their old names: state = the dispatcher fade running
  // ("pal-out" 0x64f0, "title-in" 0x63b8, "title-out" 0x62e0, "pal-in" 0x6498,
  // "quit-out" state 3), frame = e (ticks since that fade started).
  const char* fade = "none";
  switch (flow_phase_) {
    case FlowPhase::kEntryPaletteOut: fade = "pal-out"; break;
    case FlowPhase::kEntryTitleIn: fade = "title-in"; break;
    case FlowPhase::kExitTitleOut: fade = "title-out"; break;
    case FlowPhase::kExitPaletteIn: fade = "pal-in"; break;
    case FlowPhase::kQuitTitleOut: fade = "quit-out"; break;
    default: break;
  }
  const char* pending = "none";
  switch (flow_exit_) {
    case FlowExit::kNone: pending = dispatch_state_ == 1 && flow_phase_ == FlowPhase::kEntryPaletteOut ? "frontend_from_gameplay" : "none"; break;
    case FlowExit::kNewGame: pending = "gameplay_new_game"; break;
    case FlowExit::kResume: pending = "gameplay_resume"; break;
    case FlowExit::kSplashes: pending = "splashes"; break;
  }

  json.Key("scene").Str(scene).Key("scene_name").Str(scene_name());
  json.Key("splash").BeginObject().Key("page").Int(splash_page_).Key("frame").Int(splash_frame_).EndObject();
  json.Key("session")
      .BeginObject()
      .Key("live_game_active").Bool(live_game_active_)
      .Key("live_game_available").Bool(live_game_available_)
      .Key("quit_requested").Bool(quit_requested_)
      .Key("gameplay_preset").Int(gameplay_preset_index_)
      .EndObject();
  json.Key("phase").Str(phase);

  // Piece (port numbering; geometry from RotatedCells, i.e. what draw/collision use).
  json.Key("piece").BeginObject().Key("id").Int(live_piece_.type).Key("rotation").Int(live_piece_.rotation).Key("x").Int(
      live_piece_.x).Key("y").Int(live_piece_.y).Key("next").Int(live_next_piece_type_);
  json.Key("cells").BeginArray();
  for (const CellOffset& cell : RotatedCells(live_piece_.type, live_piece_.rotation)) {
    json.BeginArray().Int(live_piece_.x + cell.x).Int(live_piece_.y + cell.y).EndArray();
  }
  json.EndArray();
  json.Key("stats").BeginArray();
  for (const int count : live_piece_stats_) {
    json.Int(count);
  }
  json.EndArray().EndObject();

  // Gravity bracket for this step (StepFallingGameplay); rate includes the 0x8000 soft-drop override.
  json.Key("gravity").BeginObject().Key("stepped").Bool(audit_gravity_active_);
  if (audit_gravity_active_) {
    json.Key("rate").Int(audit_gravity_rate_).Key("acc_before").Int(audit_gravity_acc_before_);
  } else {
    json.Key("rate").Null().Key("acc_before").Null();
  }
  json.Key("acc_after").Int(gravity_accumulator_)
      .Key("natural_rate").Int(CurrentGravityRate())
      .Key("soft_drop_override").Bool(audit_gravity_active_ && audit_gravity_rate_ == kSoftDropIncrement &&
                                      CurrentGravityRate() != kSoftDropIncrement)
      .Key("descent_attempts").Int(audit_descent_attempts_)
      .Key("descent_successes").Int(audit_descent_successes_)
      .Key("lock_frames").Int(lock_frames_)
      .Key("down_lockout").Int(down_lockout_frames_)
      .EndObject();

  // Input: the eight semantic actions (v1 trace order) mapped through the bindings.
  const std::array<std::uint8_t, 8> action_scancodes = {
      0xC8, keyboard_bindings_[0], keyboard_bindings_[1], keyboard_bindings_[2],
      keyboard_bindings_[3], keyboard_bindings_[4], 0x1C, 0x01};
  auto write_set = [&](const std::array<bool, 256>& states) {
    json.BeginArray();
    for (size_t i = 0; i < action_scancodes.size(); ++i) {
      const std::uint8_t sc = action_scancodes[i];
      if (sc != 0 && states[sc]) {
        json.Str(AuditActionName(i));
      }
    }
    json.EndArray();
  };
  json.Key("input").BeginObject();
  json.Key("held_at_step_start");
  write_set(audit_held_at_step_start_);
  json.Key("held");
  write_set(held_original_scancodes_);
  json.Key("pressed");
  write_set(audit_pressed_);
  json.Key("released");
  write_set(audit_released_);
  json.Key("held_frames").BeginObject();
  for (size_t i = 0; i < action_scancodes.size(); ++i) {
    const std::uint8_t sc = action_scancodes[i];
    json.Key(AuditActionName(i)).Int(sc != 0 && held_original_scancodes_[sc] ? held_original_scancode_frames_[sc] : 0);
  }
  json.EndObject();
  // Gameplay auto-repeat fires in Tick when held_frames > 0 && held_frames % 8 == 0
  // (kGameplayBindingRepeatFrames); the frontend Left/Right edit repeats at 12 then every 4.
  json.Key("das_period").Int(kGameplayBindingRepeatFrames);
  json.Key("held_raw").BeginArray();
  for (size_t sc = 0; sc < held_original_scancodes_.size(); ++sc) {
    if (held_original_scancodes_[sc]) {
      json.Int(static_cast<int>(sc));
    }
  }
  json.EndArray().EndObject();

  // RNG (consumption 0x3280 model): draws this step, FNV-1a64 over the raw LE u32 values.
  json.Key("rng")
      .BeginObject()
      .Key("seed").UInt(piece_seed_)
      .Key("state").UInt(gameplay_random_state_)
      .Key("index_before").Int(audit_rng_index_before_)
      .Key("index_after").Int(gameplay_random_index_)
      .Key("draws").UInt(audit_rng_draws_)
      .Key("values_fnv1a64").Str(Hex64(audit_rng_hash_));
  json.Key("values");
  if (include_rng_values || audit_rng_draws_ <= 8) {
    json.BeginArray();
    for (const std::uint32_t value : audit_rng_values_) {
      json.UInt(value);
    }
    json.EndArray();
  } else {
    json.Null();
  }
  json.EndObject();

  // Board: 20 rows top->bottom, 10 cells left->right, value = cell byte (0 empty,
  // 1..7 = port piece type + 1). Hash = FNV-1a64 over the 200 bytes (v1 board domain).
  std::uint64_t board_hash = kFnvOffsetBasis;
  for (const int cell : live_board_cells_) {
    const std::uint8_t byte = static_cast<std::uint8_t>(cell);
    board_hash = Fnv1a64Update(board_hash, &byte, 1);
  }
  json.Key("board").BeginObject().Key("fnv1a64").Str(Hex64(board_hash)).Key("rows").BeginArray();
  for (int y = 0; y < kBoardHeight; ++y) {
    std::string row;
    for (int x = 0; x < kBoardWidth; ++x) {
      row += static_cast<char>('0' + std::clamp(live_board_cells_[static_cast<size_t>(y * kBoardWidth + x)], 0, 9));
    }
    json.Str(row);
  }
  json.EndArray().EndObject();

  json.Key("score")
      .BeginObject()
      .Key("value").Int(live_score_)
      .Key("lines").Int(live_lines_)
      .Key("level").Int(live_level_)
      .Key("start_level").Int(start_level_)
      .Key("delta").Int(live_score_ - audit_score_before_)
      .EndObject();

  json.Key("clear").BeginObject().Key("rows").BeginArray();
  for (const int row : pending_clear_rows_) {
    json.Int(row);
  }
  json.EndArray().Key("collapse_frame").Int(collapse_frame_).EndObject();

  json.Key("particles")
      .BeginObject()
      .Key("active").UInt(debris_particles_.size())
      .Key("spawned").UInt(audit_particles_spawned_)
      .Key("dropped_at_cap").UInt(audit_particles_dropped_)
      .Key("retired").UInt(audit_particles_retired_)
      .EndObject();

  json.Key("warning")
      .BeginObject()
      .Key("band").Int(warning_band_)
      .Key("cooldowns").BeginArray().Int(warning_cooldowns_[0]).Int(warning_cooldowns_[1]).Int(warning_cooldowns_[2]).EndArray()
      .Key("sleepy").Bool(sleepy_)
      .Key("frames_since_clear").Int(frames_since_clear_)
      .EndObject();

  json.Key("alert").BeginObject().Key("id");
  if (alert_effect_id_ >= 0) {
    json.Int(alert_effect_id_);
  } else {
    json.Null();
  }
  json.Key("lifetime").Int(alert_lifetime_).Key("reveal_frame").Int(alert_reveal_frame_).EndObject();

  json.Key("topout")
      .BeginObject()
      .Key("frame").Int(top_out_frame_)
      .Key("gameplay_frame").Int(gameplay_top_out_frame_)
      .Key("dissolved_pixel_rows").Int(top_out_dissolved_rows_)
      .Key("overlay_visible").Bool(scene_ == Scene::kTopOut && top_out_frame_ >= kTopOutOverlayStartFrame &&
                                   top_out_frame_ < kTopOutOverlayEndFrame)
      .Key("saved_under_valid").Bool(top_out_saved_under_valid_)
      .Key("handoff_pending").Bool(top_out_handoff_pending_)
      .EndObject();

  json.Key("frontend")
      .BeginObject()
      .Key("view").Str(view)
      .Key("selected_row").Int(frontend_view_ == FrontendView::kMainMenu ? main_menu_row_ : frontend_selected_row_)
      .Key("view_frame").Int(frontend_view_frame_)
      .Key("frame_counter").Int(frontend_frame_counter_)
      .Key("cycle_counter").Int(frontend_cycle_counter_)
      .Key("bank").Int(frontend_current_bank_)
      .Key("next_bank").Int(frontend_next_bank_)
      .Key("pulse").Int(PulseRevealAmount())
      .Key("visible_rows").Int(scene_ == Scene::kFrontend ? VisibleRowCount() : 0)
      .Key("conceal_frames").Int(flow_phase_ == FlowPhase::kConceal ? kRevealSteps - flow_step_ : 0)
      .Key("hs_bootstrap_remaining").Int(0)
      .Key("credits_page").Int(credits_page_)
      .Key("credits_timer").Int(credits_counter_)
      .Key("keyboard_capture_row").Int(keyboard_capture_row_)
      .Key("entry_row").Int(frontend_view_ == FrontendView::kHighScoreEntry ? hs_slot_ : -1)
      .Key("entry_name").Str(hs_buffer_)
      .Key("commit_pending").Bool(hs_commit_);
  // Dispatcher-model state (fix-frontend-flow prototype).
  json.Key("flow").Str(FlowPhaseName())
      .Key("flow_step").Int(flow_step_)
      .Key("dispatch_state").Int(dispatch_state_)
      .Key("pending").Int(menu_pending_)
      .Key("return_state").Int(menu_return_state_)
      .Key("main_menu_row").Int(main_menu_row_)
      .Key("pulse_phase").Int(pulse_phase_)
      .Key("entry_pulse_phase").Int(entry_pulse_phase_)
      .Key("title_level").Int(CurrentTitleLevel())
      .Key("splash_level").Int(CurrentSplashLevel())
      .Key("frozen_level").Int(CurrentFrozenLevel())
      .Key("release_newest").Int(PeekRelease())
      .Key("burst_next").Int(frontend_burst_)
      .Key("credits_mode").Int(credits_mode_);
  json.Key("pf")
      .BeginObject()
      .Key("ang_xy").Int(frontend_pointfield_state_.angle_xy)
      .Key("ang_z").Int(frontend_pointfield_state_.angle_z)
      .Key("ph_x").Int(frontend_pointfield_state_.phase_x)
      .Key("ph_y").Int(frontend_pointfield_state_.phase_y)
      .Key("ph_z").Int(frontend_pointfield_state_.phase_z)
      .Key("org_x").Int(frontend_pointfield_state_.origin_x)
      .Key("org_y").Int(frontend_pointfield_state_.origin_y)
      .Key("org_z").Int(frontend_pointfield_state_.origin_z)
      .EndObject();
  json.Key("row_amounts").BeginArray();
  for (const int amount : row_amount_) {
    json.Int(amount);
  }
  json.EndArray();
  json.Key("row_texts").BeginArray();
  for (const std::string& text : row_text_) {
    json.Str(text);
  }
  json.EndArray();
  json.EndObject();

  json.Key("fade")
      .BeginObject()
      .Key("state").Str(fade)
      .Key("frame").Int(FlowFadeE())
      .Key("alpha").Int(screen_fade_alpha())
      .Key("pending_scene").Str(pending)
      .Key("enabled").Bool(fades_enabled_)
      .EndObject();

  json.Key("settings")
      .BeginObject()
      .Key("music_volume").Int(music_volume_)
      .Key("sfx_volume").Int(sfx_volume_)
      .Key("music_track_index").Int(music_track_index_)
      .EndObject();
}

}  // namespace atet
