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

class MilestoneADemo {
 public:
  MilestoneADemo() = default;
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
  // Disable the gameplay<->frontend/boot screen fades (used by --no-fade and by
  // the --live-demo/--topout-demo shortcuts for frame-deterministic smoke runs).
  void set_fades_enabled(bool enabled) { fades_enabled_ = enabled; }
  // 0..255 black-overlay alpha for the active screen fade (scene transition or
  // splash); the caller composites it over the rendered frame.
  [[nodiscard]] int screen_fade_alpha() const;

 private:
  enum class Scene {
    kSplash,
    kFrontend,
    kGameplay,
    kTopOut,
  };

  // Screen fade-through-black state (declared before the methods/members that
  // use it). See the fade members and helpers below.
  enum class ScreenFade { kNone, kOut, kIn };
  enum class PendingScene { kNone, kFrontendFromGameplay, kGameplayNewGame, kGameplayResume };

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

  struct FrontendPointfieldState {
    std::int64_t origin_x = 0;
    std::int64_t origin_y = 0x1000000;
    std::int64_t origin_z = 0;
    int phase_x = 0x200;
    int phase_y = 0x200;
    int phase_z = 0x200;
    int angle_xy = 0;
    int angle_z = 0;
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
  void RenderFrontendRows(SDL_Renderer* renderer, const std::vector<std::string>& rows, int selected_row) const;
  void RenderFrontendText(SDL_Renderer* renderer, const std::string& text, int center_x, int y, int reveal_amount) const;
  void AdvanceFrontendPointfield();
  void AdvanceFrontendTimers();
  void AdvanceSplashTimers();
  void AdvanceTopOutTimers();
  void ActivateFrontendSelection();
  void MoveFrontendSelection(int delta);
  void SetFrontendView(FrontendView view, int selected_row);
  void BeginStartupSplashes();
  // Screen-fade transition helpers: BeginSceneFade starts a fade-out (or
  // switches instantly when fades are disabled); AdvanceScreenFade steps the
  // fade each tick and performs the pending scene switch at black.
  void BeginSceneFade(PendingScene target);
  void AdvanceScreenFade();
  void CompleteSceneSwitch();
  void CompleteStartupSplashes();
  void TrackKeyDown(SDL_Keycode key);
  void TrackKeyUp(SDL_Keycode key);
  void CaptureKeyboardBinding(std::uint8_t original_scancode);
  bool ApplyFrontendHorizontalEdit(int direction, bool allow_toggle_rows);
  bool ApplyGameplayBinding(std::uint8_t original_scancode, bool allow_down);
  bool HandleGameplayBinding(SDL_Keycode key);
  bool HandleHighScoreNameEntry(SDL_Keycode key);
  void CommitHighScoreNameEntry();
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
  void BeginHighScoreConcealToMain();
  void ResetSetupStateToDefaults();
  void BeginHighScoreNameEntry(int score, int lines);
  [[nodiscard]] std::vector<std::string> CurrentFrontendRows() const;
  [[nodiscard]] std::string KeyboardBindingRowText(int row_index) const;
  [[nodiscard]] std::string HighScoreRowText(int row_index) const;
  [[nodiscard]] std::string SoundSetupRowText(int row_index) const;
  [[nodiscard]] int MeasureFrontendText(const std::string& text) const;
  [[nodiscard]] int PulseRevealAmount() const;
  [[nodiscard]] int FrontendRowRevealDelayFrames() const;
  [[nodiscard]] int HighScoreBootstrapRemainingFrames() const;
  [[nodiscard]] int RevealedFrontendRowCount(size_t row_count) const;
  [[nodiscard]] int VisibleFrontendRowCount(size_t row_count) const;
  [[nodiscard]] bool HighScoreEntryInputReady() const;
  [[nodiscard]] int SelectableFrontendRowCount() const;

  Scene scene_ = Scene::kFrontend;
  FrontendView frontend_view_ = FrontendView::kMainMenu;
  int frontend_selected_row_ = 0;
  int gameplay_preset_index_ = 0;
  int music_track_index_ = 0;
  int start_level_ = 0;
  int music_volume_ = 100;
  int sfx_volume_ = 90;
  int credits_page_ = 0;
  int credits_page_timer_ = 0;
  int keyboard_capture_row_ = -1;
  int sound_device_index_ = 0;
  int mixing_rate_hz_ = 44100;
  int stereo_flag_ = 1;
  int bit_depth_flag_ = 1;
  std::array<std::uint8_t, 5> keyboard_bindings_ = {0xD0, 0xCB, 0xCD, 0x1E, 0x1F};
  std::array<bool, 256> held_original_scancodes_{};
  std::array<int, 256> held_original_scancode_frames_{};
  std::array<HighScoreRecord, 5> high_scores_{};
  int high_score_entry_row_ = -1;
  std::string high_score_entry_name_;
  bool high_score_entry_lowercase_ = false;
  bool high_score_entry_commit_pending_ = false;
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
  int high_score_bootstrap_frames_ = 0;
  int high_score_conceal_frames_ = 0;
  bool debug_state_logging_ = false;
  std::vector<AudioEvent> audio_events_;
  int line_clear_demo_count_ = 0;

  // Screen fade-through-black for gameplay<->frontend transitions and boot
  // splashes (RE 0x3830: palette fade-out 0x64f0 / fade-in 0x6498, 0x40 ticks
  // each). fades_enabled_ is cleared by --no-fade and the demo shortcuts.
  bool fades_enabled_ = true;
  ScreenFade screen_fade_ = ScreenFade::kNone;
  int screen_fade_frame_ = 0;
  PendingScene pending_scene_ = PendingScene::kNone;

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
  std::array<int, 2048> sine_table_{};
  FrontendPointfieldState frontend_pointfield_state_;
  int frontend_current_bank_ = 2;
  int frontend_next_bank_ = 0;
  int frontend_cycle_counter_ = 0;
  int frontend_frame_counter_ = 0;
  int frontend_view_frame_ = 0;
  int splash_page_ = 0;
  int splash_frame_ = 0;
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
};

}  // namespace atet
