#include "audio_probe.h"

#include <algorithm>
#include <string>
#include <vector>

namespace atet {

namespace {

void SetErrorMessage(std::string* error_out, const char* message) {
  if (error_out == nullptr) {
    return;
  }

  *error_out = message != nullptr ? message : "Unknown SDL audio error.";
}

// Balance-style pan: kPanCenter keeps both channels at full gain (so centered
// SFX are unchanged), and offsets attenuate the opposite channel toward zero.
void PanToChannelGains(int pan, float* left, float* right) {
  const int clamped = std::clamp(pan, 0, 255);
  if (clamped <= AudioProbe::kPanCenter) {
    *left = 1.0f;
    *right = static_cast<float>(clamped) / static_cast<float>(AudioProbe::kPanCenter);
  } else {
    *right = 1.0f;
    *left = static_cast<float>(255 - clamped) / static_cast<float>(255 - AudioProbe::kPanCenter);
  }
}

}  // namespace

AudioProbe::~AudioProbe() {
  Shutdown();
}

int AudioProbe::EngineVolume(int volume_percent) {
  const int percent = std::clamp(volume_percent, 0, 100);
  return percent * kEngineVolumeMax / 100;  // 0x6817 scales percent into 0..0x40
}

bool AudioProbe::Initialize(const std::vector<Uint8>& wav_bytes, std::string* error_out) {
  ShutdownClip(&probe_clip_);

  if (!LoadClip(wav_bytes, "probe", &probe_clip_, error_out)) {
    return false;
  }
  if (error_out != nullptr) {
    error_out->clear();
  }

  return true;
}

bool AudioProbe::InitializeSfxSlots(const std::array<std::vector<Uint8>, 12>& wav_bytes, std::string* error_out) {
  ShutdownSfxSlots();

  for (size_t slot = 0; slot < wav_bytes.size(); ++slot) {
    if (!LoadClip(wav_bytes[slot], "sfx-slot-" + std::to_string(slot), &sfx_slots_[slot], error_out)) {
      ShutdownSfxSlots();
      return false;
    }
    loaded_sfx_slots_ = static_cast<int>(slot) + 1;
  }

  if (error_out != nullptr) {
    error_out->clear();
  }
  return true;
}

bool AudioProbe::Play(std::string* error_out) {
  return PlayClip(&probe_clip_, 1.0f, kPanCenter, error_out);
}

bool AudioProbe::PlaySfxSlot(int slot, int volume_percent, int pan, std::string* error_out) {
  if (slot < 0 || slot >= static_cast<int>(sfx_slots_.size())) {
    SetErrorMessage(error_out, "SFX slot is out of range.");
    return false;
  }

  const int engine_volume = EngineVolume(volume_percent);
  if (engine_volume <= 0) {
    if (error_out != nullptr) {
      error_out->clear();
    }
    return true;
  }

  const float gain = static_cast<float>(engine_volume) / static_cast<float>(kEngineVolumeMax);
  return PlayClip(&sfx_slots_[static_cast<size_t>(slot)], gain, pan, error_out);
}

void AudioProbe::Shutdown() {
  ShutdownClip(&probe_clip_);
  ShutdownSfxSlots();
}

bool AudioProbe::ready() const {
  return probe_clip_.ready();
}

const std::string& AudioProbe::wav_path() const {
  return probe_clip_.path;
}

bool AudioProbe::LoadClip(const std::vector<Uint8>& wav_bytes, std::string_view label, Clip* clip,
                          std::string* error_out) {
  if (clip == nullptr) {
    SetErrorMessage(error_out, "Audio clip storage is unavailable.");
    return false;
  }

  ShutdownClip(clip);
  clip->path = std::string(label);

  if (wav_bytes.empty()) {
    SetErrorMessage(error_out, ("WAV chunk for " + clip->path + " is empty.").c_str());
    return false;
  }

  SDL_AudioSpec file_spec{};
  Uint8* file_buffer = nullptr;
  Uint32 file_length = 0;
  SDL_IOStream* io = SDL_IOFromConstMem(wav_bytes.data(), wav_bytes.size());
  if (io == nullptr || !SDL_LoadWAV_IO(io, /*closeio=*/true, &file_spec, &file_buffer, &file_length)) {
    SetErrorMessage(error_out, SDL_GetError());
    return false;
  }

  // Decode to normalized mono float so pan can be rendered as left/right gains.
  const SDL_AudioSpec mono_spec{SDL_AUDIO_F32, 1, file_spec.freq};
  Uint8* mono_buffer = nullptr;
  int mono_length = 0;
  const bool converted = SDL_ConvertAudioSamples(
      &file_spec, file_buffer, static_cast<int>(file_length), &mono_spec, &mono_buffer, &mono_length);
  SDL_free(file_buffer);
  if (!converted) {
    SetErrorMessage(error_out, SDL_GetError());
    return false;
  }

  const int frame_count = mono_length / static_cast<int>(sizeof(float));
  clip->mono.assign(
      reinterpret_cast<const float*>(mono_buffer),
      reinterpret_cast<const float*>(mono_buffer) + frame_count);
  clip->freq = file_spec.freq;
  SDL_free(mono_buffer);

  // The stream is fed interleaved F32 stereo (pan already baked into the data);
  // SDL converts from here to the device format.
  const SDL_AudioSpec stereo_spec{SDL_AUDIO_F32, 2, clip->freq};
  clip->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &stereo_spec, nullptr, nullptr);
  if (clip->stream == nullptr) {
    SetErrorMessage(error_out, SDL_GetError());
    clip->mono.clear();
    return false;
  }

  if (!SDL_ResumeAudioStreamDevice(clip->stream)) {
    SetErrorMessage(error_out, SDL_GetError());
    ShutdownClip(clip);
    return false;
  }

  return true;
}

bool AudioProbe::PlayClip(Clip* clip, float gain, int pan, std::string* error_out) {
  if (clip == nullptr || !clip->ready()) {
    SetErrorMessage(error_out, "Audio probe is not initialized.");
    return false;
  }

  float left = 1.0f;
  float right = 1.0f;
  PanToChannelGains(pan, &left, &right);
  left *= gain;
  right *= gain;

  // Bake gain + pan into an interleaved stereo buffer so a single scalar stream
  // gain is not enough to lose the left/right balance.
  std::vector<float> stereo(clip->mono.size() * 2);
  for (size_t i = 0; i < clip->mono.size(); ++i) {
    const float sample = clip->mono[i];
    stereo[i * 2] = sample * left;
    stereo[i * 2 + 1] = sample * right;
  }

  if (!SDL_ClearAudioStream(clip->stream)) {
    SetErrorMessage(error_out, SDL_GetError());
    return false;
  }

  if (!SDL_PutAudioStreamData(
          clip->stream, stereo.data(), static_cast<int>(stereo.size() * sizeof(float)))) {
    SetErrorMessage(error_out, SDL_GetError());
    return false;
  }

  if (!SDL_FlushAudioStream(clip->stream)) {
    SetErrorMessage(error_out, SDL_GetError());
    return false;
  }

  if (error_out != nullptr) {
    error_out->clear();
  }
  return true;
}

void AudioProbe::ShutdownClip(Clip* clip) {
  if (clip == nullptr) {
    return;
  }

  if (clip->stream != nullptr) {
    SDL_DestroyAudioStream(clip->stream);
    clip->stream = nullptr;
  }

  clip->mono.clear();
  clip->mono.shrink_to_fit();
  clip->freq = 0;
  clip->path.clear();
}

void AudioProbe::ShutdownSfxSlots() {
  for (Clip& clip : sfx_slots_) {
    ShutdownClip(&clip);
  }
  loaded_sfx_slots_ = 0;
}

}  // namespace atet
