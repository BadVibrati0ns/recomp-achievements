#include "recomp/achievements/ui/sdl_support.h"

#if RECOMP_ACH_HAS_SDL3
#include <SDL3/SDL.h>

#include <cstring>
#endif

namespace recomp::achievements::ui {

#if RECOMP_ACH_HAS_SDL3

namespace {

bool DecodeWithSdl(std::span<const uint8_t> bytes, Image& out) {
  static constexpr uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  SDL_IOStream* io = SDL_IOFromConstMem(bytes.data(), bytes.size());
  if (!io) return false;
  SDL_Surface* surface = nullptr;
  if (bytes.size() >= 8 && std::memcmp(bytes.data(), kPng, 8) == 0) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
    surface = SDL_LoadPNG_IO(io, true);
#else
    SDL_CloseIO(io);  // SDL before 3.4 has no PNG loader; supply your own decoder.
    return false;
#endif
  } else {
    surface = SDL_LoadBMP_IO(io, true);
  }
  if (!surface) return false;
  SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(surface);
  if (!rgba) return false;
  out.width = rgba->w;
  out.height = rgba->h;
  out.rgba.resize(static_cast<size_t>(rgba->w) * rgba->h * 4);
  const bool locked = SDL_MUSTLOCK(rgba) ? SDL_LockSurface(rgba) : true;
  if (locked) {
    for (int y = 0; y < rgba->h; ++y) {
      std::memcpy(out.rgba.data() + static_cast<size_t>(y) * rgba->w * 4,
                  static_cast<const uint8_t*>(rgba->pixels) + static_cast<size_t>(y) * rgba->pitch,
                  static_cast<size_t>(rgba->w) * 4);
    }
    if (SDL_MUSTLOCK(rgba)) SDL_UnlockSurface(rgba);
  }
  SDL_DestroySurface(rgba);
  return locked;
}

bool LoadWavWithSdl(const std::filesystem::path& path, Pcm& out) {
  SDL_AudioSpec spec{};
  Uint8* buf = nullptr;
  Uint32 len = 0;
  const std::u8string utf8 = path.u8string();  // SDL wants UTF-8 on every platform.
  if (!SDL_LoadWAV(reinterpret_cast<const char*>(utf8.c_str()), &spec, &buf, &len)) return false;
  SDL_AudioSpec target{};
  target.format = SDL_AUDIO_F32;
  target.channels = kSoundChannels;
  target.freq = kSoundSampleRate;
  Uint8* dst = nullptr;
  int dst_len = 0;
  const bool ok = SDL_ConvertAudioSamples(&spec, buf, static_cast<int>(len), &target, &dst, &dst_len);
  SDL_free(buf);
  if (!ok || !dst) return false;
  out.samples.resize(static_cast<size_t>(dst_len) / sizeof(float));
  std::memcpy(out.samples.data(), dst, out.samples.size() * sizeof(float));
  SDL_free(dst);
  return !out.samples.empty();
}

}  // namespace

bool InstallSdlImageDecoder(IconCache& cache) {
  cache.SetDecoder(DecodeWithSdl);
  return true;
}

bool InstallSdlWavLoader(SoundBank& bank) {
  bank.SetWavLoader(LoadWavWithSdl);
  return true;
}

#else

bool InstallSdlImageDecoder(IconCache&) { return false; }
bool InstallSdlWavLoader(SoundBank&) { return false; }

#endif

}  // namespace recomp::achievements::ui
