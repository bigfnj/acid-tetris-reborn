#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atet_data.h"

namespace atet {

class JsonOut;  // audit_instrument.h (audit worktree only)

class MilestoneADemo {
 public:
  MilestoneADemo() = default;

  // ---- Audit instrumentation (audit/instrument worktree only; diagnostics) ----
  // Called by main at the top of each loop iteration: resets per-step
  // accumulators (RNG draws, gravity bracket, emitted events, particle spawns).
  void AuditBeginStep(std::uint64_t step);
  // Labels where emitted audio events originate: "input", "tick", "render".
  void AuditSetStage(const char* stage) { audit_stage_ = stage; }
  // Full post-iteration state snapshot (see harness/port/README.md for fields).
  void AuditWriteState(JsonOut& json, bool include_rng_values) const;
  // Presentation counters that Render() consumes/advances (sampled pre-render).
  void AuditWritePresentation(JsonOut& json) const;
  // Audio events pushed by EmitAudioEvent/EmitMusicEvent during this step.
  void AuditWriteEmittedEvents(JsonOut& json) const;
  MilestoneADemo(const MilestoneADemo&) = delete;
  MilestoneADemo& operator=(const MilestoneADemo&) = delete;

  // The decoded ATET.DAT (populated by Initialize) -- also the source for the
  // audio/music chunks that main.cpp routes into the SFX/music players.
  const AtetData& atet_data() const { return atet_data_; }

  ~MilestoneADemo();

  struct AudioEventNotification {
    std::string_view name;
    bool is_sfx = false;
    int slot = 0;
    int track = 0;
    int music_volume = 0;
    int sfx_volume = 0;
    int pan = 0x80;         // 0x6817 ECX: 0x80 centered, offsets bias left/right
    int voice_class = 0;    // 0x6817 EDX: coarse playback-class offset (0 UI, 1 warning)
  };

  bool Initialize(SDL_Renderer* renderer, std::string* error_out, bool start_in_sound_setup = false);
  bool HandleKey(SDL_Keycode key);
  bool HandleKeyDown(SDL_Keycode key);
  void HandleKeyUp(SDL_Keycode key);
  void ResetBuildLocalSetupState();
  void BeginLiveGameplayDemo(int simulated_clear_count = 0);
  void BeginTopOutDemo(int score, int lines);
  void InjectFilledRowsForSmoke(int row_count);
  bool CaptureGameplaySnapshot(SDL_Renderer* renderer);
  void Tick();
  void Render(SDL_Renderer* renderer);
  void Shutdown();

  [[nodiscard]] const char* scene_name() const;
  [[nodiscard]] std::string debug_state_line() const;
  [[nodiscard]] int current_preset_index() const { return gameplay_preset_index_; }
  [[nodiscard]] bool quit_requested() const { return quit_requested_; }
  void set_debug_state_logging(bool enabled) { debug_state_logging_ = enabled; }
  void set_audio_event_callback(std::function<void(const AudioEventNotification&)> callback) {
    audio_event_callback_ = std::move(callback);
  }
  void set_piece_seed(std::uint32_t seed) { piece_seed_ = seed == 0 ? 1 : seed; }
  void set_start_level_override(int level);
  // Disable the palette fades (the four 63-tick dispatcher fades and the splash
  // brightness ramps) for --no-fade and the --live-demo/--topout-demo shortcuts.
  // Every other frontend timing (splash pages, boot music wait, 48-step row
  // reveal/conceal, pulse gates) still runs.
  void set_fades_enabled(bool enabled) { fades_enabled_ = enabled; }
  // Developer shortcuts (Tab, T, G, 1-3) jump between scenes without the
  // dispatcher and do not exist in the original; off unless --debug.
  void set_debug_keys_enabled(bool enabled) { debug_keys_enabled_ = enabled; }
  // Legacy black-overlay alpha. Every fade is now rendered as a palette level
  // (the original's 0x284c scaling), so this is always 0; kept for main.cpp.
  [[nodiscard]] int screen_fade_alpha() const;

 private:
  enum class Scene {
    kSplash,
    kFrontend,
    kGameplay,
    kTopOut,
  };

  // Frontend flow, modelled on the original's dispatcher 0x3830 and the screen
  // loops it calls. Each phase is advanced once per logic tick from Tick();
  // Render() only draws the state.
  enum class FlowPhase {
    kNone,             // gameplay / top-out: no frontend flow running
    kSplash,           // 0x2998 splash page tick loop (splash_page_ 0 = DDD, 1 = WARNING)
    kBootGap,          // after the WARNING: load allowance + 0x6544's 32-tick music wait (black)
    kEntryPaletteOut,  // 0x64f0: frozen picture, gameplay palette 64-e for e = 1..63 (busy wait)
    kEntryTitleIn,     // 0x63b8: title + pointfield, title palette level e for e = 0..62
    kReveal,           // 0x3df4 (menus, credits pages) / 0x50b0 table reveal: 48 steps
    kScreen,           // the current screen's own per-tick loop
    kConceal,          // 0x3ee4 / 0x50b0 table conceal: 48 steps
    kExitTitleOut,     // 0x62e0 at the dispatcher exit (states 2 and 4, Sound Setup state 2)
    kExitPaletteIn,    // 0x6498: frozen gameplay picture, level e for e = 1..63, then full
    kQuitTitleOut,     // state 3: 0x699e(2,0x40) music ramp + 0x62e0, then quit
    kMusicFreeze,      // 0x6544 called inside a main-menu tick: 32 ticks with no presents
  };
  // Where the dispatcher exit tail goes after its fades.
  enum class FlowExit { kNone, kNewGame, kResume, kSplashes };
  // What the 0x64f0 entry fade shows.
  enum class FrozenSource { kBlack, kGameplay, kTopOut };

  enum class FrontendView {
    kMainMenu,
    kOptions,
    kKeyboardSetup,
    kHighScores,
    kHighScoreEntry,
    kCredits,
    kSoundSetup,
  };

  struct TextureAsset {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
  };

  struct FrontendPoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;
  };

  // 0x5ef4 / 0x39c4 state, all 32-bit like the original (IMUL/IDIV wrap).
  struct FrontendPointfieldState {
    std::int32_t origin_x = 0;          // [0x2d22b]
    std::int32_t origin_y = 0x1000000;  // [0x2d227]
    std::int32_t origin_z = 0;          // [0x2d213]
    std::int32_t phase_x = 0x200;       // [0x2d23f]
    std::int32_t phase_y = 0x200;       // [0x2d243]
    std::int32_t phase_z = 0x200;       // [0x2d23b]
    std::int32_t angle_xy = 0;          // [0x2d237]
    std::int32_t angle_z = 0;           // [0x2d247]
  };

  // One projected point as 0x39c4 stores it (+0x0c x, +0x10 y, +0x14 colour).
  struct ProjectedPoint {
    int x = -1;
    int y = -1;
    int color = 0;
    bool valid = false;
  };

  struct HighScoreRecord {
    std::string name;
    int score = 0;
    int lines = 0;
  };

  struct LivePiece {
    int type = 0;
    int rotation = 0;
    int x = 4;
    int y = 0;
  };

  enum class GameplayPhase {
    kFalling,
    kCollapse,
    kTopOutDissolve,
  };

  // Line-clear debris particle, modelling the original's spawners 0x2f78/0x3034
  // (RE: line-clear-helper-decode-pass-2026-07-21). Position and velocity are
  // 16.16 fixed-point; age accumulates by decay toward 0x400 over the lifetime,
  // with the 0..3 fade stage taken from the high bits.
  struct DebrisParticle {
    int x = 0;      // 16.16 fixed-point screen X
    int y = 0;      // 16.16 fixed-point screen Y
    int vx = 0;     // 16.16 fixed-point per-tick velocity
    int vy = 0;
    int age = 0;    // 0..0x400 accumulator
    int decay = 0;  // 0x400 / lifetime, added to age each tick
    int color = 0;  // sampled palette index into the chunk-3 four-stage ramps
  };

  struct LiveSnapshot {
    bool valid = false;
    GameplayPhase phase = GameplayPhase::kFalling;
    std::array<int, 200> board_cells{};
    std::array<int, 7> piece_stats{};
    LivePiece piece;
    int next_piece_type = 0;
    int score = 0;
    int lines = 0;
    int level = 0;
    int gravity_accumulator = 0;
    int lock_frames = 0;
    int down_lockout_frames = 0;
    int collapse_frame = 0;
    int top_out_frame = 0;
    int warning_band = 0;
    std::array<int, 3> warning_cooldowns{};
    bool last_clear_was_four = false;
    bool sleepy = false;
    int frames_since_clear = 0;
  };

  struct AudioEvent {
    std::string name;
    bool is_sfx = false;
    int slot = 0;
    int track = 0;
    int music_volume = 0;
    int sfx_volume = 0;
    int pan = 0x80;
    int voice_class = 0;
  };

  // Build a texture from in-memory decoded indices + a palette (in-engine
  // extraction), replacing the old file+palette-path loader.
  bool LoadTextureFromData(
      SDL_Renderer* renderer,
      const AtetData::Indexed& image,
      const std::array<SDL_Color, 256>& palette,
      int transparent_index,
      TextureAsset* asset_out,
      std::string* error_out);

  bool LoadFrontendFont(std::string* error_out);
  bool LoadFrontendPointfield(std::string* error_out);
  bool LoadGameplayParticleAssets(std::string* error_out);
  bool LoadSetupDat();
  bool SaveSetupDat() const;

  void RenderFrontend(SDL_Renderer* renderer);
  void RenderGameplay(SDL_Renderer* renderer) const;
  bool RenderGameplayToTexture(SDL_Renderer* renderer, SDL_Texture* texture);
  void RenderTopOut(SDL_Renderer* renderer);
  void RenderTopOutBackdrop(SDL_Renderer* renderer);
  void RenderDebrisParticles(SDL_Renderer* renderer) const;
  void UpdateTopOutDissolve();
  void SpawnTopOutPixelRow(int pixel_row);
  // Gameplay alert/mood-face tile system (chunk-6). TriggerAlert arms a tile for a
  // lifetime (restarting the reveal on a change of tile); UpdateAlert ages it;
  // RenderAlertTile draws the current tile with a staged vertical reveal-in.
  void TriggerAlert(int effect_id, int lifetime);
  void UpdateAlert();
  void RenderAlertTile(SDL_Renderer* renderer) const;
  int AlertIdForClear(int cleared_rows, bool was_four, bool was_sleepy) const;
  bool CaptureTopOutSavedUnder(SDL_Renderer* renderer);
  void RenderSplash(SDL_Renderer* renderer);
  void RenderFrontendPointfield(SDL_Renderer* renderer);
  void RenderFrontendRows(SDL_Renderer* renderer) const;
  void RenderFrontendText(SDL_Renderer* renderer, const std::string& text, int center_x, int y, int reveal_amount) const;
  void AdvanceTopOutTimers();
  void TrackKeyDown(SDL_Keycode key);
  void TrackKeyUp(SDL_Keycode key);
  bool ApplyGameplayBinding(std::uint8_t original_scancode, bool allow_down);
  bool HandleGameplayBinding(SDL_Keycode key);

  // ---- frontend flow (dispatcher 0x3830 model) ----
  // 32-entry release history of the keyboard ISR 0x800 (newest at [31]):
  // 0x964 peeks the newest release, 0x94c clears the history.
  void PushRelease(std::uint8_t original_scancode);
  [[nodiscard]] std::uint8_t PeekRelease() const { return release_fifo_[31]; }
  void ClearReleases() { release_fifo_.fill(0); }
  [[nodiscard]] bool KeyHeld(std::uint8_t original_scancode) const { return held_original_scancodes_[original_scancode]; }
  void BeginStartupSplashes();
  void BeginSplashPage(int page);
  void TickSplash();
  void TickBootGap();
  // Dispatcher entry: 0x64f0 over the frozen picture, title copy, 0x63b8, then
  // the screen for `state` (1 main menu, 5 Sound Setup, 9 high scores after a game).
  void BeginDispatcherEntry(int state, FrozenSource source);
  void TickEntryPaletteOut();
  void TickEntryTitleIn();
  // Starts the screen for a dispatcher state: builds its row strings and runs the reveal.
  void StartScreen(int state);
  void TickReveal();
  void TickScreen();
  void TickConceal();
  // Dispatcher loop after a screen returned `state`.
  void DispatchState(int state);
  void BeginDispatcherExit(FlowExit exit);
  void TickExitTitleOut();
  void TickExitPaletteIn();
  // New Game exit tail: 0x6498 shows the screen saved at the entry with the well
  // and the next box zeroed by 0x05e0.
  void UseSavedScreenForNewGame();
  void FinishDispatcherExit();
  void BeginQuitFade();
  void TickQuitTitleOut();
  void TickMusicFreeze();
  void TickMainMenu(bool resume_after_music);
  void TickOptions();
  void TickKeyboardSetup();
  void TickSoundSetup();
  void TickCredits();
  void TickHighScores();
  // One frontend logic tick of the active phase. frame_start is false for the
  // extra ticks of a catch-up burst (a screen only leaves at the next frame).
  void TickFrontendOnce(bool frame_start);
  // 0x3fd8(row, mode): menu row pulse on the shared phase [0x1990f].
  int PulseRow(int row, int mode);
  // 0x5868(slot, mode): high-score entry row pulse on its own phase [0x19913].
  int PulseEntryRow(int slot, int mode);
  void SetAllRowAmounts(int amount);
  void SetMainMenuRowText(int row);
  void BuildOptionsRows();
  void BuildKeyboardSetupRows();
  void BuildSoundSetupRows();
  void BuildCreditsRows();
  void BuildHighScoreRows();
  // 0x5ef4 / 0x39c4.
  void InitFrontendPointfield();
  void StepFrontendPointfield();
  void SeedBootRandomTable();
  // Frozen-picture capture for the busy-wait palette fades.
  void CaptureFrozenPicture(SDL_Renderer* renderer);
  void RenderFrozenPicture(SDL_Renderer* renderer, int level);
  void RenderTopOutFrozen(SDL_Renderer* renderer);
  void EnsureTitleLevel(int level);
  void EnsureSplashLevel(int page, int level);
  [[nodiscard]] std::array<SDL_Color, 256> ScaledPalette(int chunk_id, int level) const;
  [[nodiscard]] int CurrentTitleLevel() const;
  [[nodiscard]] int CurrentSplashLevel() const;
  [[nodiscard]] int CurrentFrozenLevel() const;
  [[nodiscard]] int FlowFadeE() const;
  [[nodiscard]] const char* FlowPhaseName() const;
  void MarkSetupDirty() { setup_dirty_ = true; }
  bool TryMoveLivePiece(int delta_x, int delta_y);
  bool TryRotateLivePiece(int delta_rotation);
  bool CanPlaceLivePiece(const LivePiece& piece) const;
  void ResetLiveGameplayState();
  bool SpawnNextLivePiece();
  void CommitLivePieceToBoard();
  void StepLiveGameplay();
  void StepFallingGameplay();
  void StepCollapseGameplay();
  void StepTopOutGameplay();
  int CurrentDropIncrement() const;
  void BeginLineCollapse(const std::vector<int>& rows);
  std::vector<int> DetectFullRows() const;
  void ApplyLineClearScore(int clear_count);
  void CollapseMarkedRows();
  void SeedLineClearDebris(const std::vector<int>& rows);
  // One of the ten level%10 row-clear emitters (0x14b4..0x1c40): draws the
  // family's RNG (for the burst families) and spawns a particle for the sampled
  // pixel. col is the original 0..79 column, row8 the 0..7 band row.
  void EmitLineClearParticle(int style, int col, int row8, int screen_x, int screen_y, int color);
  void SpawnDebrisPolar(int screen_x, int screen_y, int speed, int angle, int lifetime, int color);   // 0x2f78
  void SpawnDebrisTarget(int screen_x, int screen_y, int target_x, int target_y, int lifetime, int color);  // 0x3034
  void StepDebrisParticles();
  void UpdateStackWarning(bool allow_sound);
  int HighestOccupiedRow() const;
  int CurrentGravityRate() const;
  void SaveLiveSnapshot();
  void RestoreLiveSnapshot();
  void RefillGameplayRandomTable();
  std::uint32_t NextGameplayRandom15();
  std::uint32_t NextGameplayRandomU32();
  int RollNextPieceType();
  void SimulateLiveLineClear(int clear_count);
  [[nodiscard]] int CurrentRunScore() const;
  [[nodiscard]] int CurrentRunLines() const;
  void StepGameplayPreset(int delta);
  void PersistSetupState() const;
  // sfx_volume_override < 0 uses the saved SFX volume; the piece-lock sound
  // passes half (the original halves the 0x6817 volume arg for that call only).
  void EmitAudioEvent(const char* event_name, int slot, int pan = 0x80, int voice_class = 0, int sfx_volume_override = -1);
  void EmitMusicEvent(const char* event_name);
  // Map a live piece column to a 0x6817 pan value around center (modeled: the
  // exact original board-position-to-pan curve is not recovered).
  [[nodiscard]] int PanForPieceColumn(int piece_x) const;
  void FlushAudioEvents();
  void StartCurrentMusic(const char* reason);
  void ScheduleExitFade();
  void ApplyMusicVolume();
  void ResetSetupStateToDefaults();
  // Game over to the high-score table (state 9): kept as the top-out code's
  // entry point; it now starts the dispatcher entry with the frozen top-out
  // picture, and the table is qualified when 0x50b0(1) starts.
  void BeginHighScoreNameEntry(int score, int lines);
  [[nodiscard]] std::string KeyboardBindingRowText(int row_index) const;
  [[nodiscard]] int MeasureFrontendText(const std::string& text) const;
  // Amount the selected row was last drawn with (debug/trace only).
  [[nodiscard]] int PulseRevealAmount() const;
  [[nodiscard]] int VisibleRowCount() const;

  Scene scene_ = Scene::kFrontend;
  FrontendView frontend_view_ = FrontendView::kMainMenu;
  int frontend_selected_row_ = 0;
  int gameplay_preset_index_ = 0;
  int music_track_index_ = 0;
  int start_level_ = 0;
  int music_volume_ = 100;
  int sfx_volume_ = 90;
  int credits_page_ = 0;
  int keyboard_capture_row_ = -1;
  int sound_device_index_ = 0;
  int mixing_rate_hz_ = 44100;
  int stereo_flag_ = 1;
  int bit_depth_flag_ = 1;
  std::array<std::uint8_t, 5> keyboard_bindings_ = {0xD0, 0xCB, 0xCD, 0x1E, 0x1F};
  std::array<bool, 256> held_original_scancodes_{};
  std::array<int, 256> held_original_scancode_frames_{};
  std::array<HighScoreRecord, 5> high_scores_{};
  bool live_game_available_ = false;
  bool live_game_active_ = false;
  bool quit_requested_ = false;
  std::array<int, 200> live_board_cells_{};
  std::array<int, 7> live_piece_stats_{};
  LivePiece live_piece_;
  int live_next_piece_type_ = 0;
  int live_score_ = 0;
  int live_lines_ = 0;
  int live_level_ = 0;
  std::uint32_t piece_seed_ = 0x41C64E6D;
  std::uint32_t gameplay_random_state_ = 0x41C64E6D;
  std::array<std::uint32_t, 8192> gameplay_random_table_{};
  int gameplay_random_index_ = 0;
  GameplayPhase gameplay_phase_ = GameplayPhase::kFalling;
  int gravity_accumulator_ = 0;
  int lock_frames_ = 0;
  int down_lockout_frames_ = 0;
  std::vector<int> pending_clear_rows_;
  int collapse_frame_ = 0;
  int gameplay_top_out_frame_ = 0;
  int warning_band_ = 0;
  std::array<int, 3> warning_cooldowns_{};
  std::vector<DebrisParticle> debris_particles_;
  LiveSnapshot live_snapshot_;
  SDL_Texture* gameplay_snapshot_texture_ = nullptr;
  std::function<void(const AudioEventNotification&)> audio_event_callback_;
  bool gameplay_snapshot_texture_valid_ = false;
  bool last_clear_was_four_ = false;
  bool sleepy_ = false;
  int frames_since_clear_ = 0;
  bool debug_state_logging_ = false;
  std::vector<AudioEvent> audio_events_;
  int line_clear_demo_count_ = 0;

  // fades_enabled_ is cleared by --no-fade and the demo shortcuts (see setter).
  bool fades_enabled_ = true;
  bool debug_keys_enabled_ = false;

  // ---- frontend flow state (dispatcher 0x3830 model) ----
  std::uint32_t ticks_ = 0;            // [0x2d2a3]: logic ticks since the timer started (step + 1)
  std::array<std::uint8_t, 32> release_fifo_{};
  FlowPhase flow_phase_ = FlowPhase::kNone;
  std::uint32_t flow_t0_ = 0;          // tick at which the current timed phase started
  int flow_step_ = 0;                  // reveal / conceal step (0..48)
  int dispatch_state_ = 1;             // screen the dispatcher runs (1 menu, 5 setup, 6 options, ...)
  FlowExit flow_exit_ = FlowExit::kNone;
  FrozenSource frozen_source_ = FrozenSource::kBlack;
  bool frozen_capture_pending_ = false;
  std::vector<Uint8> frozen_rgba_;     // captured 320x240 RGBA picture for 0x64f0 / 0x6498
  std::vector<Uint8> saved_screen_rgba_;  // [0x2c69f]: the screen 0x3830 saved at its entry
  SDL_Texture* frozen_texture_ = nullptr;
  int frozen_texture_level_ = -1;
  int menu_pending_ = 0;               // 0 idle, 1 leave screen, 2 up, 3 down, 4 key capture
  int menu_return_state_ = 1;
  bool screen_exit_ = false;           // set when the gate let the action through
  int main_menu_row_ = 0;              // [0x1886f], kept across screens and games
  int pulse_phase_ = 0;                // [0x1990f], shared by every 0x3fd8 pulse
  int entry_pulse_phase_ = 0;          // [0x19913], high-score entry row
  std::array<std::string, 8> row_text_{};   // row strings 0x2d0d3 + 40*row
  std::array<int, 8> row_amount_{};         // last amount each row was drawn with (0 = blank)
  std::array<std::string, 5> hs_score_text_{};  // high-score columns 0x2cc93 / 0x2cb53
  std::array<std::string, 5> hs_lines_text_{};
  int frontend_burst_ = 0;             // stale-pacer catch-up: menu ticks the next step runs
  bool state9_pending_ = false;        // game over -> high scores requested outside Tick
  FrozenSource state9_source_ = FrozenSource::kTopOut;
  int music_freeze_left_ = 0;
  int esc_hold_ticks_ = 0;             // high-score footer: ticks spent frozen on a held Esc
  bool setup_dirty_ = false;           // SETUP.DAT edits not yet saved (saved at the exit points)
  int title_level_ = 64;               // title palette level shown by the frontend (0..64)
  int rendered_title_level_ = -1;
  int rendered_splash_page_ = -1;
  int rendered_splash_level_ = -1;
  std::array<SDL_Color, 256> scaled_title_palette_{};
  int sound_rate_index_ = 6;           // Sound Setup local rate index ([esp+0x1c] of 0x5a94)
  // credits 0x4f38
  int credits_mode_ = 1;               // 1 build + reveal page, 2 conceal + next, 0 hold
  int credits_counter_ = 0;            // EBX
  bool credits_esc_latch_ = false;
  // high scores 0x50b0
  int hs_mode_ = 0;                    // 1 = a game just ended (qualification + name entry)
  int hs_slot_ = -1;
  int hs_stage_ = 0;                   // 0 name entry, 1 footer
  std::string hs_buffer_;
  int hs_len_ = 0;
  bool hs_caps_ = false;
  bool hs_commit_ = false;
  bool hs_done_ = false;

  TextureAsset title_screen_;
  TextureAsset gameplay_screen_;
  TextureAsset piece_atlas_;
  TextureAsset digit_atlas_;
  TextureAsset frontend_font_atlas_;

  std::array<int, 66> frontend_font_widths_{};
  std::array<SDL_Color, 256> title_palette_{};
  std::array<SDL_Color, 256> gameplay_palette_{};
  std::array<Uint8, 1024> particle_ramps_{};
  std::vector<Uint8> title_indices_;
  std::vector<Uint8> piece_atlas_indices_;
  std::array<std::vector<FrontendPoint>, 8> frontend_banks_;
  std::array<int, 2048> sine_table_{};           // port table (amplitude 65536), used by debris
  std::array<int, 2048> original_sine_table_{};  // 0x27b8 table (peak 32768): pointfield + pulses
  FrontendPointfieldState frontend_pointfield_state_;
  std::array<FrontendPoint, 1024> frontend_active_points_{};      // working array [0x2d223]
  std::array<FrontendPoint, 1024> frontend_morph_delta_{};        // 0x3c70 deltas
  std::array<ProjectedPoint, 1024> frontend_projected_{};
  int frontend_current_bank_ = 5;
  int frontend_next_bank_ = 0;
  int frontend_cycle_counter_ = 0;
  int frontend_frame_counter_ = 0;   // pointfield steps since boot
  int frontend_view_frame_ = 0;      // ticks since the current screen started
  int splash_page_ = 0;
  int splash_frame_ = 0;             // ticks since the page's t0
  int splash_hold_t1_ = 0;           // 0x2998 hold start (relative)
  int splash_stage_ = 0;             // 0 fade-in, 1 hold, 2 fade-out
  int splash_level_ = 0;
  int top_out_frame_ = 0;
  int top_out_score_ = 0;
  int top_out_lines_ = 0;
  bool top_out_handoff_pending_ = false;
  SDL_Texture* top_out_saved_under_texture_ = nullptr;
  bool top_out_saved_under_valid_ = false;

  TextureAsset ddd_splash_;
  TextureAsset warning_splash_;
  TextureAsset game_over_overlay_;
  // The chunk-6 50x50 alert/mood-face bank: alert ID N == tile N
  // (RE alert-id-correlation-pass). 3/4/5 = low/med/high stack warning,
  // 6 = top-out/game-over, 2/0/9/7 = 1/2/3/4-line clear, 1/10 = fast follow-up,
  // 8 = back-to-back tetris, 11 = sleepy, 12 = wake-after-clear, 0/13 misc/unused.
  std::array<TextureAsset, 14> alert_tiles_{};
  // Gameplay alert-tile controller (RE board-alert-pass 0x206c/0x2008): the
  // currently-shown mood tile, its remaining on-screen lifetime, and the frame
  // counter that drives the staged vertical reveal-in.
  int alert_effect_id_ = -1;
  int alert_lifetime_ = 0;
  int alert_reveal_frame_ = 0;
  // Rows already converted to upward debris fountains during the top-out dissolve
  // (GAP A): tracks the dissolve front so each newly-cleared row bursts once.
  int top_out_dissolved_rows_ = 0;

  AtetData atet_data_;

  // ---- Audit instrumentation state (never read by game logic) ----
  struct AuditEmitted {
    std::string stage;
    std::string name;
    bool is_sfx = false;
    int slot = 0;
    int track = 0;
    int music_volume = 0;
    int sfx_volume = 0;
    int pan = 0x80;
    int voice_class = 0;
  };
  std::uint64_t audit_step_ = 0;
  const char* audit_stage_ = "setup";
  std::vector<AuditEmitted> audit_emitted_;
  int audit_rng_index_before_ = 0;
  std::uint32_t audit_rng_draws_ = 0;
  std::uint64_t audit_rng_hash_ = 0;
  std::vector<std::uint32_t> audit_rng_values_;
  bool audit_gravity_active_ = false;
  int audit_gravity_rate_ = 0;
  int audit_gravity_acc_before_ = 0;
  int audit_descent_attempts_ = 0;
  int audit_descent_successes_ = 0;
  int audit_score_before_ = 0;
  std::uint32_t audit_particles_spawned_ = 0;
  std::uint32_t audit_particles_dropped_ = 0;
  std::uint32_t audit_particles_retired_ = 0;
  std::array<bool, 256> audit_pressed_{};
  std::array<bool, 256> audit_released_{};
  std::array<bool, 256> audit_held_at_step_start_{};
};

}  // namespace atet
