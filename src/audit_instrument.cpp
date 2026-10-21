#include "audit_instrument.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <dxgi1_2.h>
#endif

namespace atet {

AuditFrameCounters g_audit;
AuditStartupTimes g_audit_startup;

// --------------------------------------------------------------------------- JsonOut
void JsonOut::Prefix() {
  if (after_key_) {
    after_key_ = false;
    return;
  }
  if (!stack_.empty()) {
    if (!stack_.back()) {
      out_ += ',';
    }
    stack_.back() = false;
  }
}

void JsonOut::AppendString(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  out_ += '"';
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"':
        out_ += "\\\"";
        break;
      case '\\':
        out_ += "\\\\";
        break;
      case '\n':
        out_ += "\\n";
        break;
      case '\r':
        out_ += "\\r";
        break;
      case '\t':
        out_ += "\\t";
        break;
      default:
        if (ch < 0x20) {
          out_ += "\\u00";
          out_ += kHex[(ch >> 4) & 0xF];
          out_ += kHex[ch & 0xF];
        } else {
          out_ += static_cast<char>(ch);
        }
        break;
    }
  }
  out_ += '"';
}

JsonOut& JsonOut::BeginObject() {
  Prefix();
  out_ += '{';
  stack_.push_back(true);
  return *this;
}
JsonOut& JsonOut::EndObject() {
  out_ += '}';
  if (!stack_.empty()) stack_.pop_back();
  return *this;
}
JsonOut& JsonOut::BeginArray() {
  Prefix();
  out_ += '[';
  stack_.push_back(true);
  return *this;
}
JsonOut& JsonOut::EndArray() {
  out_ += ']';
  if (!stack_.empty()) stack_.pop_back();
  return *this;
}
JsonOut& JsonOut::Key(std::string_view key) {
  if (!stack_.empty()) {
    if (!stack_.back()) {
      out_ += ',';
    }
    stack_.back() = false;
  }
  AppendString(key);
  out_ += ':';
  after_key_ = true;
  return *this;
}
JsonOut& JsonOut::Int(long long value) {
  Prefix();
  out_ += std::to_string(value);
  return *this;
}
JsonOut& JsonOut::UInt(unsigned long long value) {
  Prefix();
  out_ += std::to_string(value);
  return *this;
}
JsonOut& JsonOut::Bool(bool value) {
  Prefix();
  out_ += value ? "true" : "false";
  return *this;
}
JsonOut& JsonOut::Null() {
  Prefix();
  out_ += "null";
  return *this;
}
JsonOut& JsonOut::Str(std::string_view value) {
  Prefix();
  AppendString(value);
  return *this;
}
JsonOut& JsonOut::Double(double value, int decimals) {
  Prefix();
  if (!std::isfinite(value)) {
    out_ += "null";
    return *this;
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
  out_ += buffer;
  return *this;
}
JsonOut& JsonOut::Raw(std::string_view json) {
  Prefix();
  out_ += json;
  return *this;
}

std::string Hex64(std::uint64_t value) {
  char buffer[17];
  std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
  return buffer;
}

// --------------------------------------------------------------------------- options
namespace {

bool StartsWith(const std::string& text, const char* prefix, std::string* rest) {
  const size_t length = std::strlen(prefix);
  if (text.compare(0, length, prefix) != 0) {
    return false;
  }
  if (rest != nullptr) {
    *rest = text.substr(length);
  }
  return true;
}

bool ParseU64(const std::string& text, std::uint64_t* out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (end != text.c_str() + text.size()) return false;
  *out = value;
  return true;
}

}  // namespace

AuditOptions ParseAuditOptions(int argc, char** argv, std::string* error_out) {
  AuditOptions options;
  auto fail = [&](const std::string& message) {
    if (error_out != nullptr && error_out->empty()) {
      *error_out = message;
    }
  };
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index] == nullptr ? std::string{} : std::string(argv[index]);
    std::string rest;
    if (StartsWith(arg, "--dump-frames=", &rest)) {
      options.dump_dir = rest;
      if (rest.empty()) fail("--dump-frames needs a directory");
    } else if (StartsWith(arg, "--dump-range=", &rest)) {
      const size_t colon = rest.find(':');
      std::uint64_t a = 0;
      std::uint64_t b = 0;
      if (colon == std::string::npos || !ParseU64(rest.substr(0, colon), &a) ||
          !ParseU64(rest.substr(colon + 1), &b) || b < a) {
        fail("--dump-range expects a:b with 0 <= a <= b (simulation steps, inclusive)");
      } else {
        options.dump_from = a;
        options.dump_to = b;
      }
    } else if (StartsWith(arg, "--dump-mode=", &rest)) {
      if (rest == "logical") {
        options.dump_window_mode = false;
      } else if (rest == "window") {
        options.dump_window_mode = true;
      } else {
        fail("--dump-mode expects logical or window");
      }
    } else if (StartsWith(arg, "--trace=", &rest)) {
      options.trace_path = rest;
      if (rest.empty()) fail("--trace needs a file path");
    } else if (arg == "--trace-rng-values") {
      options.trace_rng_values = true;
    } else if (StartsWith(arg, "--dump-audio=", &rest)) {
      options.audio_path = rest;
      if (rest.empty()) fail("--dump-audio needs a file path");
    } else if (StartsWith(arg, "--audio-model=", &rest)) {
      if (rest == "ideal") {
        options.audio_model_winmm = false;
      } else if (rest == "winmm") {
        options.audio_model_winmm = true;
      } else {
        fail("--audio-model expects ideal or winmm");
      }
    } else if (StartsWith(arg, "--paced-frames=", &rest)) {
      std::uint64_t value = 0;
      if (!ParseU64(rest, &value) || value == 0 || value > 100000000ULL) {
        fail("--paced-frames expects a positive integer");
      } else {
        options.paced_frames = static_cast<int>(value);
      }
    } else if (StartsWith(arg, "--fast-forward=", &rest)) {
      if (!ParseU64(rest, &options.fast_forward)) {
        fail("--fast-forward expects a non-negative integer (steps)");
      }
    } else if (StartsWith(arg, "--perf=", &rest)) {
      options.perf_path = rest;
    } else if (StartsWith(arg, "--perf-summary=", &rest)) {
      options.perf_summary_path = rest;
    } else if (arg == "--report-displays") {
      options.report_displays = true;
    } else if (StartsWith(arg, "--vsync=", &rest)) {
      options.vsync_set = true;
      options.vsync = std::atoi(rest.c_str());
    } else if (StartsWith(arg, "--inject-load=", &rest)) {
      const size_t at = rest.find('@');
      options.inject_load_ms = std::atof(rest.substr(0, at).c_str());
      if (at != std::string::npos) {
        options.inject_every = std::max(1, std::atoi(rest.substr(at + 1).c_str()));
      }
    } else if (arg == "--no-activate") {
      options.no_activate = true;
    } else if (arg == "--legacy-scripted-input") {
      options.legacy_scripted_input = true;
    } else if (arg == "--quiet-log") {
      options.quiet_log = true;
    }
  }
  return options;
}

// --------------------------------------------------------------------------- WAV
WavWriter::~WavWriter() { Close(); }

bool WavWriter::Open(const std::string& path, int sample_rate, int channels) {
  Close();
  file_ = std::fopen(path.c_str(), "wb");
  if (file_ == nullptr) {
    return false;
  }
  rate_ = sample_rate;
  channels_ = channels;
  frames_ = 0;
  std::uint8_t header[44] = {};
  std::fwrite(header, 1, sizeof(header), file_);  // patched on Close
  return true;
}

void WavWriter::AppendFrames(const std::int16_t* interleaved, size_t frames) {
  if (file_ == nullptr || frames == 0) {
    return;
  }
  std::fwrite(interleaved, sizeof(std::int16_t) * static_cast<size_t>(channels_), frames, file_);
  frames_ += frames;
}

bool WavWriter::Close() {
  if (file_ == nullptr) {
    return true;
  }
  const std::uint32_t data_bytes = static_cast<std::uint32_t>(frames_ * static_cast<std::uint64_t>(channels_) * 2);
  auto put16 = [](std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
  };
  auto put32 = [](std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  };
  std::uint8_t header[44];
  std::memcpy(header, "RIFF", 4);
  put32(header + 4, 36 + data_bytes);
  std::memcpy(header + 8, "WAVEfmt ", 8);
  put32(header + 16, 16);
  put16(header + 20, 1);  // PCM
  put16(header + 22, static_cast<std::uint16_t>(channels_));
  put32(header + 24, static_cast<std::uint32_t>(rate_));
  put32(header + 28, static_cast<std::uint32_t>(rate_ * channels_ * 2));
  put16(header + 32, static_cast<std::uint16_t>(channels_ * 2));
  put16(header + 34, 16);
  std::memcpy(header + 36, "data", 4);
  put32(header + 40, data_bytes);
  std::fseek(file_, 0, SEEK_SET);
  std::fwrite(header, 1, sizeof(header), file_);
  const bool ok = std::fclose(file_) == 0;
  file_ = nullptr;
  return ok;
}

// --------------------------------------------------------------------------- SFX
bool OfflineSfxMixer::Load(const std::array<std::vector<Uint8>, 12>& wav_chunks, int out_rate,
                           std::string* error_out) {
  for (size_t slot = 0; slot < wav_chunks.size(); ++slot) {
    clips_[slot].clear();
    voices_[slot] = Voice{};
    const std::vector<Uint8>& bytes = wav_chunks[slot];
    if (bytes.empty()) {
      continue;
    }
    SDL_AudioSpec spec{};
    Uint8* buffer = nullptr;
    Uint32 length = 0;
    SDL_IOStream* io = SDL_IOFromConstMem(bytes.data(), bytes.size());
    if (io == nullptr || !SDL_LoadWAV_IO(io, true, &spec, &buffer, &length)) {
      if (error_out != nullptr) *error_out = SDL_GetError();
      return false;
    }
    const SDL_AudioSpec mono{SDL_AUDIO_F32, 1, out_rate};
    Uint8* converted = nullptr;
    int converted_length = 0;
    const bool ok = SDL_ConvertAudioSamples(&spec, buffer, static_cast<int>(length), &mono, &converted,
                                            &converted_length);
    SDL_free(buffer);
    if (!ok) {
      if (error_out != nullptr) *error_out = SDL_GetError();
      return false;
    }
    const size_t count = static_cast<size_t>(converted_length) / sizeof(float);
    clips_[slot].assign(reinterpret_cast<const float*>(converted), reinterpret_cast<const float*>(converted) + count);
    SDL_free(converted);
  }
  return true;
}

void OfflineSfxMixer::Trigger(int slot, int volume_percent, int pan) {
  if (slot < 0 || slot >= static_cast<int>(clips_.size())) {
    return;
  }
  // AudioProbe::EngineVolume + PlaySfxSlot: engine = percent*0x40/100; 0 -> silent no-op.
  const int percent = std::clamp(volume_percent, 0, 100);
  const int engine = percent * 0x40 / 100;
  if (engine <= 0) {
    return;
  }
  const float gain = static_cast<float>(engine) / 64.0f;
  // PanToChannelGains (audio_probe.cpp): balance law around 0x80.
  const int clamped = std::clamp(pan, 0, 255);
  float left = 1.0f;
  float right = 1.0f;
  if (clamped <= 0x80) {
    right = static_cast<float>(clamped) / 128.0f;
  } else {
    left = static_cast<float>(255 - clamped) / 127.0f;
  }
  Voice& voice = voices_[static_cast<size_t>(slot)];
  voice.active = !clips_[static_cast<size_t>(slot)].empty();
  voice.position = 0;
  voice.left = left * gain;
  voice.right = right * gain;
}

void OfflineSfxMixer::MixInto(float* stereo, size_t frames) {
  for (size_t slot = 0; slot < voices_.size(); ++slot) {
    Voice& voice = voices_[slot];
    if (!voice.active) {
      continue;
    }
    const std::vector<float>& clip = clips_[slot];
    const size_t available = clip.size() > voice.position ? clip.size() - voice.position : 0;
    const size_t count = std::min(available, frames);
    for (size_t i = 0; i < count; ++i) {
      const float sample = clip[voice.position + i];
      stereo[i * 2] += sample * voice.left;
      stereo[i * 2 + 1] += sample * voice.right;
    }
    voice.position += count;
    if (voice.position >= clip.size()) {
      voice.active = false;
    }
  }
}

int OfflineSfxMixer::active_voices() const {
  int count = 0;
  for (const Voice& voice : voices_) {
    count += voice.active ? 1 : 0;
  }
  return count;
}

// --------------------------------------------------------------------------- process stats
ProcessStats QueryProcessStats() {
  ProcessStats stats;
#ifdef _WIN32
  FILETIME creation{};
  FILETIME exit_time{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) {
    auto to_u64 = [](const FILETIME& ft) {
      return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    stats.user_seconds = static_cast<double>(to_u64(user)) / 1.0e7;
    stats.kernel_seconds = static_cast<double>(to_u64(kernel)) / 1.0e7;
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    stats.seconds_since_process_creation =
        static_cast<double>(to_u64(now) - to_u64(creation)) / 1.0e7;
    stats.valid = true;
  }
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                           sizeof(counters))) {
    stats.working_set = counters.WorkingSetSize;
    stats.peak_working_set = counters.PeakWorkingSetSize;
    stats.private_bytes = counters.PrivateUsage;
    stats.peak_private_bytes = counters.PeakPagefileUsage;
  }
#endif
  return stats;
}

void ReportDisplays(SDL_Window* window, SDL_Renderer* renderer) {
  int count = 0;
  SDL_DisplayID* displays = SDL_GetDisplays(&count);
  for (int i = 0; displays != nullptr && i < count; ++i) {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(displays[i]);
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(displays[i]);
    SDL_Log("Audit display %d id=%u name='%s' current=%dx%d@%.3fHz (%d/%d) desktop=%dx%d@%.3fHz scale=%.2f",
            i, displays[i], SDL_GetDisplayName(displays[i]), mode ? mode->w : 0, mode ? mode->h : 0,
            mode ? mode->refresh_rate : 0.0f, mode ? mode->refresh_rate_numerator : 0,
            mode ? mode->refresh_rate_denominator : 0, desktop ? desktop->w : 0, desktop ? desktop->h : 0,
            desktop ? desktop->refresh_rate : 0.0f, mode ? mode->pixel_density : 0.0f);
  }
  SDL_free(displays);
  if (window != nullptr) {
    const SDL_DisplayID window_display = SDL_GetDisplayForWindow(window);
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    SDL_Log("Audit window display id=%u pixels=%dx%d video_driver=%s", window_display, w, h,
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
  }
  if (renderer != nullptr) {
    int vsync = -99;
    SDL_GetRenderVSync(renderer, &vsync);
    int ow = 0;
    int oh = 0;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    SDL_Log("Audit renderer name=%s vsync=%d output=%dx%d", SDL_GetRendererName(renderer), vsync, ow, oh);
  }
  SDL_Log("Audit audio driver=%s", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
}

DxgiPresentStats QueryDxgiPresentStats(SDL_Renderer* renderer) {
  DxgiPresentStats stats;
#ifdef _WIN32
  if (renderer == nullptr) {
    return stats;
  }
  auto* swap_chain = static_cast<IDXGISwapChain1*>(
      SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_D3D11_SWAPCHAIN_POINTER, nullptr));
  if (swap_chain == nullptr) {
    return stats;
  }
  UINT last = 0;
  if (SUCCEEDED(swap_chain->GetLastPresentCount(&last))) {
    stats.last_present_count = last;
  }
  DXGI_FRAME_STATISTICS frame{};
  if (SUCCEEDED(swap_chain->GetFrameStatistics(&frame))) {
    stats.valid = true;
    stats.present_count = frame.PresentCount;
    stats.present_refresh_count = frame.PresentRefreshCount;
    stats.sync_refresh_count = frame.SyncRefreshCount;
    stats.sync_qpc = frame.SyncQPCTime.QuadPart;
  }
#else
  (void)renderer;
#endif
  return stats;
}

void BusyWaitMs(double ms) {
  if (ms <= 0.0) {
    return;
  }
  const Uint64 freq = SDL_GetPerformanceFrequency();
  const Uint64 start = SDL_GetPerformanceCounter();
  const Uint64 ticks = static_cast<Uint64>(ms * static_cast<double>(freq) / 1000.0);
  while (SDL_GetPerformanceCounter() - start < ticks) {
  }
}

}  // namespace atet
