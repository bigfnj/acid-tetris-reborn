#pragma once

// Audit-only instrumentation for the October 2026 fidelity audit (worktree
// audit/instrument, never committed). Everything here is diagnostics: it must
// not change game decisions. See harness/port/README.md for usage.

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace atet {

// ---------------------------------------------------------------------------
// Per-frame counters filled by hooks inside the demo (reset by main each step).
struct AuditFrameCounters {
  std::uint32_t render_texture_calls = 0;  // SDL_RenderTexture issued by the demo
  std::uint32_t fill_rect_calls = 0;       // SDL_RenderFillRect issued by the demo
  std::uint32_t draw_color_calls = 0;      // SDL_SetRenderDrawColor issued by the demo
  std::uint32_t pointfield_points = 0;     // chunk-7 points actually drawn
  std::uint32_t glyph_row_draws = 0;       // 1-pixel glyph row blits (frontend text)
  std::uint32_t debris_drawn = 0;          // particles drawn this frame
  std::uint32_t debris_active = 0;         // pool size after the last step/render this frame
  std::uint64_t pointfield_ticks = 0;      // perf-counter ticks inside RenderFrontendPointfield
  std::uint64_t frontend_rows_ticks = 0;   // ticks inside RenderFrontendRows
  std::uint64_t debris_render_ticks = 0;   // ticks inside RenderDebrisParticles
  std::uint64_t topout_update_ticks = 0;   // ticks inside UpdateTopOutDissolve (spawn+step)
  std::uint64_t debris_step_ticks = 0;     // ticks inside StepDebrisParticles
  std::uint64_t line_clear_seed_ticks = 0; // ticks inside SeedLineClearDebris
  void Reset() { *this = AuditFrameCounters{}; }
};
extern AuditFrameCounters g_audit;

// Startup sub-phase timings recorded inside MilestoneADemo::Initialize.
struct AuditStartupTimes {
  std::uint64_t data_ticks = 0;      // locate + read + decode ATET.DAT / archive
  std::uint64_t textures_ticks = 0;  // BuildIndexedImage + SDL texture creation/upload
  std::uint64_t tables_ticks = 0;    // font/pointfield/particle tables
  std::uint64_t setup_ticks = 0;     // SETUP.DAT load/persist + initial state
  std::string data_source;           // path that successfully loaded
};
extern AuditStartupTimes g_audit_startup;

// ---------------------------------------------------------------------------
// Minimal streaming JSON writer (compact, comma management automatic).
class JsonOut {
 public:
  JsonOut& BeginObject();
  JsonOut& EndObject();
  JsonOut& BeginArray();
  JsonOut& EndArray();
  JsonOut& Key(std::string_view key);
  JsonOut& Int(long long value);
  JsonOut& UInt(unsigned long long value);
  JsonOut& Bool(bool value);
  JsonOut& Null();
  JsonOut& Str(std::string_view value);
  JsonOut& Double(double value, int decimals = 3);
  JsonOut& Raw(std::string_view json);  // pre-serialized JSON value
  [[nodiscard]] const std::string& str() const { return out_; }
  void Clear() {
    out_.clear();
    stack_.clear();
    after_key_ = false;
  }

 private:
  void Prefix();
  void AppendString(std::string_view value);
  std::string out_;
  std::vector<bool> stack_;
  bool after_key_ = false;
};

// ---------------------------------------------------------------------------
// FNV-1a 64 (same algorithm/basis/prime as docs/specs/gameplay-trace-v1.md).
constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x00000100000001b3ULL;
inline std::uint64_t Fnv1a64Update(std::uint64_t hash, const void* data, size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= kFnvPrime;
  }
  return hash;
}
std::string Hex64(std::uint64_t value);

// ---------------------------------------------------------------------------
// Command-line options.
struct AuditOptions {
  // --dump-frames=<dir> [--dump-range=a:b] [--dump-mode=logical|window]
  std::string dump_dir;
  std::uint64_t dump_from = 0;
  std::uint64_t dump_to = ~0ULL;
  bool dump_window_mode = false;  // false = 320x240 logical target (default)
  // --trace=<file.jsonl> [--trace-rng-values]
  std::string trace_path;
  bool trace_rng_values = false;
  // --dump-audio=<file.wav> [--audio-model=ideal|winmm] [--dump-audio-via-stream]
  std::string audio_path;
  bool audio_model_winmm = false;
  // --dump-audio-via-stream : the capture pulls through an SDL audio stream fed by the device
  // callback (the device's pull semantics on the step clock) instead of calling the mixer directly
  bool audio_via_stream = false;
  // --paced-frames=N : bounded run through the real (paced) loop
  int paced_frames = 0;
  // --fast-forward=N : skip the limiter for the first N steps (reach a scene
  // quickly), then pace normally; perf statistics exclude those steps
  std::uint64_t fast_forward = 0;
  // --perf=<file.csv> : per-iteration timing rows; --perf-summary=<file.json>
  std::string perf_path;
  std::string perf_summary_path;
  // --report-displays : log displays/modes/renderer/vsync at startup
  bool report_displays = false;
  // --vsync=N : call SDL_SetRenderVSync(renderer, N) (measurement only; the
  // shipped port never calls it)
  bool vsync_set = false;
  int vsync = 0;
  // --inject-load=MS[@EVERY] : busy-wait MS milliseconds after Render on every
  // EVERY-th iteration (default every iteration) to test overload behaviour
  double inject_load_ms = 0.0;
  int inject_every = 1;
  // --no-activate : do not activate (focus) the window when shown
  bool no_activate = false;
  // --legacy-scripted-input : key scripted input to the shipped smoke counter
  // (only advances under --smoke-frames) instead of the simulation step
  bool legacy_scripted_input = false;
  // --quiet-log : suppress per-event SDL_Log spam from the shipped code paths
  bool quiet_log = false;
};
AuditOptions ParseAuditOptions(int argc, char** argv, std::string* error_out);

// ---------------------------------------------------------------------------
// 16-bit PCM WAV writer (header patched on Close).
class WavWriter {
 public:
  ~WavWriter();
  bool Open(const std::string& path, int sample_rate, int channels);
  void AppendFrames(const std::int16_t* interleaved, size_t frames);
  bool Close();
  [[nodiscard]] std::uint64_t frames_written() const { return frames_; }

 private:
  std::FILE* file_ = nullptr;
  int rate_ = 0;
  int channels_ = 0;
  std::uint64_t frames_ = 0;
};

// ---------------------------------------------------------------------------
// Offline model of AudioProbe::PlaySfxSlot (0x6817 model): one voice per slot,
// a retrigger restarts that slot (PlayClip clears the slot's SDL stream), gain =
// EngineVolume(percent)/0x40, balance pan identical to PanToChannelGains.
class OfflineSfxMixer {
 public:
  bool Load(const std::array<std::vector<Uint8>, 12>& wav_chunks, int out_rate, std::string* error_out);
  void Trigger(int slot, int volume_percent, int pan);
  // Adds this block's SFX into an interleaved float stereo buffer.
  void MixInto(float* stereo, size_t frames);
  [[nodiscard]] int active_voices() const;

 private:
  struct Voice {
    bool active = false;
    size_t position = 0;
    float left = 0.0f;
    float right = 0.0f;
  };
  std::array<std::vector<float>, 12> clips_{};  // mono float at out_rate
  std::array<Voice, 12> voices_{};
};

// ---------------------------------------------------------------------------
// Process statistics (Windows: GetProcessTimes / GetProcessMemoryInfo).
struct ProcessStats {
  bool valid = false;
  double user_seconds = 0.0;
  double kernel_seconds = 0.0;
  double seconds_since_process_creation = 0.0;
  std::uint64_t working_set = 0;
  std::uint64_t peak_working_set = 0;
  std::uint64_t private_bytes = 0;
  std::uint64_t peak_private_bytes = 0;
};
ProcessStats QueryProcessStats();

// DXGI frame statistics of the direct3d11 renderer's swap chain (flip model):
// which vblank the most recently displayed present reached. valid=false on other
// renderers or when DXGI reports no statistics yet.
struct DxgiPresentStats {
  bool valid = false;
  unsigned last_present_count = 0;     // IDXGISwapChain::GetLastPresentCount
  unsigned present_count = 0;          // DXGI_FRAME_STATISTICS.PresentCount
  unsigned present_refresh_count = 0;  // .PresentRefreshCount (vblank of that present)
  unsigned sync_refresh_count = 0;     // .SyncRefreshCount
  long long sync_qpc = 0;              // .SyncQPCTime (QPC ticks)
};
DxgiPresentStats QueryDxgiPresentStats(SDL_Renderer* renderer);

// Logs every display's current mode, the window's display, and renderer/vsync.
void ReportDisplays(SDL_Window* window, SDL_Renderer* renderer);

// Busy-waits for the given number of milliseconds (used by --inject-load).
void BusyWaitMs(double ms);

}  // namespace atet
