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
// 0x9C8 (0xB33/0xB91/0xBFF/0xC6F): a Left/Right/A/S action reloads its repeat timer with 8.
constexpr int kGameplayBindingRepeatFrames = 8;
constexpr int kGameplayGravityThreshold = 0x10000;
// 0x9C8 0xCB4..0xCBD: while Down is held and the Down lockout is 0 the per-step gravity increment
// is replaced by 0x8000 (half the descent threshold) => one row every two steps.
constexpr int kSoftDropIncrement = 0x8000;
// 0x1D04: each cleared row is collapsed over 8 calls (0x1D68 cmp ebx,8), one call per step.
constexpr int kCollapseStepsPerRow = 8;
// 0x9C8 0xE6F..0xE76: a clearing lock arms the Down lockout [0x2C783] with 0x348 live steps.
constexpr int kDownLockoutAfterClear = 0x348;
// 0x1348 0x13C6..0x13DF: every promoted piece starts at x=4, y=0, rotation 0.
constexpr int kSpawnX = 4;
constexpr int kGameplayMaxDebrisParticles = 4096;  // 0x1000, matching the original particle-pool cap
constexpr int kDebrisAgeFull = 0x400;              // age accumulator reaches this at end of life (4 stages of 0x100)
// Stack warnings (0x21C4, start of every normal step): r = first occupied row - 1;
// r < 2 high (face 5), r < 4 mid (face 4), r < 7 low (face 3). Every in-band step reloads
// its band's cooldown (and the lower ones) to 300, so a face and its sound come once per
// climb into a band whose cooldown has run out.
constexpr int kWarningHighLimit = 2;
constexpr int kWarningMidLimit = 4;
constexpr int kWarningLowLimit = 7;
constexpr int kWarningCooldownTicks = 0x12C;
// Mood faces (0x2008 trigger / 0x206C update). Lifetimes count logic steps; the sleepy and
// fast-follow tests compare the PIT tick counter 0x2D2A3 (Tick32(), which also runs through
// menus and fades) with the tick of the last clear.
constexpr int kAlertFaceX = 20;   // 0x1793D / 0x2138: working screen + 0xAF14 = (20,140)
constexpr int kAlertFaceY = 140;
constexpr int kAlertFaceSize = 50;
constexpr Uint8 kAlertTransparentIndex = 0x20;
constexpr int kAlertClearLifetime = 0xA0;    // clear, wake, fast-follow and warning faces
constexpr int kAlertSleepyLifetime = 0x140;  // 0x10B8
constexpr int kAlertInfiniteLifetime = -1;   // top-out face (0x1037)
constexpr std::uint32_t kSleepyTicksSinceClear = 0x708;  // exactly 1800 ticks (0x10A6)
constexpr std::uint32_t kFastFollowTicks = 0xA0;         // 0x0F5F / 0x0F83, unsigned compare
constexpr std::array<int, 6> kAlertRevealRows = {50, 40, 30, 20, 10, 1};  // table 0x9B0, by counter - 1
// (The old 120-frame splash/screen fade constants were fitted to a capture; every
// frontend fade is now the original's tick-exact palette level, see kFadeTicks.)
// Top-out (0x1F8C/0x1EDC): from the tick after the failed spawn, one 80-pixel scanline of
// the well per tick (y = 21..180) is sprayed and erased; GAME OVER (0x22FC, 128x36 at
// (85,83)) is drawn in the tick of the 160th. Nothing times out after that: the session
// loop leaves the game-over screen only on an Esc release (0x4A3..0x4D6).
constexpr int kTopOutDissolveFrames = kBoardHeight * kPieceTileSize;  // 160 scanlines
constexpr int kGameOverX = 0x55;
constexpr int kGameOverY = 0x53;
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

// The original's authored piece table (flat 0x184E3, 7 ids x 4 rotations x 4 cells of int32 (dx,dy),
// 896 bytes, sha256 330c1ec9...; read by collision 0x12AC, lock 0x9C8 0xD6B and draw 0x10EC). Ids are
// the RNG value % 7 and also select the chunk-3 tile and the statistics row: 0 S, 1 Z, 2 J, 3 L, 4 T,
// 5 I, 6 O. Rotation +1 (key A) is clockwise on screen; S, Z and I have two distinct states, and the
// offsets keep the pieces' turning cell fixed, so x/y never change on a turn. Cells are listed in the
// table's own order.
constexpr int kPieceCount = 7;
constexpr std::array<std::array<std::array<CellOffset, 4>, 4>, kPieceCount> kPieceTable = {{
    {{  // 0 S
        {{{1, 1}, {2, 1}, {2, 0}, {3, 0}}},
        {{{2, -1}, {2, 0}, {3, 0}, {3, 1}}},
        {{{1, 1}, {2, 1}, {2, 0}, {3, 0}}},
        {{{2, -1}, {2, 0}, {3, 0}, {3, 1}}},
    }},
    {{  // 1 Z
        {{{1, 0}, {2, 0}, {2, 1}, {3, 1}}},
        {{{3, -1}, {3, 0}, {2, 0}, {2, 1}}},
        {{{1, 0}, {2, 0}, {2, 1}, {3, 1}}},
        {{{3, -1}, {3, 0}, {2, 0}, {2, 1}}},
    }},
    {{  // 2 J
        {{{1, 0}, {2, 0}, {3, 0}, {3, 1}}},
        {{{2, -1}, {2, 0}, {2, 1}, {1, 1}}},
        {{{1, 0}, {2, 0}, {3, 0}, {1, -1}}},
        {{{2, -1}, {2, 0}, {2, 1}, {3, -1}}},
    }},
    {{  // 3 L
        {{{1, 0}, {2, 0}, {3, 0}, {1, 1}}},
        {{{2, -1}, {2, 0}, {2, 1}, {1, -1}}},
        {{{1, 0}, {2, 0}, {3, 0}, {3, -1}}},
        {{{2, -1}, {2, 0}, {2, 1}, {3, 1}}},
    }},
    {{  // 4 T
        {{{1, 0}, {2, 0}, {3, 0}, {2, 1}}},
        {{{2, -1}, {2, 0}, {2, 1}, {1, 0}}},
        {{{1, 0}, {2, 0}, {3, 0}, {2, -1}}},
        {{{2, -1}, {2, 0}, {2, 1}, {3, 0}}},
    }},
    {{  // 5 I
        {{{0, 0}, {1, 0}, {2, 0}, {3, 0}}},
        {{{2, -2}, {2, -1}, {2, 0}, {2, 1}}},
        {{{0, 0}, {1, 0}, {2, 0}, {3, 0}}},
        {{{2, -2}, {2, -1}, {2, 0}, {2, 1}}},
    }},
    {{  // 6 O
        {{{2, 0}, {3, 0}, {2, 1}, {3, 1}}},
        {{{2, 0}, {3, 0}, {2, 1}, {3, 1}}},
        {{{2, 0}, {3, 0}, {2, 1}, {3, 1}}},
        {{{2, 0}, {3, 0}, {2, 1}, {3, 1}}},
    }},
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

  // SDL_LoadFile takes a UTF-8 path on every platform. std::ifstream(const char*)
  // reads the path in the ANSI code page on Windows, so data in a folder with a
  // non-ASCII name (SDL_GetBasePath() is UTF-8) was never found.
  size_t size = 0;
  void* data = SDL_LoadFile(path, &size);
  if (data == nullptr) {
    SetErrorMessage(error_out, "Failed to open binary asset: " + std::string(path) + " (" + SDL_GetError() + ")");
    return false;
  }

  const Uint8* begin = static_cast<const Uint8*>(data);
  bytes_out->assign(begin, begin + size);
  SDL_free(data);
  return true;
}

// An absolute spelling of a search folder ("" = the working directory), always with
// a trailing separator, for SETUP.DAT paths and log lines.
std::string AbsoluteDir(const std::string& dir) {
  if (!dir.empty()) {
    return dir;  // SDL_GetBasePath() results are absolute and end with a separator
  }
  if (char* cwd = SDL_GetCurrentDirectory()) {
    std::string result(cwd);
    SDL_free(cwd);
    return result;
  }
  return dir;  // still resolves against the working directory
}

// True when `path` can be written without changing an existing file: an existing
// regular file must open for appending, a missing one must be creatable (the empty
// probe is removed again, so a first run still finds no SETUP.DAT and writes the
// defaults, as the original does).
bool IsWritableFile(const std::string& path) {
  SDL_PathInfo info;
  const bool exists = SDL_GetPathInfo(path.c_str(), &info);
  if (exists && info.type != SDL_PATHTYPE_FILE) {
    return false;
  }
  SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "ab");
  if (stream == nullptr) {
    return false;
  }
  SDL_CloseIO(stream);
  if (!exists) {
    SDL_RemovePath(path.c_str());
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

// The particle polar table exactly as 0x27B8 builds it: 512 values
// chop(sin(i * 0.0030679616) * 32769.0) (doubles at 0x17AE8/0x17AF0), mirrored into 2048
// entries, so t[1] = 100 and t[0x200] = 32768. The nearest value to an integer is 0.002
// away, so double precision reproduces the original's x87 table on every platform.
std::array<int, 2048> BuildParticleSineTable() {
  std::array<int, 2048> table{};
  for (int index = 0; index < 0x200; ++index) {
    const int value = static_cast<int>(std::trunc(std::sin(index * 0.0030679616) * 32769.0));
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

// Cells of piece `id` in `rotation`, straight from the authored table (0x12AC masks the rotation
// with 3). Nothing is synthesised or re-anchored.
std::array<CellOffset, 4> RotatedCells(int id, int rotation) {
  return kPieceTable[static_cast<size_t>(NormalizeModulo(id, kPieceCount))][static_cast<size_t>(rotation & 3)];
}

// Debug overlay presets only (keys 1-3, not gameplay): rotation-0 cells moved to start at (0,0)
// so the hand-placed preset pieces stay inside the well.
void DrawTetromino(
    SDL_Renderer* renderer,
    SDL_Texture* atlas,
    int atlas_index,
    int shape_index,
    int screen_x,
    int screen_y) {
  const std::array<CellOffset, 4> cells = RotatedCells(shape_index, 0);
  int min_x = cells[0].x;
  int min_y = cells[0].y;
  for (const CellOffset& cell : cells) {
    min_x = std::min(min_x, cell.x);
    min_y = std::min(min_y, cell.y);
  }
  for (const CellOffset& cell : cells) {
    DrawPieceTile(
        renderer,
        atlas,
        atlas_index,
        screen_x + ((cell.x - min_x) * kPieceTileSize),
        screen_y + ((cell.y - min_y) * kPieceTileSize));
  }
}

// NEXT preview, 0x1348 -> 0x10EC(0x1E, 0x38, next, 0): rotation-0 cells at (30 + 8dx, 56 + 8dy) with
// the raw table offsets, so every piece's right edge is x 61 (3-wide pieces 38..61, the I 30..61,
// the O 46..61).
void DrawPreviewPiece(SDL_Renderer* renderer, SDL_Texture* atlas, int id) {
  for (const CellOffset& cell : RotatedCells(id, 0)) {
    DrawPieceTile(
        renderer,
        atlas,
        NormalizeModulo(id, kPieceCount),
        kPreviewX + (cell.x * kPieceTileSize),
        kPreviewY + (cell.y * kPieceTileSize));
  }
}

// The falling piece, 0x10EC: tile = id, cell (x+dx, y+dy) at (109 + 8(x+dx), 21 + 8(y+dy)); cells
// above the well (screen y < 21, i.e. row < 0) are skipped (0x113C).
void DrawLiveTetromino(
    SDL_Renderer* renderer,
    SDL_Texture* atlas,
    int piece_type,
    int rotation,
    int board_x,
    int board_y) {
  for (const CellOffset& cell : RotatedCells(piece_type, rotation)) {
    if (board_y + cell.y < 0) {
      continue;
    }
    DrawPieceTile(
        renderer,
        atlas,
        NormalizeModulo(piece_type, kPieceCount),
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
  std::string data_dir;  // folder of the data file actually loaded ("" = working directory)
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
        data_dir = dir;
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
        data_dir = dir;
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
      const std::string fallback = ATET_PORT_ATET_DAT_PATH;
      const size_t separator = fallback.find_last_of("/\\");
      data_dir = separator == std::string::npos ? std::string{} : fallback.substr(0, separator + 1);
      g_audit_startup.data_source = std::string("compiled-fallback:") + ATET_PORT_ATET_DAT_PATH;
    }
  }
  g_audit_startup.data_ticks = SDL_GetPerformanceCounter() - audit_t_data;
  const Uint64 audit_t_textures = SDL_GetPerformanceCounter();
  if (!loaded) {
    std::string searched;
    for (const std::string& dir : search_dirs) {
      searched += (searched.empty() ? "" : ", ") + AbsoluteDir(dir);
    }
    SetErrorMessage(error_out,
                    "Could not load the game data (" + dat_error +
                        "). Place ATET.DAT (or SABA's saba.dat), or an ACiD Tetris / SABA "
                        "archive, beside the executable. Folders searched: " + searched);
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
  // Frontend flow state. The PIT tick counter [0x2d2a3] is the main loop's tick (Tick32()); the
  // timer is installed just before the first splash, so the splash t0 is tick 0.
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
  top_out_frame_ = 0;
  top_out_dissolved_rows_ = 0;
  top_out_source_cells_.fill(0);
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
  repeat_timers_.fill(0);
  down_lockout_frames_ = 0;
  pending_clear_rows_.clear();
  collapse_frame_ = 0;
  collapse_step_ = 0;
  collapse_batch_ = 0;
  live_piece_hidden_ = false;
  top_out_after_collapse_ = false;
  warning_band_ = 0;
  warning_cooldowns_.fill(0);
  debris_particles_.clear();  // startup only (0x2DA0, single caller 0x126)
  debris_hidden_after_frontend_ = false;
  alert_corner_hold_ = false;
  live_snapshot_ = LiveSnapshot{};
  gameplay_snapshot_texture_valid_ = false;
  // Startup: main fills the shared RNG table at 0x150 with srand([0x2D2A3]) = srand(0), before the
  // PIT is installed at 0x16D; the tick counter (tick_count_, set by the main loop) starts from 0.
  // That one fill is SeedBootRandomTable() above, which the pointfield's start bank already drew
  // from; a second fill here would rewind the shared index.
  piece_seed_ = 0;
  last_clear_was_four_ = false;
  last_clear_tick_ = 0;
  alert_effect_id_ = -1;
  alert_lifetime_ = 0;
  alert_reveal_counter_ = 0;
  AlertCornerRestore();
  audio_events_.clear();
  line_clear_demo_count_ = 0;
  ResetSetupStateToDefaults();
  live_game_available_ = false;
  quit_requested_ = false;
  ResolveSetupDatPath(data_dir);
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
    // The keyboard ISR 0x800 records only key RELEASES for the session loop; a press, and
    // any key but Esc, changes nothing on the dissolve or the GAME OVER screen. The Esc
    // release is read from the release history at the start of the next tick (TickLogic).
    // Keys are swallowed so main's debug probe stays silent.
    return true;
  }

  if (scene_ == Scene::kGameplay && flow_phase_ == FlowPhase::kNone) {
    // In play Esc acts on its RELEASE (main 0x4a3: newest release == 0x01),
    // checked once per tick in Tick(); the press itself does nothing.
    if (key == SDLK_ESCAPE) {
      return true;
    }
    if (debug_keys_enabled_ && key == SDLK_H) {
      BeginHighScoreNameEntry(CurrentRunScore(), CurrentRunLines());
      return true;
    }
    if (debug_keys_enabled_ && key == SDLK_O) {
      BeginTopOutDemo(CurrentRunScore(), CurrentRunLines());
      return true;
    }
    if (debug_keys_enabled_ && key == SDLK_C && live_game_active_) {
      InjectFilledRowsForSmoke((line_clear_demo_count_ % 4) + 1);
      ++line_clear_demo_count_;
      return true;
    }
    if (debug_keys_enabled_ && key == SDLK_B && live_game_active_) {
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

void MilestoneADemo::TickLogic() {
  // One logic step. The PIT counter [0x2d2a3] is the main loop's tick (set_tick_count, read through
  // Tick32()): every step of one catch-up frame reads the same value, as the original's steps do.
  const bool frame_start = frame_start_pending_;
  frame_start_pending_ = false;
  if (frame_start) {
    frame_batch_done_ = false;
  }

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
    // The catch-up steps of a frame (the 0x24d0 pacer's return, 1..6) each run one screen tick; a
    // screen loop that sets its exit flag (a gate returned 1) is left at once, so the rest of the
    // frame's steps run no screen logic.
    if (!frame_batch_done_) {
      TickFrontendOnce(frame_start);
      if (scene_ == Scene::kGameplay || quit_requested_ || screen_exit_) {
        frame_batch_done_ = true;
      }
    }
    if (scene_ != Scene::kGameplay && scene_ != Scene::kTopOut) {
      FlushAudioEvents();
      return;
    }
    // The dispatcher exit finished in this step: main's session loop runs its first step in the
    // same tick (0x488 / 0x4db), a gameplay step or, after Return to Game from an Esc during the
    // dissolve, a game-over scene step, so fall through. The main loop counts this step as the
    // first step of the pacer frame that follows the 0x6498 tick loop.
  }

  if (scene_ == Scene::kTopOut) {
    // Session loop 0x4A3..0x4D6 at the start of the tick: a released Esc (the newest entry of the
    // release history) clears the history and leaves before anything else of this tick runs.
    if (PeekRelease() == 0x01) {
      ClearReleases();
      if (top_out_dissolved_rows_ >= kTopOutDissolveFrames) {
        // After GAME OVER: 0x23AC (save-under back into the working screen only) and 0x3830(9):
        // the displayed GAME OVER frame fades out (0x64f0), then state 9.
        BeginHighScoreNameEntry(top_out_score_, top_out_lines_);
        state9_pending_ = false;
        BeginDispatcherEntry(9, FrozenSource::kTopOut);
      } else {
        // During the dissolve: 0x3830(1), the main menu, with the game still resumable (the live
        // flag 0x2C72B drops only in the GAME OVER tick); Return to Game continues the dissolve.
        SaveLiveSnapshot();
        BeginDispatcherEntry(1, FrozenSource::kTopOut);
      }
      FlushAudioEvents();
      return;
    }
    // One tick of the game-over scene: particles, one dissolve scanline (GAME OVER with the
    // 160th), the face update. A catch-up frame advances it once per step.
    StepTopOutScene();
    FlushAudioEvents();
    return;
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
    // The whole gameplay step, input included (0x9C8): held-key state is read inside the step,
    // before gravity, with the original's four 8-step repeat timers (StepGameplayInput).
    StepLiveGameplay();
  }
  FlushAudioEvents();
}

void MilestoneADemo::TickPresentation() {
  // timing-core's split: the per-step counters the original advances after the frame's draw. Every
  // frontend counter is advanced in the logic step before the draw (frontend-flow's order, matched
  // frame by frame against the original), and the game-over scene steps itself in TickLogic
  // (effects-alerts: nothing times out after GAME OVER), so nothing is left here; the hook stays
  // for the main loop's step/draw contract.
}

bool MilestoneADemo::InBlockingPhase() const {
  // The original's tick loops: one pass per tick, no flip and no 0x24d0 pacer call, so the pacer's
  // baseline goes stale and its first return afterwards is capped at 6 (timing-core contract).
  // The object fades 0x62e0 / 0x63b8, the reveal and conceal and every screen are pacer loops.
  switch (flow_phase_) {
    case FlowPhase::kSplash:           // 0x2998
    case FlowPhase::kBootGap:          // chunk loads, then 0x6544's 32-tick wait
    case FlowPhase::kEntryPaletteOut:  // 0x64f0
    case FlowPhase::kExitPaletteIn:    // 0x6498
      return true;
    default:
      return InNestedBlockingPhase();
  }
}

bool MilestoneADemo::InNestedBlockingPhase() const {
  // Tick loops entered from inside a screen-loop iteration, which then resumes (draw, flip, pacer):
  // 0x6544 when the Music row changes the track, and the high-score footer frozen on a held Esc.
  return flow_phase_ == FlowPhase::kMusicFreeze || esc_hold_ticks_ > 0;
}

bool MilestoneADemo::ConsumePacerResetRequest() {
  // Deliver only once the dispatcher has returned: the original rebases right after 0x3830 returns,
  // i.e. after the palette fade-in 0x6498, just before the first resumed gameplay step (0x4db-0x4ea).
  if (!pacer_reset_pending_ || flow_phase_ != FlowPhase::kNone) {
    return false;
  }
  pacer_reset_pending_ = false;
  return true;
}

std::string MilestoneADemo::StepStateFingerprint() const {
  // The logical step state Render() must not move (--check-render-purity draws every frame twice
  // and compares this before and after). Render caches (palette-level textures, the frozen-picture
  // capture) are left out on purpose.
  std::ostringstream stream;
  stream << "scene=" << static_cast<int>(scene_) << " view=" << static_cast<int>(frontend_view_)
         << " flow=" << static_cast<int>(flow_phase_) << " t0=" << flow_t0_ << " fstep=" << flow_step_
         << " dstate=" << dispatch_state_ << " exit=" << static_cast<int>(flow_exit_)
         << " row=" << frontend_selected_row_ << " mrow=" << main_menu_row_ << " pend=" << menu_pending_
         << " sexit=" << (screen_exit_ ? 1 : 0) << " pulse=" << pulse_phase_ << "," << entry_pulse_phase_
         << " title=" << title_level_ << " splash=" << splash_page_ << ":" << splash_frame_ << ":"
         << splash_stage_ << ":" << splash_level_ << " frame_counter=" << frontend_frame_counter_
         << " cycle=" << frontend_cycle_counter_ << " bank=" << frontend_current_bank_ << ">"
         << frontend_next_bank_ << " pointfield=" << frontend_pointfield_state_.origin_x << ","
         << frontend_pointfield_state_.origin_y << "," << frontend_pointfield_state_.origin_z << ","
         << frontend_pointfield_state_.angle_xy << "," << frontend_pointfield_state_.angle_z
         << " view_frame=" << frontend_view_frame_ << " credits=" << credits_page_ << ":" << credits_mode_
         << ":" << credits_counter_ << " hs=" << hs_mode_ << ":" << hs_slot_ << ":" << hs_stage_ << ":"
         << hs_len_ << ":" << hs_buffer_ << " freeze=" << music_freeze_left_ << " eschold=" << esc_hold_ticks_
         << " s9=" << (state9_pending_ ? 1 : 0) << " topout=" << top_out_frame_
         << " dissolved=" << top_out_dissolved_rows_ << " alert=" << alert_effect_id_ << ":" << alert_lifetime_
         << ":" << alert_reveal_counter_ << " hold=" << (alert_corner_hold_ ? 1 : 0)
         << " debris_hidden=" << (debris_hidden_after_frontend_ ? 1 : 0) << " last_clear=" << last_clear_tick_
         << " debris=" << debris_particles_.size() << " rng=" << gameplay_random_index_
         << " events=" << audio_events_.size() << " amounts=";
  for (const int amount : row_amount_) {
    stream << amount << ",";
  }
  if (!debris_particles_.empty()) {
    const DebrisParticle& first = debris_particles_.front();
    stream << " debris0=" << first.x << "," << first.y << "," << first.age;
  }
  return stream.str();
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
  if (alert_corner_texture_ != nullptr) {
    SDL_DestroyTexture(alert_corner_texture_);
    alert_corner_texture_ = nullptr;
  }
  alert_corner_dirty_ = true;

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

  // Absolute cells of the live piece (table 0x184E3 offsets + reference position), e.g. 6:0/7:0/6:1/7:1.
  std::string cells;
  for (const CellOffset& cell : RotatedCells(live_piece_.type, live_piece_.rotation)) {
    cells += (cells.empty() ? "" : "/") + std::to_string(live_piece_.x + cell.x) + ":" +
             std::to_string(live_piece_.y + cell.y);
  }

  std::ostringstream stream;
  stream << "Debug state: scene=" << scene_name()
         << " phase=" << phase
         << " piece=" << live_piece_.type << ":" << live_piece_.rotation
         << " pos=" << live_piece_.x << "," << live_piece_.y
         << " cells=" << cells
         << " next=" << live_next_piece_type_
         << " score=" << live_score_
         << " lines=" << live_lines_
         << " level=" << live_level_
         << " gravity=" << gravity_accumulator_
         << " lockout=" << down_lockout_frames_
         << " seed=" << piece_seed_
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
         << " sleepy=" << (alert_effect_id_ == 11 ? 1 : 0)
         << " snapshot=" << (gameplay_snapshot_texture_valid_ ? 1 : 0)
         << " alertlife=" << alert_lifetime_
         << " gameover=" << (scene_ == Scene::kTopOut && top_out_dissolved_rows_ >= kTopOutDissolveFrames ? 1 : 0)
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
         << " tlevel=" << CurrentTitleLevel();
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
  particle_sine_table_ = BuildParticleSineTable();

  // The face corner is a 50x50 piece of the working screen. 0x2188 saves the base screen
  // under it once at startup; 0x2138 restores it when a face expires.
  const AtetData::Indexed& base = atet_data_.gameplay_screen();
  if (base.width < kAlertFaceX + kAlertFaceSize || base.height < kAlertFaceY + kAlertFaceSize ||
      base.indices.size() < static_cast<size_t>(base.width) * static_cast<size_t>(base.height)) {
    SetErrorMessage(error_out, "Gameplay screen too small for the alert corner.");
    return false;
  }
  for (int row = 0; row < kAlertFaceSize; ++row) {
    for (int col = 0; col < kAlertFaceSize; ++col) {
      alert_corner_background_[static_cast<size_t>(row * kAlertFaceSize + col)] =
          base.indices[static_cast<size_t>((kAlertFaceY + row) * base.width + kAlertFaceX + col)];
    }
  }
  for (int tile = 0; tile < static_cast<int>(alert_tiles_.size()); ++tile) {
    const AtetData::Indexed& face = atet_data_.alert_tile(tile);
    if (face.width != kAlertFaceSize || face.height != kAlertFaceSize ||
        face.indices.size() != static_cast<size_t>(kAlertFaceSize * kAlertFaceSize)) {
      SetErrorMessage(error_out, "ATET.DAT chunk 6 alert tile is not 50x50.");
      return false;
    }
  }
  AlertCornerRestore();
  return true;
}

void MilestoneADemo::ResolveSetupDatPath(const std::string& data_dir) {
  if (!setup_dat_override_.empty()) {
    setup_dat_path_ = setup_dat_override_;
    SDL_Log("SETUP.DAT: %s (from --setup-dat)", setup_dat_path_.c_str());
    return;
  }

  // The original opens the bare name "setup.dat" (load 0x36dc, save 0x3604), relative
  // to the working directory where it also opens "atet.dat", so the two files always
  // sit in one folder. Keep that: SETUP.DAT lives beside the data file actually
  // loaded, which is the executable's folder in the release layout.
  const std::string primary = AbsoluteDir(data_dir) + "SETUP.DAT";
  if (IsWritableFile(primary)) {
    setup_dat_path_ = primary;
    SDL_Log("SETUP.DAT: %s", setup_dat_path_.c_str());
    return;
  }

  // Read-only install (Program Files, read-only media) or something in the way:
  // keep the settings in the per-user data folder instead and say so.
  if (char* pref = SDL_GetPrefPath("", "saba-reborn")) {
    setup_dat_path_ = std::string(pref) + "SETUP.DAT";
    SDL_free(pref);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "SETUP.DAT: %s is not writable; using the per-user copy %s",
                primary.c_str(), setup_dat_path_.c_str());
    return;
  }

  setup_dat_path_ = primary;
  SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
              "SETUP.DAT: %s is not writable and no per-user folder is available (%s); settings will not be saved",
              primary.c_str(), SDL_GetError());
}

bool MilestoneADemo::LoadSetupDat() {
  std::vector<Uint8> bytes;
  std::string ignored_error;
  if (setup_dat_path_.empty() || !ReadBinaryAsset(setup_dat_path_.c_str(), &bytes, &ignored_error)) {
    return false;
  }

  // 0x36dc checks only that the file opens and that it starts with the 5-byte
  // 'AciD'+NUL signature, then reads the fields in order with no size check: a short
  // file leaves the later fields zero, a longer one is read up to byte 178. (This
  // loader used to reject every size but 178, after which Initialize() overwrote the
  // player's file with defaults.)
  if (bytes.size() < kSetupSignature.size()) {
    return false;
  }

  for (size_t index = 0; index < kSetupSignature.size(); ++index) {
    if (bytes[kSetupSignatureOffset + index] != kSetupSignature[index]) {
      return false;
    }
  }
  bytes.resize(kSetupDatSize, 0);

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

  if (setup_dat_path_.empty()) {
    return false;
  }
  // SDL_SaveFile takes a UTF-8 path on every platform (see ReadBinaryAsset).
  return SDL_SaveFile(setup_dat_path_.c_str(), bytes.data(), bytes.size());
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

void MilestoneADemo::RenderTopOut(SDL_Renderer* renderer) const {
  // The game-over scene is the working screen as the failed spawn left it, redrawn every
  // frame from state (all stepping happens in Tick): HUD, the well as drawn (board plus the
  // colliding piece when it was drawn) minus the scanlines 0x1EDC has already erased, the
  // face corner, GAME OVER once the walk is done (0x22FC at (85,83), colour 0 transparent),
  // and the debris plotted last (0x2F24 runs after every tick, over face and GAME OVER).
  DrawTexture(renderer, gameplay_screen_.texture, gameplay_screen_.width, gameplay_screen_.height);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
  const SDL_FRect preview_clear = {static_cast<float>(kPreviewX), static_cast<float>(kPreviewY),
                                   static_cast<float>(kPreviewWidth), static_cast<float>(kPreviewHeight)};
  SDL_RenderFillRect(renderer, &preview_clear);
  const SDL_FRect well_clear = {static_cast<float>(kGameplayClearX), static_cast<float>(kGameplayClearY),
                                static_cast<float>(kGameplayClearWidth), static_cast<float>(kGameplayClearHeight)};
  SDL_RenderFillRect(renderer, &well_clear);
  if (live_game_active_) {
    RenderLiveHud(renderer);
  }
  DrawBoardCells(renderer, top_out_source_cells_, false);

  const int erased = std::clamp(top_out_dissolved_rows_, 0, kTopOutDissolveFrames);
  if (erased > 0) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    const SDL_FRect erased_rect = {static_cast<float>(kGameplayClearX), static_cast<float>(kGameplayClearY),
                                   static_cast<float>(kGameplayClearWidth), static_cast<float>(erased)};
    SDL_RenderFillRect(renderer, &erased_rect);
  }

  RenderAlertTile(renderer);
  if (top_out_dissolved_rows_ >= kTopOutDissolveFrames) {
    DrawTextureAt(renderer, game_over_overlay_.texture, kGameOverX, kGameOverY, game_over_overlay_.width,
                  game_over_overlay_.height);
  }
  if (!debris_hidden_after_frontend_) {
    RenderDebrisParticles(renderer);
  }
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
    if (glyph_code == 0x42 && (Tick32() & 0x10U) != 0) {
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
      return static_cast<int>(Tick32() - flow_t0_);
    default:
      return 0;
  }
}

int MilestoneADemo::CurrentFrozenLevel() const {
  const int e = std::clamp(static_cast<int>(Tick32() - flow_t0_), 0, kFadeTicks);
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
  const auto render_into = [&](std::vector<Uint8>& dst) {
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
                    dst.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(y) * kScreenWidth * 4));
        }
        SDL_DestroySurface(rgba);
      }
      SDL_DestroySurface(raw);
    }
  };
  if (SDL_SetRenderTarget(renderer, target)) {
    // The entry fade 0x64f0 ramps the displayed page, debris included.
    render_into(frozen_rgba_);
    if (entry) {
      // [0x2c69f], restored by the exit tail, is the working screen saved at 0x3835: it never holds
      // particles (0x17875 restored them at the top of the frame, 0x2F24 plots after the ticks), so
      // the exit fade-in 0x6498 shows a particle-free screen (FX-23).
      saved_screen_rgba_ = frozen_rgba_;
      const bool hidden = debris_hidden_after_frontend_;
      debris_hidden_after_frontend_ = true;
      render_into(saved_screen_rgba_);
      debris_hidden_after_frontend_ = hidden;
    }
    SDL_SetRenderTarget(renderer, previous_target);
  } else if (entry) {
    saved_screen_rgba_ = frozen_rgba_;
  }
  SDL_DestroyTexture(target);
}

void MilestoneADemo::RenderTopOutFrozen(SDL_Renderer* renderer) {
  // The game-over picture as the screen shows it (RenderTopOut draws the scene from state and
  // advances nothing): the dissolve so far, GAME OVER once it is up, and the debris.
  RenderTopOut(renderer);
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
  flow_t0_ = Tick32();
}

void MilestoneADemo::TickSplash() {
  const int e = static_cast<int>(Tick32() - flow_t0_);
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
  flow_t0_ = Tick32();
  flow_step_ = 0;  // 1 once 0x6544 has been called (the boot music request)
}

void MilestoneADemo::TickBootGap() {
  const int e = static_cast<int>(Tick32() - flow_t0_);
  if (e >= kBootLoadAllowanceTicks && flow_step_ == 0) {
    // main calls 0x6544 after the chunk loads. The request goes out here: the music player runs
    // 0x6544's 0x20-tick fade-out (nothing plays yet) and then loads and starts the module (audio
    // MU-09), while this phase keeps the screen black for the same 32 ticks.
    flow_step_ = 1;
    StartCurrentMusic("music-start-after-startup");
  }
  if (e < kBootLoadAllowanceTicks + kMusicChangeWaitTicks) {
    return;
  }
  // 0x6544 returns with the module started and fading in: 0x3830(1).
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
  flow_t0_ = Tick32();
  if (!fades_enabled_) {
    title_level_ = kFrontendRestLevel;
    StartScreen(state);
    TickReveal();
    return;
  }
  flow_phase_ = FlowPhase::kEntryPaletteOut;
}

void MilestoneADemo::TickEntryPaletteOut() {
  const int e = static_cast<int>(Tick32() - flow_t0_);
  if (e < kFadeTicks) {
    return;  // level 64 - e, read by Render; no game logic, no pointfield
  }
  // Last upload (e = 63) done: the title copy and 0x63b8 start in this tick.
  if (dispatch_state_ == 9 && frozen_source_ == FrozenSource::kTopOut) {
    // effects-alerts FX-VP-02: at black after the GAME OVER fade the lost game is over for good
    // (0x1FF5 cleared the live flag in the GAME OVER tick); nothing can re-enter it.
    live_game_active_ = false;
    live_game_available_ = false;
  }
  flow_phase_ = FlowPhase::kEntryTitleIn;
  flow_t0_ = Tick32();
  title_level_ = 0;
  TickEntryTitleIn();
}

void MilestoneADemo::TickEntryTitleIn() {
  const int e = static_cast<int>(Tick32() - flow_t0_);
  if (e >= kFadeTicks) {
    // 0x63b8 leaves after the frame with e = 62: the title palette stays at 62/64.
    StartScreen(dispatch_state_);
    TickReveal();
    return;
  }
  title_level_ = e;
  // 0x63b8 does not re-base the pacer, whose base predates the 63-tick busy wait, so the fade's
  // second frame runs the capped 6 catch-up steps (the main loop's pacer): one pointfield step each.
  StepFrontendPointfield();
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
  flow_t0_ = Tick32();
  row_amount_.fill(0);
  TickExitTitleOut();
}

void MilestoneADemo::TickExitTitleOut() {
  const int e = static_cast<int>(Tick32() - flow_t0_);
  if (e >= kFadeTicks) {
    // 0x3604 saves SETUP.DAT, the saved gameplay picture is restored (New Game
    // also runs its init, 0x2d88 / 0x5e0), then 0x6498 starts in this tick.
    PersistSetupState();
    setup_dirty_ = false;
    frozen_capture_pending_ = true;
    switch (flow_exit_) {
      case FlowExit::kNewGame:
        // effects-alerts AL-VO-01: New Game init (0x05E0 -> 0x206C(1)) resets the face only in the
        // working screen, so the corner as it was left stays on screen until the first tick.
        alert_corner_shown_ = alert_corner_;
        alert_corner_hold_ = true;
        ResetLiveGameplayState();
        live_game_active_ = true;
        live_game_available_ = true;
        StartCurrentMusic("music-continue-new-game");
        frozen_source_ = FrozenSource::kGameplay;
        UseSavedScreenForNewGame();
        break;
      case FlowExit::kResume:
        // The original restores the saved screen; nothing in the frontend changes the
        // parked game, so rendering the restored game gives the same picture (the game-over
        // scene's picture after an Esc during the dissolve).
        RestoreLiveSnapshot();
        live_game_active_ = true;
        StartCurrentMusic("music-continue-return-to-game");
        frozen_source_ = gameplay_phase_ == GameplayPhase::kTopOutDissolve ? FrozenSource::kTopOut
                                                                          : FrozenSource::kGameplay;
        break;
      case FlowExit::kSplashes:
      case FlowExit::kNone:
        frozen_source_ = FrozenSource::kBlack;
        break;
    }
    if (flow_exit_ == FlowExit::kNewGame || flow_exit_ == FlowExit::kResume) {
      // effects-alerts FX-23: the exit tail puts the saved working screen back on the pages before
      // 0x6498 (0x395a-0x39ae) and that screen holds no particles, so the fade-in shows none; they
      // show again once the first tick back in the game has stepped them (the flag is cleared in
      // StepLiveGameplay / StepTopOutScene). Set after New Game's ResetLiveGameplayState.
      debris_hidden_after_frontend_ = true;
    }
    frozen_texture_level_ = -1;
    flow_phase_ = FlowPhase::kExitPaletteIn;
    flow_t0_ = Tick32();
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
  const int e = static_cast<int>(Tick32() - flow_t0_);
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
        alert_corner_shown_ = alert_corner_;
        alert_corner_hold_ = true;
        ResetLiveGameplayState();
        live_game_active_ = true;
        live_game_available_ = true;
        StartCurrentMusic("music-continue-new-game");
      }
      scene_ = Scene::kGameplay;
      // effects-alerts FX-23: the restored screen holds no particles; they show again after the
      // first tick back in the game has stepped them.
      debris_hidden_after_frontend_ = true;
      // The first game after boot enters the session loop straight from main (0x483 -> 0x488) with
      // no pacer rebase, so its second frame runs 6 steps; every later New Game comes back through
      // the session loop's own 0x3830 call and the rebase at 0x4db-0x4ea.
      pacer_reset_pending_ = frontend_new_games_ > 0;
      ++frontend_new_games_;
      break;
    case FlowExit::kResume:
      if (!fades_enabled_) {
        RestoreLiveSnapshot();
        live_game_active_ = true;
        StartCurrentMusic("music-continue-return-to-game");
      }
      // effects-alerts FX-40: Return to Game after an Esc during the top-out dissolve resumes it.
      scene_ = gameplay_phase_ == GameplayPhase::kTopOutDissolve ? Scene::kTopOut : Scene::kGameplay;
      debris_hidden_after_frontend_ = true;
      pacer_reset_pending_ = true;  // Return to Game: rebase, exactly one step (0x4db-0x4ea)
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
  flow_t0_ = Tick32();
  row_amount_.fill(0);
  TickQuitTitleOut();
}

void MilestoneADemo::TickQuitTitleOut() {
  const int e = static_cast<int>(Tick32() - flow_t0_);
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
  // The hold ends: the footer iteration it froze continues, and the main loop's pacer then returns
  // min(ticks since its last call, 6) for the next frame (InNestedBlockingPhase).
  esc_hold_ticks_ = 0;
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
        "Failed to save SETUP.DAT state: %s (%s)",
        setup_dat_path_.c_str(),
        SDL_GetError());
    return;
  }
  // The original writes SETUP.DAT only at the frontend exits (0x3604 from the
  // dispatcher's state 3 and exit tail) and when it creates the defaults; the log
  // line lets the suites check that an edit alone does not save (IN-14, HS-26).
  if (debug_state_logging_) {
    SDL_Log("Setup: SETUP.DAT saved at tick %u", static_cast<unsigned>(Tick32()));
  }
}

void MilestoneADemo::EmitAudioEvent(const char* event_name, int slot, int pan, int voice_class, int sfx_volume_override) {
  if (event_name == nullptr) {
    return;
  }
  if (voice_class < 0) {
    // 0x6817 plays on voice (module channels + EDX); EDX is 0 at the lock (0x0D4E) and the menu
    // blips (0x4445, 0x447C, 0x487F, 0x48BA, 0x4C9E, 0x4CD7) and 1 at every other call site.
    voice_class = (slot == 0 || slot == 1) ? 0 : 1;
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
  // Lock-sound pan, 0x0D23-0x0D4E: ecx = (x - 5) * 21 + 0x80 with x the piece's reference x
  // (the origin of its 4x4 box in the authored table 0x184E3, [0x2C737]); the mixer keeps the low
  // 8 bits (0xB276), so x = -2 (an O or vertical I/S/Z at the left wall) pans hard right (237).
  // The reference x is the piece's leftmost board column minus the authored left margin of its
  // shape; the margin depends only on the shape (derived from 0x184E3: 0 for a horizontal I, 1 for
  // three-wide states and the J/L/T states that point left, 2 for the rest).
  struct ShapeMargin {
    std::uint16_t mask;  // bit (dy * 4 + dx) of the cells shifted to their top-left
    int margin;
  };
  static constexpr std::array<ShapeMargin, 19> kAuthoredLeftMargins = {{
      {0x000F, 0}, {0x0017, 1}, {0x0027, 1}, {0x0033, 2}, {0x0036, 1}, {0x0047, 1}, {0x0063, 1},
      {0x0071, 1}, {0x0072, 1}, {0x0074, 1}, {0x0113, 2}, {0x0131, 2}, {0x0132, 2}, {0x0223, 1},
      {0x0231, 2}, {0x0232, 1}, {0x0311, 2}, {0x0322, 1}, {0x1111, 2},
  }};
  const std::array<CellOffset, 4> cells = RotatedCells(live_piece_.type, live_piece_.rotation);
  int min_x = cells[0].x;
  int min_y = cells[0].y;
  for (const CellOffset& cell : cells) {
    min_x = std::min(min_x, cell.x);
    min_y = std::min(min_y, cell.y);
  }
  std::uint16_t mask = 0;
  for (const CellOffset& cell : cells) {
    const int bit = (cell.y - min_y) * 4 + (cell.x - min_x);
    if (bit >= 0 && bit < 16) {
      mask = static_cast<std::uint16_t>(mask | (1u << bit));
    }
  }
  int margin = 0;
  for (const ShapeMargin& entry : kAuthoredLeftMargins) {
    if (entry.mask == mask) {
      margin = entry.margin;
      break;
    }
  }
  const int reference_x = piece_x + min_x - margin;
  return ((reference_x - 5) * 21 + 0x80) & 0xFF;
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
  ArmTopOut(score, lines, /*piece_drawn=*/false, /*announce=*/true);
}

void MilestoneADemo::ArmTopOut(int score, int lines, bool piece_drawn, bool announce) {
  // Failed spawn (0x102D..0x1065): face 6 for ever through the shared trigger 0x2008,
  // 0x1F8C(2) (scanline counter 0, logical board cleared), SFX 5 (centre, class 1). The
  // dissolve samples the working screen, so its source is the board plus the colliding
  // piece when that was drawn (0x10A1 runs only when no collapse is pending). A spawn that
  // failed in a clearing lock step played the face and the sound there (announce = false)
  // and arms the scene only when gameplay-core's collapse of the frozen stack has run.
  top_out_frame_ = 0;
  top_out_dissolved_rows_ = 0;
  top_out_score_ = score;
  top_out_lines_ = lines;
  top_out_source_cells_ = live_board_cells_;
  if (piece_drawn) {
    for (const CellOffset& cell : RotatedCells(live_piece_.type, live_piece_.rotation)) {
      const int board_x = live_piece_.x + cell.x;
      const int board_y = live_piece_.y + cell.y;
      if (board_x >= 0 && board_x < kBoardWidth && board_y >= 0 && board_y < kBoardHeight) {
        top_out_source_cells_[static_cast<size_t>(board_y * kBoardWidth + board_x)] =
            NormalizeModulo(live_piece_.type, kPieceCount) + 1;
      }
    }
  }
  if (announce) {
    TriggerAlert(6, kAlertInfiniteLifetime);
  }
  live_board_cells_.fill(0);
  gameplay_phase_ = GameplayPhase::kTopOutDissolve;
  if (announce) {
    EmitAudioEvent("top-out", 5, 0x80, 1);
  }
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

bool MilestoneADemo::CanPlaceLivePiece(const LivePiece& piece) const {
  // 0x12AC(x, y, rot, id): a cell blocks when its column is outside 0..9, when its row is >= 20, or
  // when its row is >= 0 and the board cell is set. Cells above the well (row < 0) are legal.
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
  // 0x5E0 (0x5FC): the running level starts at the menu's start level; after that only a ten-line
  // crossing changes it (0xEA7).
  live_level_ = start_level_;
  gameplay_phase_ = GameplayPhase::kFalling;
  gravity_accumulator_ = 0;
  // 0x9C8(1) called at 0x707: the four repeat timers and the Down lockout start at 0.
  repeat_timers_.fill(0);
  down_lockout_frames_ = 0;
  pending_clear_rows_.clear();
  collapse_frame_ = 0;
  collapse_step_ = 0;
  collapse_batch_ = 0;
  live_piece_hidden_ = false;
  top_out_after_collapse_ = false;
  top_out_frame_ = 0;
  top_out_dissolved_rows_ = 0;
  top_out_source_cells_.fill(0);
  warning_band_ = 0;
  // The particle pool is NOT reset: 0x2DA0 runs once at startup, so debris alive when the
  // player left for the menu keeps flying in the new game (it froze while the menu ran).
  live_snapshot_ = LiveSnapshot{};
  ResetAlertState();  // 0x206C(1) from New Game init (0x0757): face, last-clear tick, b2b, cooldowns
  // 0x5E0 -> 0x32B0 at 0x6F8: srand([0x2D2A3]), the tick counter, then refill the 8192-entry table,
  // so every New Game deals a new sequence. --piece-seed pins the seed for tests and comparisons.
  piece_seed_ = has_piece_seed_override_ ? piece_seed_override_ : static_cast<std::uint32_t>(tick_count_);
  RefillGameplayRandomTable();
  // 0x70C: next = draw % 7; then 0x1348 promotes it (current = next, next = draw % 7, counted).
  live_next_piece_type_ = RollNextPieceType();
  live_piece_ = LivePiece{0, 0, kSpawnX, 0};
  line_clear_demo_count_ = 0;
  SpawnNextLivePiece();
}

bool MilestoneADemo::SpawnNextLivePiece() {
  // 0x1348: current = next, next = rng % 7, statistics[current] += 1, x=4 y=0 rotation 0,
  // accumulator 0. It never writes the Down lockout 0x2C783.
  live_piece_ = LivePiece{live_next_piece_type_, 0, kSpawnX, 0};
  live_next_piece_type_ = RollNextPieceType();
  gravity_accumulator_ = 0;
  ++live_piece_stats_[static_cast<size_t>(live_piece_.type)];
  // 0x102D: the spawn is tested in the same step on the board as it is now; after a clearing lock
  // that board has its full rows emptied but not yet compacted.
  if (!CanPlaceLivePiece(live_piece_)) {
    if (gameplay_phase_ == GameplayPhase::kCollapse) {
      // 0x9C8 runs a pending collapse (0xA20) before it looks at the top-out (0xA3A), so the frozen
      // stack is collapsed for 8 steps per row first and the dissolve starts after it. The rest of
      // the top-out (0x1037..0x1065) happens now: the top-out face (alert 6, lifetime -1) replaces
      // the clear's reward face and the top-out sound (slot 5, centre, class 1) plays in this step.
      top_out_after_collapse_ = true;
      TriggerAlert(6, kAlertInfiniteLifetime);
      EmitAudioEvent("top-out", 5, 0x80, 1);
    } else {
      // The colliding piece is drawn (0x10A1 runs when no collapse is pending) and the dissolve
      // samples it (effects-alerts FX-20).
      ArmTopOut(live_score_, live_lines_, /*piece_drawn=*/true, /*announce=*/true);
    }
    return false;
  }
  return true;
}

void MilestoneADemo::CommitLivePieceToBoard(int lock_y) {
  // 0x9C8 0xD53..0xD8E: the cells go to row lock_y + dy; rows < 0 are dropped. (The original's logical
  // board stores 1 and keeps the colours in the screen image; the port stores id + 1 so it can redraw.)
  for (const CellOffset& cell : RotatedCells(live_piece_.type, live_piece_.rotation)) {
    const int board_x = live_piece_.x + cell.x;
    const int board_y = lock_y + cell.y;
    if (board_x < 0 || board_x >= kBoardWidth || board_y < 0 || board_y >= kBoardHeight) {
      continue;
    }
    live_board_cells_[static_cast<size_t>(board_y * kBoardWidth + board_x)] = NormalizeModulo(live_piece_.type, kPieceCount) + 1;
  }
}

void MilestoneADemo::StepLiveGameplay() {
  if (!live_game_active_) {
    return;
  }

  // One session-loop tick: particles 0x2E18, gameplay 0x9C8, alert face 0x206C.
  StepDebrisParticles();
  debris_hidden_after_frontend_ = false;
  if (alert_corner_hold_) {
    alert_corner_hold_ = false;  // the first tick's flush shows the working-screen corner
    alert_corner_dirty_ = true;
  }

  switch (gameplay_phase_) {
    case GameplayPhase::kFalling:
      StepFallingGameplay();
      break;
    case GameplayPhase::kCollapse:
      StepCollapseGameplay();
      break;
    case GameplayPhase::kTopOutDissolve:
      break;  // the game-over scene steps itself (StepTopOutScene)
  }

  UpdateAlert();
}

bool MilestoneADemo::GameplayKeyHeld(int binding) const {
  // The ISR-maintained held table, indexed by the SETUP.DAT binding (0 Down, 1 Left, 2 Right,
  // 3 A = rotation +1, 4 S = rotation -1).
  const std::uint8_t scan_code = keyboard_bindings_[static_cast<size_t>(binding)];
  return scan_code != 0 && held_original_scancodes_[scan_code];
}

void MilestoneADemo::StepGameplayInput() {
  // 0x9C8 0xA97..0xAF3: on every live step decrement the four repeat timers and the Down lockout.
  for (int& timer : repeat_timers_) {
    if (timer > 0) {
      --timer;
    }
  }
  if (down_lockout_frames_ > 0) {
    --down_lockout_frames_;
  }
  // 0xAF9..0xC94, in this order: Left (x-1), Right (x+1), A (rotation+1), S (rotation-1). A key acts
  // when held with its timer at 0 and then reloads 8 whether or not the target fitted; a released
  // key's timer is 0, so presses act at once and a held key repeats every 8 steps. Only the target
  // is tested (no kicks) and the gravity accumulator is never touched.
  static constexpr int kDeltaX[4] = {-1, 1, 0, 0};
  static constexpr int kDeltaRotation[4] = {0, 0, 1, -1};
  for (int action = 0; action < 4; ++action) {
    const bool held = GameplayKeyHeld(action + 1);
    int& timer = repeat_timers_[static_cast<size_t>(action)];
    if (held && timer == 0) {
      LivePiece moved = live_piece_;
      moved.x += kDeltaX[action];
      moved.rotation = (moved.rotation + kDeltaRotation[action]) & 3;
      if (CanPlaceLivePiece(moved)) {
        live_piece_ = moved;
      }
      timer = kGameplayBindingRepeatFrames;
    }
    if (!held) {
      timer = 0;
    }
  }
}

void MilestoneADemo::StepFallingGameplay() {
  StackWarningStep();  // 0x21C4 at 0x0A5E: start of every normal step, before the input block
  // A piece promoted by a clearing lock stayed hidden through the collapse; from its first live step
  // on 0x9C8 erases, moves and redraws it.
  live_piece_hidden_ = false;
  StepGameplayInput();

  // 0xC94..0xD03: Down held with the lockout at 0 replaces the increment by 0x8000, Down up clears the
  // lockout, the increment is clamped to 0x10000, and on reaching 0x10000 the accumulator is SET TO 0
  // (no remainder is kept) and the piece is tried one row down.
  const int increment = CurrentDropIncrement();
  if (!GameplayKeyHeld(0)) {
    down_lockout_frames_ = 0;
  }
  audit_gravity_active_ = true;
  audit_gravity_rate_ = increment;
  audit_gravity_acc_before_ = gravity_accumulator_;
  gravity_accumulator_ += increment;
  LivePiece target = live_piece_;
  if (gravity_accumulator_ >= kGameplayGravityThreshold) {
    gravity_accumulator_ = 0;
    ++target.y;
    ++audit_descent_attempts_;
  }

  // 0xD09..0xD1D: the collision test runs at that position on every live step, not only when a row is
  // due, and there is no grounded timer: a resting piece locks on its next gravity row (one period
  // after landing, 2 steps under soft drop) whatever the player does meanwhile.
  if (CanPlaceLivePiece(target)) {
    if (target.y != live_piece_.y) {
      ++audit_descent_successes_;
    }
    live_piece_ = target;
    CheckSleepyTimeout();
    return;
  }
  LockLivePiece(target.y - 1);
  CheckSleepyTimeout();  // also on lock steps, the clearing and the top-out ones (0x1065 -> 0x10A6)
}

void MilestoneADemo::LockLivePiece(int lock_y) {
  // Piece lock / board impact -> slot 0, panned by the reference column and played at half the SFX
  // volume (0x6817 caller at 0x0D4E: pan = (x-5)*21+0x80 with the reference x, volume [0x2C6E7]/2).
  EmitAudioEvent("piece-lock", 0, PanForPieceColumn(live_piece_.x), 0, sfx_volume_ / 2);
  // 0xD60 dec ebp: the cells are written one row above the position that collided.
  CommitLivePieceToBoard(lock_y);
  const std::vector<int> full_rows = FullRowsAroundLock(lock_y);
  if (!full_rows.empty()) {
    // 0xE4E..0x100C, all in this step: collapse init, Down lockout, level, reward, lines, score.
    BeginLineCollapse(full_rows);
    down_lockout_frames_ = kDownLockoutAfterClear;
    ApplyLineClearScore(static_cast<int>(full_rows.size()));
  }
  // A non-clearing lock leaves the back-to-back memory alone: 0x2C763 is written only by a clear
  // (0x0FB9) and by New Game (0x20AF) (effects-alerts AL-09 = gameplay-core LC-06).
  // 0x1011: the next piece is promoted, counted and tested in the lock step (0x107C keeps it undrawn
  // while rows are pending).
  SpawnNextLivePiece();
}

void MilestoneADemo::StepCollapseGameplay() {
  // 0x9C8 0xA20..0xA39: while rows are pending a step only calls 0x1D04(0): no input, gravity, repeat
  // timers or Down lockout. Each call slides the stack above the current row down one pixel
  // (RenderGameplay draws that from collapse_step_); the 8th call compacts the row in the board, adds
  // one to the rows still queued (all above it) and starts the next. The promoted piece's first live
  // step is the one after the last call: lock step + 8 * rows + 1.
  ++collapse_frame_;
  ++collapse_step_;
  if (collapse_step_ == kCollapseStepsPerRow) {
    collapse_step_ = 0;
    if (!top_out_after_collapse_) {
      CompactClearedRow(pending_clear_rows_[static_cast<size_t>(collapse_batch_)]);
      for (size_t later = static_cast<size_t>(collapse_batch_) + 1; later < pending_clear_rows_.size(); ++later) {
        ++pending_clear_rows_[later];
      }
    }
    ++collapse_batch_;
  }
  if (collapse_batch_ < static_cast<int>(pending_clear_rows_.size())) {
    return;  // collapse steps return before the warning (0x21C4) and sleepy (0x10A6) code
  }

  pending_clear_rows_.clear();
  collapse_batch_ = 0;
  collapse_frame_ = 0;
  gameplay_phase_ = GameplayPhase::kFalling;
  if (top_out_after_collapse_) {
    // The spawn failed in the clearing step: the original wiped its board there (0x1F8C(2)), so its
    // collapse anchor sat on the empty-board line 152 and each call blanked one scanline of the bottom
    // row (0x1E34 skips the copy) while the stack image stayed put. Now the dissolve starts.
    top_out_after_collapse_ = false;
    std::fill_n(live_board_cells_.begin() + (kBoardHeight - 1) * kBoardWidth, kBoardWidth, 0);
    // Face and sound came in the lock step; the dissolve samples the frozen stack as drawn now.
    ArmTopOut(live_score_, live_lines_, /*piece_drawn=*/false, /*announce=*/false);
    return;
  }
}

// End of every normal step (0x10A6): the sleepy face when exactly 1800 ticks have passed
// since the last clear (or New Game). A pause straddling that tick skips it for good.
void MilestoneADemo::CheckSleepyTimeout() {
  if (Tick32() - last_clear_tick_ == kSleepyTicksSinceClear) {
    TriggerAlert(11, kAlertSleepyLifetime);
    EmitAudioEvent("sleepy-no-clear-timeout", 6, 0x80, 1);
  }
}

int MilestoneADemo::CurrentDropIncrement() const {
  // 0xCAB..0xCE5: the level's increment [0x2C717] = (level<<9)+0x200, replaced by 0x8000 while Down is
  // held and the Down lockout is 0, clamped to the 0x10000 threshold.
  const bool soft_drop = GameplayKeyHeld(0) && down_lockout_frames_ == 0;
  const int increment = soft_drop ? kSoftDropIncrement : CurrentGravityRate();
  return std::min(increment, kGameplayGravityThreshold);
}

void MilestoneADemo::BeginLineCollapse(const std::vector<int>& rows) {
  // 0xE08..0xE32, per full row bottom-up: 0x1414 turns the row's pixels into particles (level%10
  // helper, pre-clear level), then the row is zeroed in the board (not compacted) and appended to the
  // clear list. The particle seeding reads the cells, so it runs before they are zeroed.
  SeedLineClearDebris(rows);
  for (const int row : rows) {
    std::fill_n(live_board_cells_.begin() + row * kBoardWidth, kBoardWidth, 0);
  }
  pending_clear_rows_ = rows;
  // 0x1D04(1): collapse step and batch start at 0.
  collapse_frame_ = 0;
  collapse_step_ = 0;
  collapse_batch_ = 0;
  gameplay_phase_ = GameplayPhase::kCollapse;
  // 0x107C: no live piece is drawn while rows are pending.
  live_piece_hidden_ = true;
  const int clear_count = std::clamp(static_cast<int>(rows.size()), 1, 4);
  // Clear block of the lock step (0x0EB2..0x0FA9): one reward face and ONE sound (centre,
  // class 1). Wake needs the sleepy face to be the current face; back-to-back needs the
  // previous clear to have been a tetris; otherwise tables 0x980 / 0x990.
  static constexpr std::array<int, 4> kClearFace = {2, 0, 9, 7};
  static constexpr std::array<int, 4> kClearSlot = {9, 7, 7, 10};
  static constexpr std::array<const char*, 4> kClearName = {"line-clear-1", "line-clear-2-or-3", "line-clear-2-or-3",
                                                            "line-clear-4"};
  int face = kClearFace[static_cast<size_t>(clear_count - 1)];
  int slot = kClearSlot[static_cast<size_t>(clear_count - 1)];
  const char* name = kClearName[static_cast<size_t>(clear_count - 1)];
  if (clear_count == 1 && alert_effect_id_ == 11) {
    face = 12;
    slot = 8;
    name = "sleepy-wake-1-line";
  } else if (clear_count == 4 && last_clear_was_four_) {
    face = 8;
    slot = 11;
    name = "line-clear-4-back-to-back";
  }
  TriggerAlert(face, kAlertClearLifetime);
  EmitAudioEvent(name, slot, 0x80, 1);
  // Then, silently, the fast-follow face when the previous clear (or New Game) came less
  // than 160 ticks ago (0x0F54..0x0F9F): 1 for a single, 10 for a double or triple.
  if (Tick32() - last_clear_tick_ < kFastFollowTicks) {
    if (clear_count == 1) {
      TriggerAlert(1, kAlertClearLifetime);
    } else if (clear_count < 4) {
      TriggerAlert(10, kAlertClearLifetime);
    }
  }
  last_clear_tick_ = Tick32();
  last_clear_was_four_ = clear_count == 4;
}

std::vector<int> MilestoneADemo::FullRowsAroundLock(int lock_y) const {
  // 0xDB8..0xE3F: rows lock_y+1 down to lock_y-2 (every row the piece can occupy), bottom-up; the clear
  // list keeps that order, which is also the collapse order.
  std::vector<int> rows;
  for (int row = lock_y + 1; row >= lock_y - 2; --row) {
    if (row < 0 || row >= kBoardHeight) {
      continue;
    }
    bool full = true;
    for (int x = 0; x < kBoardWidth; ++x) {
      if (live_board_cells_[static_cast<size_t>(row * kBoardWidth + x)] == 0) {
        full = false;
        break;
      }
    }
    if (full) {
      rows.push_back(row);
    }
  }
  return rows;
}

std::vector<int> MilestoneADemo::DetectFullRows() const {
  // Row-injection helper only (the lock scans four rows, FullRowsAroundLock); bottom-up like the lock.
  std::vector<int> rows;
  for (int y = kBoardHeight - 1; y >= 0; --y) {
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
  // 0xE58..0xEA7 then 0xFAE..0xFEC: the level goes up by one when lines%10 + n >= 10 (gravity follows,
  // +0x200 per level via CurrentGravityRate), then lines += n, then the score adds (level+1) *
  // {100,300,600,1200}[n-1] with the level already raised. The start level is never re-read here.
  static constexpr std::array<int, 5> kBaseClearScores = {0, 100, 300, 600, 1200};
  clear_count = std::clamp(clear_count, 1, 4);
  if (live_lines_ % 10 + clear_count >= 10) {
    ++live_level_;
  }
  live_lines_ += clear_count;
  live_score_ += (live_level_ + 1) * kBaseClearScores[static_cast<size_t>(clear_count)];
}

void MilestoneADemo::CompactClearedRow(int row) {
  // 0x1D6D..0x1DBA, the 8th collapse call of a row: rows 0..row-1 move down one row, row 0 is zeroed.
  for (int y = row; y > 0; --y) {
    std::copy_n(live_board_cells_.begin() + (y - 1) * kBoardWidth, kBoardWidth, live_board_cells_.begin() + y * kBoardWidth);
  }
  std::fill_n(live_board_cells_.begin(), kBoardWidth, 0);
}

void MilestoneADemo::SeedLineClearDebris(const std::vector<int>& rows) {
  AuditScopedTicks audit_ticks(&g_audit.line_clear_seed_ticks);
  const int style = NormalizeModulo(live_level_, 10);
  // Rows arrive bottom-up (the lock scans y+1 down to y-2, 0xDB8..0xE3F), so the lowest cleared
  // row draws the RNG first, as 0x1414 is called in that order.
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
  // Every helper spawns one particle per band pixel with no zero test (0x14B4..0x1C40);
  // the burst families (0/1/2) draw from the shared gameplay RNG first.
  switch (style) {
    case 0: {  // 0x14b4: random polar burst (3 draws: life, angle, speed)
      const int life = static_cast<int>(NextGameplayRandomU32() & 0x3f) + 0x10;
      const int angle = static_cast<int>(NextGameplayRandomU32() & 0x7ff);
      const int speed = static_cast<int>(NextGameplayRandomU32() & 0x1ff) + 0x80;
      SpawnDebrisPolar(screen_x, screen_y, speed, angle, life, color);
      break;
    }
    case 1: {  // 0x158c: horizontal spray, direction by band-row parity (2 draws: life, speed)
      const int life = static_cast<int>(NextGameplayRandomU32() & 0x3f) + 0x10;
      const int speed = static_cast<int>(NextGameplayRandomU32() & 0x1ff) + 0x80;
      SpawnDebrisPolar(screen_x, screen_y, speed, (row8 & 1) ? 0x400 : 0, life, color);
      break;
    }
    case 2: {  // 0x1694: column-swept fan (2 draws: life, speed)
      const int life = static_cast<int>(NextGameplayRandomU32() & 0x3f) + 0x10;
      const int speed = static_cast<int>(NextGameplayRandomU32() & 0x1ff) + 0x80;
      SpawnDebrisPolar(screen_x, screen_y, speed, 0x188 + col * 3, life, color);
      break;
    }
    case 3:  // 0x1770: deterministic fan
      SpawnDebrisPolar(screen_x, screen_y, 0x100, 0xc0 + col * 8, 0x40, color);
      break;
    case 4:  // 0x1830: target sweep; target Y starts at 8*row+0xf and gains 3 per band pixel
             // row (0x18F8 add ecx,3). screen_y = 21 + 8*row + row8.
      SpawnDebrisTarget(screen_x, screen_y, 0x1f + col * 3, screen_y - 6 + 2 * row8, 0x40, color);
      break;
    case 5:  // 0x192c: converge to a fixed point
      SpawnDebrisTarget(screen_x, screen_y, 0x95, 0xd5, 0x30, color);
      break;
    case 6:  // 0x19dc: fixed direction, column-ramped speed
      SpawnDebrisPolar(screen_x, screen_y, 0x214 + col * 4, 0x600, 0x30, color);
      break;
    case 7:  // 0x1a9c: converge to a vertical line (targetY = 8*row+0x18)
      SpawnDebrisTarget(screen_x, screen_y, 0x94, screen_y + 3 - row8, 0x30, color);
      break;
    case 8:  // 0x1b54: horizontal split at column 0x28; speed 0x80 gains 0x10 per band
             // pixel row (0x1C06 add esi,0x10)
      SpawnDebrisPolar(screen_x, screen_y, 0x80 + 0x10 * row8, col < 0x28 ? 0 : 0x400, 0x30, color);
      break;
    default:  // 9, 0x1c40: two-band split at column 0x28
      SpawnDebrisPolar(screen_x, screen_y, 0xa0, col < 0x28 ? 0x100 : 0x500, 0x30, color);
      break;
  }
}

void MilestoneADemo::SpawnDebrisPolar(int screen_x, int screen_y, int speed, int angle, int lifetime, int color) {
  if (debris_particles_.size() >= kGameplayMaxDebrisParticles) {
    ++audit_particles_dropped_;
    return;
  }
  ++audit_particles_spawned_;
  // Polar velocity (0x2f78): v = -((t[idx] * speed) >> 7) on the 0x27B8 table (peak
  // 32768). Position/velocity are 16.16.
  const int cos_index = (angle + 0x200) & 0x7FF;
  const int sin_index = angle & 0x7FF;
  const int vx = -static_cast<int>((static_cast<std::int64_t>(particle_sine_table_[cos_index]) * speed) >> 7);
  const int vy = -static_cast<int>((static_cast<std::int64_t>(particle_sine_table_[sin_index]) * speed) >> 7);
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
  // 0x2E18: age += decay and retire at 0x400 before moving; otherwise move, and retire a
  // particle whose new position is off screen (x < 0, y < 0, x > 320.0, y > 240.0; exactly
  // 320.0 or 240.0 survive; the new position is not stored). The survivors keep their order.
  for (DebrisParticle& particle : debris_particles_) {
    particle.age += particle.decay;
    if (particle.age >= kDebrisAgeFull) {
      continue;
    }
    const int next_x = particle.x + particle.vx;
    const int next_y = particle.y + particle.vy;
    if (next_x < 0 || next_y < 0 || next_x > (kScreenWidth << 16) || next_y > (kScreenHeight << 16)) {
      particle.age = kDebrisAgeFull;  // culled
      continue;
    }
    particle.x = next_x;
    particle.y = next_y;
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
  // 0x2F24 walks the list from its head, newest first (the spawners insert at the head),
  // and 0x17821 stores every colour, 0 (black) included; so the OLDEST particle on a pixel
  // is the one left visible. The container is in spawn order: draw it back to front.
  for (auto it = debris_particles_.rbegin(); it != debris_particles_.rend(); ++it) {
    const DebrisParticle& particle = *it;
    const int stage = std::clamp(particle.age >> 8, 0, 3);  // 4 stages of 0x100
    const Uint8 ramp_index = particle_ramps_[static_cast<size_t>(std::clamp(particle.color, 0, 255)) * 4 +
                                              static_cast<size_t>(stage)];
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

// One tick of the game-over scene (session loop 0x58E/0x595/0x59D): particles 0x2E18,
// then 0x9C8 -> 0x1F8C(0), which sprays and erases one scanline per tick until the 160th
// and then draws GAME OVER in that same tick (afterwards 0x9C8 returns at once), then the
// alert update 0x206C. Nothing here times out: only an Esc release leaves (HandleKeyUp).
void MilestoneADemo::StepTopOutScene() {
  AuditScopedTicks audit_ticks(&g_audit.topout_update_ticks);
  StepDebrisParticles();
  debris_hidden_after_frontend_ = false;
  if (top_out_dissolved_rows_ < kTopOutDissolveFrames) {
    SpawnTopOutPixelRow(top_out_dissolved_rows_);
    ++top_out_dissolved_rows_;
    if (top_out_dissolved_rows_ == kTopOutDissolveFrames) {
      live_game_available_ = false;  // 0x1FF5: the lost game can no longer be resumed
    }
  }
  ++top_out_frame_;
  UpdateAlert();
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
      // 0x1EDC samples the working screen: the drawn well (board + colliding piece).
      const int cell = top_out_source_cells_[static_cast<size_t>(board_row * kBoardWidth + board_col)];
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
  if (effect_id == alert_effect_id_ || alert_lifetime_ != 0) {
    // A face is up (or the same one again): the new face is drawn whole at once and gets
    // the new lifetime; a reveal still running keeps its counter.
    alert_effect_id_ = effect_id;
    AlertCornerDrawFull(effect_id);
    alert_lifetime_ = lifetime;
    return;
  }
  // Idle start: nothing drawn yet; the update reveals it over the next six steps.
  alert_effect_id_ = effect_id;
  alert_lifetime_ = lifetime;
  alert_reveal_counter_ = 6;
}

void MilestoneADemo::UpdateAlert() {
  if (alert_lifetime_ == 0) {
    return;
  }
  if (alert_lifetime_ != kAlertInfiniteLifetime) {
    --alert_lifetime_;
  }
  if (alert_reveal_counter_ > 0) {
    AlertCornerDrawStretch(alert_effect_id_,
                           kAlertRevealRows[static_cast<size_t>(alert_reveal_counter_ - 1)]);
    --alert_reveal_counter_;
  }
  if (alert_lifetime_ == 0) {
    AlertCornerRestore();
    alert_effect_id_ = -1;
  }
}

// 0x206C(1), called only by New Game init (0x0757): everything idle, the background back,
// the last-clear tick restarted and the previous clear size and warning cooldowns cleared.
void MilestoneADemo::ResetAlertState() {
  alert_reveal_counter_ = 0;
  alert_lifetime_ = 0;
  AlertCornerRestore();
  alert_effect_id_ = -1;
  last_clear_tick_ = Tick32();
  last_clear_was_four_ = false;
  warning_cooldowns_.fill(0);
}

void MilestoneADemo::AlertCornerRestore() {
  alert_corner_ = alert_corner_background_;
  alert_corner_dirty_ = true;
}

void MilestoneADemo::AlertCornerDrawFull(int effect_id) {
  const std::vector<Uint8>& tile = atet_data_.alert_tile(effect_id).indices;
  for (size_t index = 0; index < alert_corner_.size(); ++index) {
    if (tile[index] != kAlertTransparentIndex) {
      alert_corner_[index] = tile[index];
    }
  }
  alert_corner_dirty_ = true;
}

void MilestoneADemo::AlertCornerDrawStretch(int effect_id, int rows) {
  if (rows <= 0) {
    return;
  }
  const std::vector<Uint8>& tile = atet_data_.alert_tile(effect_id).indices;
  const int step = 0x320000 / rows;             // 50 rows in 16.16, idiv
  const int first_row = 25 - (rows >> 1);       // centred on corner row 25 (y 165)
  for (int band_row = 0; band_row < rows; ++band_row) {
    const int source_row = (step * band_row) >> 16;
    const int corner_row = first_row + band_row;
    for (int col = 0; col < kAlertFaceSize; ++col) {
      const size_t dst = static_cast<size_t>(corner_row * kAlertFaceSize + col);
      const Uint8 value = tile[static_cast<size_t>(source_row * kAlertFaceSize + col)];
      alert_corner_[dst] = value == kAlertTransparentIndex ? alert_corner_background_[dst] : value;
    }
  }
  alert_corner_dirty_ = true;
}

void MilestoneADemo::RenderAlertTile(SDL_Renderer* renderer) const {
  if (renderer == nullptr) {
    return;
  }
  if (alert_corner_texture_ == nullptr) {
    alert_corner_texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                              kAlertFaceSize, kAlertFaceSize);
    if (alert_corner_texture_ == nullptr) {
      return;
    }
    SDL_SetTextureScaleMode(alert_corner_texture_, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(alert_corner_texture_, SDL_BLENDMODE_NONE);
    alert_corner_dirty_ = true;
  }
  if (alert_corner_dirty_) {
    const std::array<Uint8, 2500>& corner = alert_corner_hold_ ? alert_corner_shown_ : alert_corner_;
    std::array<Uint8, kAlertFaceSize * kAlertFaceSize * 4> rgba{};
    for (size_t index = 0; index < corner.size(); ++index) {
      const SDL_Color& color = gameplay_palette_[corner[index]];
      rgba[index * 4 + 0] = color.r;
      rgba[index * 4 + 1] = color.g;
      rgba[index * 4 + 2] = color.b;
      rgba[index * 4 + 3] = SDL_ALPHA_OPAQUE;
    }
    SDL_UpdateTexture(alert_corner_texture_, nullptr, rgba.data(), kAlertFaceSize * 4);
    alert_corner_dirty_ = false;
  }
  const SDL_FRect dst = {static_cast<float>(kAlertFaceX), static_cast<float>(kAlertFaceY),
                         static_cast<float>(kAlertFaceSize), static_cast<float>(kAlertFaceSize)};
  SDL_RenderTexture(renderer, alert_corner_texture_, nullptr, &dst);
}

// 0x21C4, at the start of every normal step (never on collapse, dissolve or game-over
// steps): count the three cooldowns down, then look at the first occupied row of the
// locked board (20 when empty). A warning face is a one-shot 160-step face through the
// shared trigger; its sound needs the band's cooldown at 0 too, and the high band's sound
// also needs the mid cooldown at 0, so climbing from the mid band into the high band is
// silent. Every in-band step reloads the cooldowns, so nothing repeats while the stack
// stays in a band.
void MilestoneADemo::StackWarningStep() {
  for (int& cooldown : warning_cooldowns_) {
    if (cooldown > 0) {
      --cooldown;
    }
  }
  int& low = warning_cooldowns_[0];
  int& mid = warning_cooldowns_[1];
  int& high = warning_cooldowns_[2];
  const int highest = HighestOccupiedRow();
  const int r = (highest < 0 ? kBoardHeight : highest) - 1;
  if (r < kWarningHighLimit) {
    warning_band_ = 3;
    if (high == 0) {
      TriggerAlert(5, kAlertClearLifetime);
      if (mid == 0) {
        EmitAudioEvent("stack-warning-medium-high", 4, 0x80, 1);
      }
    }
    low = mid = high = kWarningCooldownTicks;
  } else if (r < kWarningMidLimit) {
    warning_band_ = 2;
    if (mid == 0) {
      TriggerAlert(4, kAlertClearLifetime);
      EmitAudioEvent("stack-warning-medium-high", 4, 0x80, 1);
    }
    mid = low = kWarningCooldownTicks;
  } else if (r < kWarningLowLimit) {
    warning_band_ = 1;
    if (low == 0) {
      TriggerAlert(3, kAlertClearLifetime);
      EmitAudioEvent("stack-warning-low", 3, 0x80, 1);
    }
    low = kWarningCooldownTicks;
  } else {
    warning_band_ = 0;
  }
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
// `0xeceb` (LCG 0x41C64E6D/0x3039, returning bits 16..30). The seed is the 60 Hz
// tick counter `0x2d2a3` (written only at 0x65B3/0x6683): 0 for the startup fill at
// 0x150, the current tick at every New Game; piece_seed_ holds the seed in use.
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
  live_snapshot_.repeat_timers = repeat_timers_;
  live_snapshot_.down_lockout_frames = down_lockout_frames_;
  live_snapshot_.collapse_frame = collapse_frame_;
  live_snapshot_.collapse_step = collapse_step_;
  live_snapshot_.collapse_batch = collapse_batch_;
  live_snapshot_.live_piece_hidden = live_piece_hidden_;
  live_snapshot_.top_out_after_collapse = top_out_after_collapse_;
  live_snapshot_.warning_band = warning_band_;
  live_snapshot_.warning_cooldowns = warning_cooldowns_;
  live_snapshot_.last_clear_was_four = last_clear_was_four_;
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
  repeat_timers_ = live_snapshot_.repeat_timers;
  down_lockout_frames_ = live_snapshot_.down_lockout_frames;
  collapse_frame_ = live_snapshot_.collapse_frame;
  collapse_step_ = live_snapshot_.collapse_step;
  collapse_batch_ = live_snapshot_.collapse_batch;
  live_piece_hidden_ = live_snapshot_.live_piece_hidden;
  top_out_after_collapse_ = live_snapshot_.top_out_after_collapse;
  warning_band_ = live_snapshot_.warning_band;
  warning_cooldowns_ = live_snapshot_.warning_cooldowns;
  last_clear_was_four_ = live_snapshot_.last_clear_was_four;
}

int MilestoneADemo::RollNextPieceType() {
  // 0x1368..0x1380 (and 0x70C): the next piece id is the drawn value % 7 (signed idiv; values are
  // below 2^31).
  return static_cast<int>(NextGameplayRandomU32() % kPieceCount);
}

void MilestoneADemo::SimulateLiveLineClear(int clear_count) {
  InjectFilledRowsForSmoke(clear_count);
}

void MilestoneADemo::InjectFilledRowsForSmoke(int row_count) {
  // Smoke/debug helper (--live-demo, keys C and B): fill the bottom rows and clear them through the
  // same bookkeeping as a clearing lock (particles, collapse, Down lockout, level, reward, lines,
  // score) but without a lock or a promotion. Ignored while a collapse or top-out is running.
  if (!live_game_active_ || gameplay_phase_ != GameplayPhase::kFalling) {
    return;
  }
  row_count = std::clamp(row_count, 1, 4);
  for (int row = 0; row < row_count; ++row) {
    const int board_y = kBoardHeight - 1 - row;
    for (int x = 0; x < kBoardWidth; ++x) {
      live_board_cells_[static_cast<size_t>(board_y * kBoardWidth + x)] = ((x + row) % 7) + 1;
    }
  }
  const std::vector<int> rows = DetectFullRows();
  BeginLineCollapse(rows);
  down_lockout_frames_ = kDownLockoutAfterClear;
  ApplyLineClearScore(static_cast<int>(rows.size()));
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
    // In a live game the five bound keys act only through the held-key table that the gameplay step
    // reads (0x9C8 samples it once per step: a press and release between two steps is never seen,
    // and a press is acted on by the next step). The press is consumed here so it reaches no other
    // handler.
    return std::find(keyboard_bindings_.begin(), keyboard_bindings_.end(), original_scancode) !=
           keyboard_bindings_.end();
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

void MilestoneADemo::RenderLiveHud(SDL_Renderer* renderer) const {
  const int high_score_value = high_scores_.empty() ? 0 : high_scores_[0].score;
  DrawCounter(renderer, digit_atlas_.texture, {36, 89, 7, high_score_value});
  DrawCounter(renderer, digit_atlas_.texture, {36, 101, 7, live_score_});
  DrawCounter(renderer, digit_atlas_.texture, {60, 112, 3, live_level_});
  DrawCounter(renderer, digit_atlas_.texture, {144, 195, 4, live_lines_});

  for (size_t i = 0; i < live_piece_stats_.size(); ++i) {
    DrawCounter(renderer, digit_atlas_.texture, {kPieceStatX, kPieceStatRows[i], 4, live_piece_stats_[i]});
  }

  DrawPreviewPiece(renderer, piece_atlas_.texture, live_next_piece_type_);
}

void MilestoneADemo::DrawBoardCells(SDL_Renderer* renderer, const std::array<int, 200>& cells,
                                    bool hide_clearing_rows) const {
  for (int y = 0; y < kBoardHeight; ++y) {
    // Each line-clear helper erases its 80x8 band right after spawning the band's
    // particles (memset at e.g. 0x155F), in the clear tick itself: a cleared row is never
    // drawn again; its debris (stage-0 colour = tile colour) shows it for that one frame.
    if (hide_clearing_rows && gameplay_phase_ == GameplayPhase::kCollapse &&
        std::find(pending_clear_rows_.begin(), pending_clear_rows_.end(), y) != pending_clear_rows_.end()) {
      continue;
    }
    for (int x = 0; x < kBoardWidth; ++x) {
      const int cell = cells[static_cast<size_t>(y * kBoardWidth + x)];
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
    RenderLiveHud(renderer);

    // Collapse look (0x1D04 / 0x1E34): the cleared rows were zeroed in the lock step (their pixels left
    // as particles), and each collapse call moves the band between the stack top and the current
    // cleared row down one scanline. Everything above that row is empty but the stack, so drawing
    // the rows above it collapse_step_ pixels lower reproduces the memmove pixel for pixel; the 8th
    // call compacts the board instead. If the spawn failed in the clearing step the stack stays put
    // and each call blanks one scanline of the bottom row instead (empty-board anchor 152).
    const bool collapsing = gameplay_phase_ == GameplayPhase::kCollapse &&
                            collapse_batch_ < static_cast<int>(pending_clear_rows_.size());
    const int slide_row = collapsing && !top_out_after_collapse_
                              ? pending_clear_rows_[static_cast<size_t>(collapse_batch_)]
                              : -1;
    const int blanked_bottom_lines =
        collapsing && top_out_after_collapse_ ? std::min(collapse_frame_, kPieceTileSize) : 0;
    for (int y = 0; y < kBoardHeight; ++y) {
      const int slide = y < slide_row ? collapse_step_ : 0;
      const int skip_lines = y == kBoardHeight - 1 ? blanked_bottom_lines : 0;
      for (int x = 0; x < kBoardWidth; ++x) {
        const int cell = live_board_cells_[static_cast<size_t>(y * kBoardWidth + x)];
        if (cell <= 0) {
          continue;
        }
        if (skip_lines > 0) {
          if (skip_lines < kPieceTileSize && piece_atlas_.texture != nullptr) {
            const SDL_FRect source = {static_cast<float>((cell - 1) * kPieceTileSize), static_cast<float>(skip_lines),
                                      static_cast<float>(kPieceTileSize), static_cast<float>(kPieceTileSize - skip_lines)};
            const SDL_FRect destination = {static_cast<float>(kGameplayClearX + (x * kPieceTileSize)),
                                           static_cast<float>(kGameplayClearY + (y * kPieceTileSize) + skip_lines),
                                           static_cast<float>(kPieceTileSize), static_cast<float>(kPieceTileSize - skip_lines)};
            SDL_RenderTexture(renderer, piece_atlas_.texture, &source, &destination);
          }
          continue;
        }
        DrawPieceTile(
            renderer,
            piece_atlas_.texture,
            cell - 1,
            kGameplayClearX + (x * kPieceTileSize),
            kGameplayClearY + (y * kPieceTileSize) + slide);
      }
    }

    // 0x107C/0x10A1: the live piece is drawn at the end of every live step (also the one that starts
    // a top-out, where the colliding new piece is drawn over the stack), never while rows are pending,
    // and a piece promoted during a clear only from its first live step.
    if (!live_piece_hidden_ && gameplay_phase_ != GameplayPhase::kCollapse) {
      DrawLiveTetromino(
          renderer,
          piece_atlas_.texture,
          live_piece_.type,
          live_piece_.rotation,
          live_piece_.x,
          live_piece_.y);
    }

    // The face is part of the working screen; the debris is plotted after every tick
    // (0x5AE -> 0x2F24), over the face. A screen restored by the frontend holds no
    // particles, so none are drawn until the first tick back in the game steps them.
    RenderAlertTile(renderer);
    if (!debris_hidden_after_frontend_) {
      RenderDebrisParticles(renderer);
    }
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
      .Key("tick").UInt(Tick32())
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
      .Key("lock_frames").Int(0)  // no grounded lock timer any more (GR-01); field kept for the trace schema
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
  // Gameplay repeats use 0x9C8's four timers (repeat_timers_: act at 0, reload 8 =
  // kGameplayBindingRepeatFrames); held_frames above are the frontend counters (Left/Right edit
  // repeats at 12 then every 4).
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

  // Test hook (effects-alerts prototype): the whole particle pool for the steps in
  // ATET_AUDIT_PARTICLES=A:B, in container order (index 0 = first spawned), each as
  // [x, y, vx, vy, age, decay, color] (16.16 fixed point, as the original's nodes).
  {
    static const std::pair<std::uint64_t, std::uint64_t> particle_range = [] {
      std::pair<std::uint64_t, std::uint64_t> range{1, 0};  // empty unless the variable is set
      const char* spec = SDL_getenv("ATET_AUDIT_PARTICLES");
      unsigned long long from = 0;
      unsigned long long to = 0;
      if (spec != nullptr && SDL_sscanf(spec, "%llu:%llu", &from, &to) == 2 && from <= to) {
        range = {from, to};
      }
      return range;
    }();
    if (audit_step_ >= particle_range.first && audit_step_ <= particle_range.second) {
      json.Key("particle_list").BeginArray();
      for (const DebrisParticle& particle : debris_particles_) {
        json.BeginArray()
            .Int(particle.x)
            .Int(particle.y)
            .Int(particle.vx)
            .Int(particle.vy)
            .Int(particle.age)
            .Int(particle.decay)
            .Int(particle.color)
            .EndArray();
      }
      json.EndArray();
    }
  }

  json.Key("warning")
      .BeginObject()
      .Key("band").Int(warning_band_)
      .Key("cooldowns").BeginArray().Int(warning_cooldowns_[0]).Int(warning_cooldowns_[1]).Int(warning_cooldowns_[2]).EndArray()
      .Key("sleepy").Bool(alert_effect_id_ == 11)
      .Key("frames_since_clear").Int(static_cast<long long>(Tick32() - last_clear_tick_))
      .EndObject();

  json.Key("alert").BeginObject().Key("id");
  if (alert_effect_id_ >= 0) {
    json.Int(alert_effect_id_);
  } else {
    json.Null();
  }
  json.Key("lifetime").Int(alert_lifetime_)
      .Key("reveal_frame").Int(6 - alert_reveal_counter_)
      .Key("reveal_counter").Int(alert_reveal_counter_)
      .Key("tick").UInt(Tick32())
      .Key("last_clear_tick").UInt(last_clear_tick_)
      .EndObject();

  json.Key("topout")
      .BeginObject()
      .Key("frame").Int(top_out_frame_)
      .Key("gameplay_frame").Int(top_out_frame_)
      .Key("dissolved_pixel_rows").Int(top_out_dissolved_rows_)
      .Key("overlay_visible").Bool(scene_ == Scene::kTopOut && top_out_dissolved_rows_ >= kTopOutDissolveFrames)
      .Key("saved_under_valid").Bool(false)
      .Key("handoff_pending").Bool(false)
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
