#include "audio_probe.h"

#include <algorithm>
#include <string>
#include <vector>

#include "audio_mixer.h"

namespace atet {

namespace {

void SetErrorMessage(std::string* error_out, const char* message) {
  if (error_out == nullptr) {
    return;
  }
  *error_out = message != nullptr ? message : "Unknown audio error.";
}

}  // namespace

AudioProbe::~AudioProbe() { Shutdown(); }

int AudioProbe::EngineVolume(int volume_percent) {
  const int percent = std::clamp(volume_percent, 0, 100);
  return std::min(percent * kEngineVolumeMax / 100, kEngineVolumeMax);  // 0x682E-0x6847
}

bool AudioProbe::Initialize(AudioMixer* mixer, const std::vector<Uint8>& wav_bytes, std::string* error_out) {
  mixer_ = mixer;
  probe_handle_ = -1;
  probe_label_ = "probe";
  if (mixer_ == nullptr || !mixer_->is_open()) {
    SetErrorMessage(error_out, "Audio mixer is not open.");
    return false;
  }
  probe_handle_ = mixer_->LoadWavSample(wav_bytes, error_out);
  if (probe_handle_ < 0) {
    return false;
  }
  if (error_out != nullptr) {
    error_out->clear();
  }
  return true;
}

bool AudioProbe::InitializeSfxSlots(AudioMixer* mixer, const std::array<std::vector<Uint8>, 12>& wav_bytes,
                                    std::string* error_out) {
  mixer_ = mixer;
  sfx_handles_.fill(-1);
  loaded_sfx_slots_ = 0;
  if (mixer_ == nullptr || !mixer_->is_open()) {
    SetErrorMessage(error_out, "Audio mixer is not open.");
    return false;
  }
  for (size_t slot = 0; slot < wav_bytes.size(); ++slot) {
    std::string error;
    const int handle = mixer_->LoadWavSample(wav_bytes[slot], &error);
    if (handle < 0) {
      SetErrorMessage(error_out, ("sfx-slot-" + std::to_string(slot) + ": " + error).c_str());
      sfx_handles_.fill(-1);
      loaded_sfx_slots_ = 0;
      return false;
    }
    sfx_handles_[slot] = handle;
    loaded_sfx_slots_ = static_cast<int>(slot) + 1;
  }
  if (error_out != nullptr) {
    error_out->clear();
  }
  return true;
}

bool AudioProbe::Play(std::string* error_out) {
  // The Space probe has no counterpart in the original (13 call sites of 0x6817, none keyed on
  // Space); it plays the menu blip's chunk at full volume on the class-0 voice.
  if (mixer_ == nullptr || probe_handle_ < 0) {
    SetErrorMessage(error_out, "Audio probe is not initialized.");
    return false;
  }
  mixer_->PlaySfx(0, probe_handle_, 100, kPanCenter);
  if (error_out != nullptr) {
    error_out->clear();
  }
  return true;
}

bool AudioProbe::PlaySfxSlot(int slot, int volume_percent, int pan, int voice_class, std::string* error_out) {
  if (slot < 0 || slot >= static_cast<int>(sfx_handles_.size())) {
    SetErrorMessage(error_out, "SFX slot is out of range.");
    return false;
  }
  if (mixer_ == nullptr || sfx_handles_[static_cast<size_t>(slot)] < 0) {
    SetErrorMessage(error_out, "SFX slots are not loaded.");
    return false;
  }
  mixer_->PlaySfx(voice_class, sfx_handles_[static_cast<size_t>(slot)], volume_percent, pan);
  if (error_out != nullptr) {
    error_out->clear();
  }
  return true;
}

void AudioProbe::Shutdown() {
  // The samples belong to the mixer and are released when it closes.
  probe_handle_ = -1;
  sfx_handles_.fill(-1);
  loaded_sfx_slots_ = 0;
  mixer_ = nullptr;
}

}  // namespace atet
