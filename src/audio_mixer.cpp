#include "audio_mixer.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // before mikmod_internals.h, which pulls it in for its mutex type
#endif

#include <mikmod.h>
#include <mikmod_internals.h>  // SAMPLOAD, SL_* and md_player/md_bpm/md_numchn: the driver interface

#include <algorithm>
#include <cstring>
#include <string>

namespace atet {
namespace {

// libmikmod drivers carry no user pointer, so the driver entry points reach the open mixer here.
AudioMixer* g_mixer = nullptr;

constexpr int kFracBits = 11;           // 0x80B2: sample index = position >> 11
constexpr int kPortionFrames = 1800;    // 0x7EBE/0x7ED0: tick buffer, 0x708 stereo frames
constexpr int kCallbackFrames = 1024;   // SDL callback render block
constexpr int kSamplePadding = 16;      // as libmikmod's VC1_SampleLoad (interpolation guard)

BOOL DrvIsPresent(void) { return 1; }

SWORD DrvSampleLoad(SAMPLOAD* sload, int type) {
  return g_mixer != nullptr ? static_cast<SWORD>(g_mixer->DriverSampleLoad(sload, type)) : -1;
}

void DrvSampleUnload(SWORD handle) {
  if (g_mixer != nullptr) {
    g_mixer->DriverSampleUnload(handle);
  }
}

ULONG DrvSampleSpace(int) { return 0; }

ULONG DrvSampleLength(int, SAMPLE* s) {
  if (s == nullptr) {
    return 0;
  }
  return (s->length * ((s->flags & SF_16BITS) ? 2 : 1)) + kSamplePadding;
}

int DrvInit(void) { return g_mixer != nullptr ? 0 : 1; }
void DrvExit(void) {}
int DrvSetNumVoices(void) { return g_mixer != nullptr ? g_mixer->DriverSetNumVoices(md_numchn) : 1; }
int DrvPlayStart(void) { return 0; }
void DrvPlayStop(void) {}
void DrvUpdate(void) {}  // the SDL callback (or Render) pulls; nothing to push here

void DrvVoiceSetVolume(UBYTE voice, UWORD vol) {
  // libmikmod hands 0..256; with atet_mm2_compat the player computes the MikMod 2.x value
  // v (0..64) and passes v*4, so v is recovered exactly. 2.x stores v as a byte (0x7FE3).
  if (AudioMixer::Voice* v = g_mixer != nullptr ? g_mixer->MusicVoice(voice) : nullptr) {
    v->vol = std::min(static_cast<int>(vol >> 2), 64);
  }
}

UWORD DrvVoiceGetVolume(UBYTE voice) {
  AudioMixer::Voice* v = g_mixer != nullptr ? g_mixer->MusicVoice(voice) : nullptr;
  return v != nullptr ? static_cast<UWORD>(v->vol << 2) : 0;
}

void DrvVoiceSetFrequency(UBYTE voice, ULONG frq) {
  if (AudioMixer::Voice* v = g_mixer != nullptr ? g_mixer->MusicVoice(voice) : nullptr) {
    v->frq = static_cast<std::uint32_t>(frq);
  }
}

ULONG DrvVoiceGetFrequency(UBYTE voice) {
  AudioMixer::Voice* v = g_mixer != nullptr ? g_mixer->MusicVoice(voice) : nullptr;
  return v != nullptr ? v->frq : 0;
}

void DrvVoiceSetPanning(UBYTE voice, ULONG pan) {
  // 2.x stores the pan as a byte (0x8011). UN05 modules cannot express surround.
  if (AudioMixer::Voice* v = g_mixer != nullptr ? g_mixer->MusicVoice(voice) : nullptr) {
    v->pan = pan == PAN_SURROUND ? PAN_CENTER : static_cast<int>(pan & 0xFF);
  }
}

ULONG DrvVoiceGetPanning(UBYTE voice) {
  AudioMixer::Voice* v = g_mixer != nullptr ? g_mixer->MusicVoice(voice) : nullptr;
  return v != nullptr ? static_cast<ULONG>(v->pan) : PAN_CENTER;
}

void DrvVoicePlay(UBYTE voice, SWORD handle, ULONG start, ULONG size, ULONG reppos, ULONG repend, UWORD flags) {
  AudioMixer::Voice* v = g_mixer != nullptr ? g_mixer->MusicVoice(voice) : nullptr;
  if (v == nullptr) {
    return;
  }
  // VC_VoicePlay 0x8028: no kick when start >= size; a loop end is clamped to the size.
  if (start >= size) {
    return;
  }
  if ((flags & SF_LOOP) && size < repend) {
    repend = size;
  }
  v->handle = handle;
  v->start = static_cast<std::uint32_t>(start);
  v->size = static_cast<std::uint32_t>(size);
  v->reppos = static_cast<std::uint32_t>(reppos);
  v->repend = static_cast<std::uint32_t>(repend);
  v->flags = flags;
  v->kick = true;
}

void DrvVoiceStop(UBYTE voice) {
  if (AudioMixer::Voice* v = g_mixer != nullptr ? g_mixer->MusicVoice(voice) : nullptr) {
    v->active = false;
  }
}

BOOL DrvVoiceStopped(UBYTE voice) {
  AudioMixer::Voice* v = g_mixer != nullptr ? g_mixer->MusicVoice(voice) : nullptr;
  return v == nullptr || !v->active;
}

SLONG DrvVoiceGetPosition(UBYTE voice) {
  AudioMixer::Voice* v = g_mixer != nullptr ? g_mixer->MusicVoice(voice) : nullptr;
  return v != nullptr ? static_cast<SLONG>(v->current >> kFracBits) : 0;
}

ULONG DrvVoiceRealVolume(UBYTE) { return 0; }

MDRIVER g_driver = {
    nullptr,
    "ATET SDL3",
    "MikMod 2.x-compatible mixer on SDL3",
    0,    // HardVoiceLimit
    255,  // SoftVoiceLimit
    "atetsdl",
    nullptr,
    nullptr,
    DrvIsPresent,
    DrvSampleLoad,
    DrvSampleUnload,
    DrvSampleSpace,
    DrvSampleLength,
    DrvInit,
    DrvExit,
    nullptr,
    DrvSetNumVoices,
    DrvPlayStart,
    DrvPlayStop,
    DrvUpdate,
    nullptr,
    DrvVoiceSetVolume,
    DrvVoiceGetVolume,
    DrvVoiceSetFrequency,
    DrvVoiceGetFrequency,
    DrvVoiceSetPanning,
    DrvVoiceGetPanning,
    DrvVoicePlay,
    DrvVoiceStop,
    DrvVoiceStopped,
    DrvVoiceGetPosition,
    DrvVoiceRealVolume,
};

// 0x7848: number of output samples until the position passes `end`, capped at `todo`. The
// division truncates toward zero like the original's idiv. A negative count (which would make
// the original overrun its buffer) is treated as 0, ending the voice.
int PredictSamples(std::int64_t current, std::int64_t end, std::int64_t increment, int todo) {
  if (increment == 0) {
    return todo;
  }
  std::int64_t count = (end - current) / increment;
  std::int64_t position = current + count * increment;
  if (increment < 0) {
    while (position >= end) {
      ++count;
      position += increment;
    }
  } else {
    while (position <= end) {
      ++count;
      position += increment;
    }
  }
  if (count < 0) {
    return 0;
  }
  return static_cast<int>(std::min<std::int64_t>(count, todo));
}

}  // namespace

AudioMixer::~AudioMixer() { Close(); }

MDRIVER* AudioMixer::MikModDriver() { return &g_driver; }

bool AudioMixer::Open(const Config& config, bool open_device, std::string* error_out) {
  Close();
  config_ = config;
  if (config_.mix_rate < 4000 || config_.mix_rate > 65535) {
    config_.mix_rate = 44100;  // 2.x keeps mixfreq in a 16-bit word
  }
  mutex_ = SDL_CreateMutex();
  if (mutex_ == nullptr) {
    if (error_out != nullptr) *error_out = SDL_GetError();
    return false;
  }
  const int channels = config_.stereo ? 2 : 1;
  accumulator_.assign(static_cast<size_t>(kPortionFrames) * 2, 0);
  callback_buffer_.assign(static_cast<size_t>(kCallbackFrames) * 2, 0);
  samples_.clear();
  music_voices_.clear();
  sfx_voices_ = {};
  tick_left_ = 0;  // VC_PlayStart: the first mix call runs a player tick at once
  tick_length_ = 0;
  frames_rendered_ = 0;
  player_ticks_ = 0;
  events_.clear();
  g_mixer = this;
  open_ = true;

  if (open_device) {
    const SDL_AudioSpec spec{SDL_AUDIO_S16, channels, config_.mix_rate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioMixer::StreamCallback, this);
    if (stream_ == nullptr || !SDL_ResumeAudioStreamDevice(stream_)) {
      if (error_out != nullptr) *error_out = SDL_GetError();
      if (stream_ != nullptr) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
      }
      // The mixer stays usable without a device (nothing is heard, nothing advances).
      return false;
    }
  }
  return true;
}

void AudioMixer::CloseDevice() {
  if (stream_ != nullptr) {
    // Not under mutex_: the callback may be waiting for it while SDL holds the stream.
    SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
  }
}

void AudioMixer::Close() {
  CloseDevice();
  if (g_mixer == this) {
    g_mixer = nullptr;
  }
  samples_.clear();
  music_voices_.clear();
  if (mutex_ != nullptr) {
    SDL_DestroyMutex(mutex_);
    mutex_ = nullptr;
  }
  open_ = false;
}

void AudioMixer::set_quality(Quality quality) {
  ScopedLock lock(*this);
  config_.quality = quality;
}

void AudioMixer::Lock() {
  if (mutex_ != nullptr) SDL_LockMutex(mutex_);
}

void AudioMixer::Unlock() {
  if (mutex_ != nullptr) SDL_UnlockMutex(mutex_);
}

int AudioMixer::AllocateSampleSlot() {
  for (size_t i = 0; i < samples_.size(); ++i) {
    if (samples_[i].data.empty()) {
      return static_cast<int>(i);
    }
  }
  if (samples_.size() >= MAXSAMPLEHANDLES) {
    return -1;
  }
  samples_.emplace_back();
  return static_cast<int>(samples_.size()) - 1;
}

int AudioMixer::DriverSampleLoad(void* raw, int type) {
  SAMPLOAD* sload = static_cast<SAMPLOAD*>(raw);
  if (type == MD_HARDWARE || sload == nullptr || sload->sample == nullptr) {
    return -1;
  }
  SAMPLE* s = sload->sample;
  if (s->length > MAX_SAMPLE_SIZE) {
    _mm_errno = MMERR_NOT_A_STREAM;
    return -1;
  }
  const int handle = AllocateSampleSlot();
  if (handle < 0) {
    _mm_errno = MMERR_OUT_OF_HANDLES;
    return -1;
  }
  // Same loop sanity rule as libmikmod's own software mixer.
  if (s->loopend > s->length) s->loopend = s->length;
  if (s->loopstart >= s->loopend) s->flags &= ~SF_LOOP;

  SL_SampleSigned(sload);
  SL_Sample8to16(sload);
  Sample& sample = samples_[static_cast<size_t>(handle)];
  sample.data.assign(static_cast<size_t>(s->length) + kSamplePadding, 0);
  if (SL_Load(sample.data.data(), sload, s->length)) {
    sample = Sample{};
    return -1;
  }
  sample.length = s->length;
  sample.rate = s->speed;
  sample.flags = s->flags;
  sample.from_mikmod = true;
  // Interpolation guard for the clean mixer (never read by the authentic one): the samples that
  // follow the loop end, or silence after a one-shot sample.
  if (s->flags & SF_LOOP) {
    const std::uint32_t looplen = s->loopend - s->loopstart;
    for (std::uint32_t t = 0; t < kSamplePadding && t < looplen; ++t) {
      sample.data[s->loopend + t] =
          (s->flags & SF_BIDI) ? sample.data[(s->loopend - t) - 1] : sample.data[t + s->loopstart];
    }
  }
  return handle;
}

void AudioMixer::DriverSampleUnload(int handle) {
  if (handle >= 0 && handle < static_cast<int>(samples_.size())) {
    samples_[static_cast<size_t>(handle)] = Sample{};
  }
}

int AudioMixer::DriverSetNumVoices(int voices) {
  // Like the software mixers, a new voice count resets every music voice (VC_Init defaults).
  music_voices_.assign(static_cast<size_t>(std::max(voices, 0)), Voice{});
  for (size_t t = 0; t < music_voices_.size(); ++t) {
    music_voices_[t].pan = (t & 1) ? 0 : 255;
  }
  return 0;
}

AudioMixer::Voice* AudioMixer::MusicVoice(int voice) {
  if (voice < 0 || voice >= static_cast<int>(music_voices_.size())) {
    return nullptr;
  }
  return &music_voices_[static_cast<size_t>(voice)];
}

int AudioMixer::LoadWavSample(const std::vector<Uint8>& wav_bytes, std::string* error_out) {
  if (wav_bytes.empty()) {
    if (error_out != nullptr) *error_out = "empty WAV chunk";
    return -1;
  }
  SDL_AudioSpec spec{};
  Uint8* buffer = nullptr;
  Uint32 length = 0;
  SDL_IOStream* io = SDL_IOFromConstMem(wav_bytes.data(), wav_bytes.size());
  if (io == nullptr || !SDL_LoadWAV_IO(io, /*closeio=*/true, &spec, &buffer, &length)) {
    if (error_out != nullptr) *error_out = SDL_GetError();
    return -1;
  }
  std::vector<std::int16_t> pcm;
  if (spec.channels == 1 && spec.format == SDL_AUDIO_U8) {
    pcm.resize(length);
    for (Uint32 i = 0; i < length; ++i) {
      pcm[i] = static_cast<std::int16_t>((static_cast<int>(buffer[i]) - 128) * 256);  // signed 8-bit, as 2.x stores it
    }
  } else {
    const SDL_AudioSpec mono16{SDL_AUDIO_S16, 1, spec.freq};
    Uint8* converted = nullptr;
    int converted_length = 0;
    if (!SDL_ConvertAudioSamples(&spec, buffer, static_cast<int>(length), &mono16, &converted, &converted_length)) {
      SDL_free(buffer);
      if (error_out != nullptr) *error_out = SDL_GetError();
      return -1;
    }
    pcm.assign(reinterpret_cast<const std::int16_t*>(converted),
               reinterpret_cast<const std::int16_t*>(converted) + converted_length / 2);
    SDL_free(converted);
  }
  SDL_free(buffer);

  ScopedLock lock(*this);
  const int handle = AllocateSampleSlot();
  if (handle < 0) {
    if (error_out != nullptr) *error_out = "out of sample handles";
    return -1;
  }
  Sample& sample = samples_[static_cast<size_t>(handle)];
  sample.length = static_cast<std::uint32_t>(pcm.size());
  sample.rate = static_cast<std::uint32_t>(spec.freq);
  sample.flags = 0;
  sample.from_mikmod = false;
  sample.data = std::move(pcm);
  sample.data.resize(static_cast<size_t>(sample.length) + kSamplePadding, 0);
  return handle;
}

void AudioMixer::PlaySfx(int voice_class, int sample_handle, int volume_percent, int pan) {
  // 0x6826: the only early exit is a volume of 0 %.
  if (volume_percent == 0) {
    return;
  }
  ScopedLock lock(*this);
  if (sample_handle < 0 || sample_handle >= static_cast<int>(samples_.size()) ||
      samples_[static_cast<size_t>(sample_handle)].data.empty()) {
    return;  // 0x6870: no sample in the slot
  }
  const Sample& sample = samples_[static_cast<size_t>(sample_handle)];
  Voice& voice = sfx_voices_[static_cast<size_t>(voice_class & 1)];
  // 0x682E-0x6847: V = percent*64/100 (truncating), at most 64.
  voice.vol = std::min(std::max(volume_percent, 0) * 64 / 100, 64);
  voice.pan = pan & 0xFF;  // stored as a byte (0x8011)
  voice.frq = sample.rate;
  // VC_VoicePlay(voice, handle, start 0, size, 0, 0, flags): kicked at the next player tick.
  if (sample.length == 0) {
    return;
  }
  voice.handle = sample_handle;
  voice.start = 0;
  voice.size = sample.length;
  voice.reppos = 0;
  voice.repend = 0;
  voice.flags = sample.flags;
  voice.kick = true;
}

void AudioMixer::BeginTick() {
  // VC_WriteSamples 0x7C35-0x7D59: run the player, reload the tick, then latch every voice.
  if (md_player != nullptr) {
    md_player();
  }
  const std::uint32_t bpm = md_bpm != 0 ? md_bpm : 125;
  tick_length_ = (static_cast<std::uint32_t>(config_.mix_rate) * 125u) / (bpm * 50u);
  if (tick_length_ == 0) {
    tick_length_ = 1;
  }
  tick_left_ = tick_length_;
  ++player_ticks_;
  if (events_.size() < 4096) {
    events_.push_back(Event{frames_rendered_, 0, -1, -1, 0, 0, static_cast<int>(tick_length_)});
  }

  auto latch = [this](Voice& v, int sfx_class) {
    if (v.kick) {
      v.active = true;
      v.current = static_cast<std::int64_t>(v.start) << kFracBits;
      v.kick = false;
      if (sfx_class >= 0 && events_.size() < 4096) {
        events_.push_back(Event{frames_rendered_, 1, sfx_class, v.handle, v.vol, v.pan, 0});
      }
    }
    if (v.frq == 0) {
      v.active = false;
    }
    if (!v.active) {
      return;
    }
    // 0x769E: floor(frq * 2048 / mixfreq); negated while the voice plays backwards.
    v.increment = static_cast<std::int64_t>((static_cast<std::uint64_t>(v.frq) << kFracBits) /
                                            static_cast<std::uint64_t>(config_.mix_rate));
    if (v.flags & SF_REVERSE) {
      v.increment = -v.increment;
    }
    if (config_.stereo) {
      v.lsel = ((255 - v.pan) * v.vol) / 255;  // 0x7CFD-0x7D12
      v.rsel = (v.pan * v.vol) / 255;          // 0x7D14-0x7D26
    } else {
      v.lsel = v.vol;  // 0x7D4A
      v.rsel = 0;
    }
  };
  for (Voice& v : music_voices_) {
    latch(v, -1);
  }
  for (int c = 0; c < kSfxVoiceClasses; ++c) {
    latch(sfx_voices_[static_cast<size_t>(c)], c);
  }
}

void AudioMixer::AddChannel(Voice& v, int frames) {
  const Sample* sample = nullptr;
  if (v.handle >= 0 && v.handle < static_cast<int>(samples_.size()) &&
      !samples_[static_cast<size_t>(v.handle)].data.empty()) {
    sample = &samples_[static_cast<size_t>(v.handle)];
  }
  if (sample == nullptr) {  // 0x799B: sample gone -> the voice stops
    v.current = 0;
    v.active = false;
    return;
  }
  const std::int16_t* data = sample->data.data();
  const std::int64_t data_limit = static_cast<std::int64_t>(sample->data.size()) - 2;
  const std::int64_t idxsize = static_cast<std::int64_t>(v.size) * 2048 - 1;
  const std::int64_t idxlpos = static_cast<std::int64_t>(v.reppos) << kFracBits;
  const std::int64_t idxlend = static_cast<std::int64_t>(v.repend) * 2048 - 1;
  const bool stereo = config_.stereo;
  const int channels = stereo ? 2 : 1;
  const int unit = stereo ? 4 : 2;  // table step value: 0x800000/64 >> 15 (stereo) or >> 16 (mono)
  const bool clean = config_.quality == Quality::kClean;
  std::int32_t* acc = accumulator_.data();
  int todo = frames;

  while (todo > 0) {
    // 0x7882-0x7920: loop and end handling before each mixed run.
    if (!(v.flags & SF_REVERSE)) {
      if (!(v.flags & SF_LOOP)) {
        if (idxsize < v.current) {
          v.current = 0;
          v.active = false;
          return;
        }
      } else if (idxlend < v.current) {
        if (v.flags & SF_BIDI) {
          v.flags |= SF_REVERSE;
          v.increment = -v.increment;
          v.current = idxlend - (v.current - idxlend);
        } else {
          v.current = (v.current - idxlend) + idxlpos;
        }
      }
    } else {
      if (!(v.flags & SF_LOOP)) {
        if (v.current < 0) {
          v.current = 0;
          v.active = false;
          return;
        }
      } else if (v.current < idxlpos) {
        if (v.flags & SF_BIDI) {
          v.current = idxlpos * 2 - v.current;
          v.flags &= ~SF_REVERSE;
          v.increment = -v.increment;
        } else {
          v.current = idxlend - (idxlpos - v.current);
        }
      }
    }
    std::int64_t end = 0;
    if (!(v.flags & SF_REVERSE)) {
      end = (v.flags & SF_LOOP) ? idxlend : idxsize;
    } else {
      end = (v.flags & SF_LOOP) ? idxlpos : 0;
    }
    const int done = PredictSamples(v.current, end, v.increment, todo);
    if (done == 0) {
      v.active = false;
      return;
    }
    // 0x79FC: a voice at volume 0 is not mixed but its position still advances.
    if (v.vol != 0) {
      std::int64_t pos = v.current;
      const int lgain = v.lsel * unit;
      const int rgain = v.rsel * unit;
      for (int k = 0; k < done; ++k) {
        std::int64_t index = pos >> kFracBits;
        if (index < 0) index = 0;
        if (index > data_limit) index = data_limit;
        int s = 0;
        if (!clean) {
          s = data[index] >> 8;  // signed 8-bit, zero-order hold (0x80B2-0x80B7)
        } else {
          const int s0 = data[index];
          const int s1 = data[index + 1];
          const int frac = static_cast<int>(pos & ((1 << kFracBits) - 1));
          s = s0 + (((s1 - s0) * frac) >> kFracBits);  // 16-bit, linear, scaled by 256
        }
        if (stereo) {
          acc[2 * k] += s * lgain;
          acc[2 * k + 1] += s * rgain;
        } else {
          acc[k] += s * lgain;
        }
        pos += v.increment;
      }
    }
    v.current += v.increment * done;
    todo -= done;
    acc += static_cast<size_t>(done) * channels;
  }
}

void AudioMixer::MixPortion(int frames) {
  const int channels = config_.stereo ? 2 : 1;
  std::fill(accumulator_.begin(), accumulator_.begin() + static_cast<std::ptrdiff_t>(frames) * channels, 0);
  for (Voice& v : music_voices_) {
    if (v.active) AddChannel(v, frames);
  }
  for (Voice& v : sfx_voices_) {
    if (v.active) AddChannel(v, frames);
  }
}

size_t AudioMixer::Render(std::int16_t* out, size_t frames) {
  if (!open_ || out == nullptr) {
    return 0;
  }
  ScopedLock lock(*this);
  const int channels = config_.stereo ? 2 : 1;
  const int shift = config_.quality == Quality::kClean ? 8 : 0;
  size_t done = 0;
  while (done < frames) {
    if (tick_left_ == 0) {
      BeginTick();
    }
    const size_t n = std::min<size_t>({static_cast<size_t>(tick_left_), frames - done, static_cast<size_t>(kPortionFrames)});
    MixPortion(static_cast<int>(n));
    // 0x7A71: one saturating clamp for music and effects together (0x7B40-0x7B6C); 8-bit output
    // (0x7B96-0x7BB8) keeps the top byte of the clamped 16-bit value.
    std::int16_t* dst = out + done * channels;
    const bool eight_bit = !config_.sixteen_bit;
    for (size_t i = 0; i < n * channels; ++i) {
      std::int32_t value = std::clamp<std::int32_t>(accumulator_[i] >> shift, -32768, 32767);
      if (eight_bit) {
        value = (value >> 8) * 256;
      }
      dst[i] = static_cast<std::int16_t>(value);
    }
    tick_left_ -= static_cast<std::uint32_t>(n);
    done += n;
    frames_rendered_ += n;
  }
  return frames;
}

void SDLCALL AudioMixer::StreamCallback(void* userdata, SDL_AudioStream* stream, int additional_amount, int) {
  auto* self = static_cast<AudioMixer*>(userdata);
  const int channels = self->config_.stereo ? 2 : 1;
  const int frame_bytes = channels * static_cast<int>(sizeof(std::int16_t));
  int frames = (additional_amount + frame_bytes - 1) / frame_bytes;
  while (frames > 0) {
    const int n = std::min(frames, kCallbackFrames);
    self->Render(self->callback_buffer_.data(), static_cast<size_t>(n));
    SDL_PutAudioStreamData(stream, self->callback_buffer_.data(), n * frame_bytes);
    frames -= n;
  }
}

std::uint64_t AudioMixer::frames_rendered() {
  ScopedLock lock(*this);
  return frames_rendered_;
}

std::uint64_t AudioMixer::player_ticks() {
  ScopedLock lock(*this);
  return player_ticks_;
}

std::vector<AudioMixer::Event> AudioMixer::TakeEvents() {
  ScopedLock lock(*this);
  std::vector<Event> out;
  out.swap(events_);
  return out;
}

std::string AudioMixer::AuditJson() {
  ScopedLock lock(*this);
  std::string json = "{\"quality\":\"";
  json += config_.quality == Quality::kClean ? "clean" : "authentic";
  json += "\",\"rate\":" + std::to_string(config_.mix_rate) + ",\"channels\":" + std::to_string(config_.stereo ? 2 : 1) +
          ",\"bits\":" + std::to_string(config_.sixteen_bit ? 16 : 8) +
          ",\"device\":" + (stream_ != nullptr ? "true" : "false") + ",\"frames\":" + std::to_string(frames_rendered_) +
          ",\"player_ticks\":" + std::to_string(player_ticks_) + ",\"tick_length\":" + std::to_string(tick_length_) +
          ",\"tick_left\":" + std::to_string(tick_left_) + ",\"music_voices\":" + std::to_string(music_voices_.size()) +
          ",\"sfx\":[";
  for (int c = 0; c < kSfxVoiceClasses; ++c) {
    const Voice& v = sfx_voices_[static_cast<size_t>(c)];
    if (c > 0) json += ",";
    json += "{\"class\":" + std::to_string(c) + ",\"active\":" + (v.active ? "true" : "false") +
            ",\"kick\":" + (v.kick ? "true" : "false") + ",\"sample\":" + std::to_string(v.handle) +
            ",\"vol\":" + std::to_string(v.vol) + ",\"pan\":" + std::to_string(v.pan) + "}";
  }
  // Music voices as [active, sample, frequency, volume 0..64, pan], comparable with the original's
  // VC voice array (0x411C3 + 0x28*i: +1 active, +4 handle, +0x16 frequency, +0x1A volume, +0x1B pan).
  json += "],\"music_voices\":[";
  for (size_t i = 0; i < music_voices_.size(); ++i) {
    const Voice& v = music_voices_[i];
    if (i > 0) json += ",";
    json += "[" + std::to_string(v.active ? 1 : 0) + "," + std::to_string(v.handle) + "," + std::to_string(v.frq) + "," +
            std::to_string(v.vol) + "," + std::to_string(v.pan) + "]";
  }
  json += "]}";
  return json;
}

}  // namespace atet
