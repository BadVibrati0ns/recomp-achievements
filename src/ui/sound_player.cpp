#include "recomp/achievements/ui/sound_player.h"

#include <algorithm>
#include <mutex>

#include "recomp/achievements/ui/sdl_support.h"

#if RECOMP_ACH_HAS_SDL3
#include <SDL3/SDL.h>
#endif

namespace recomp::achievements::ui {

struct SoundPlayer::Impl {
  SoundBank bank;
  std::mutex mutex;
  float master = 1.0f;
  bool muted = false;
#if RECOMP_ACH_HAS_SDL3
  bool tried_init = false;
  bool we_inited_audio = false;
  SDL_AudioStream* streams[2] = {nullptr, nullptr};  // Unlock, progress.

  // Lazily, so a game that never shows a popup never opens a device.
  SDL_AudioStream* Stream(NotificationKind kind) {
    const int i = kind == NotificationKind::kUnlock ? 0 : 1;
    if (streams[i]) return streams[i];
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
      if (tried_init) return nullptr;
      tried_init = true;
      if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return nullptr;
      we_inited_audio = true;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = kSoundChannels;
    spec.freq = kSoundSampleRate;
    streams[i] = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (streams[i]) SDL_ResumeAudioStreamDevice(streams[i]);
    return streams[i];
  }

  ~Impl() {
    for (auto*& s : streams) {
      if (s) SDL_DestroyAudioStream(s);
      s = nullptr;
    }
    if (we_inited_audio) SDL_QuitSubSystem(SDL_INIT_AUDIO);
  }
#endif
};

SoundPlayer::SoundPlayer() : impl_(std::make_unique<Impl>()) { InstallSdlWavLoader(impl_->bank); }

SoundPlayer::~SoundPlayer() = default;

bool SoundPlayer::available() {
#if RECOMP_ACH_HAS_SDL3
  return true;
#else
  return false;
#endif
}

SoundBank& SoundPlayer::bank() { return impl_->bank; }

void SoundPlayer::Attach(NotificationCenter& center) {
  center.SetSoundHandler([this](const NotificationCenter::SoundRequest& r) { Play(r); });
}

bool SoundPlayer::Play(const NotificationCenter::SoundRequest& request) {
  auto pcm = impl_->bank.ForEvent(request.kind, request.tier, request.sound);
  return pcm && Play(*pcm, request.volume, request.kind);
}

bool SoundPlayer::Play(std::string_view ref, float volume, NotificationKind channel) {
  auto pcm = impl_->bank.Get(ref);
  return pcm && Play(*pcm, volume, channel);
}

bool SoundPlayer::Play(const Pcm& pcm, float volume, NotificationKind channel) {
  std::lock_guard lock(impl_->mutex);
  const float gain = std::clamp(volume, 0.0f, 1.0f) * impl_->master;
  if (impl_->muted || gain <= 0.0f || pcm.samples.empty()) return false;
#if RECOMP_ACH_HAS_SDL3
  SDL_AudioStream* stream = impl_->Stream(channel);
  if (!stream) return false;
  SDL_ClearAudioStream(stream);
  SDL_SetAudioStreamGain(stream, gain);
  return SDL_PutAudioStreamData(stream, pcm.samples.data(),
                                static_cast<int>(pcm.samples.size() * sizeof(float)));
#else
  (void)channel;
  return false;
#endif
}

void SoundPlayer::SetMasterVolume(float volume) {
  std::lock_guard lock(impl_->mutex);
  impl_->master = std::clamp(volume, 0.0f, 1.0f);
}

float SoundPlayer::master_volume() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->master;
}

void SoundPlayer::SetMuted(bool muted) {
  std::lock_guard lock(impl_->mutex);
  impl_->muted = muted;
  if (muted) {
#if RECOMP_ACH_HAS_SDL3
    for (auto* s : impl_->streams)
      if (s) SDL_ClearAudioStream(s);
#endif
  }
}

bool SoundPlayer::muted() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->muted;
}

void SoundPlayer::Stop() {
#if RECOMP_ACH_HAS_SDL3
  std::lock_guard lock(impl_->mutex);
  for (auto* s : impl_->streams)
    if (s) SDL_ClearAudioStream(s);
#endif
}

}  // namespace recomp::achievements::ui
