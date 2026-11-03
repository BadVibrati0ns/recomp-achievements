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

bool FillOverlayInput(OverlayInput& in, const bool* keys, SDL_Gamepad* pad) {
  if (keys) {
    for (int i = 1; i <= 12; ++i) in.f[i] = keys[SDL_SCANCODE_F1 + (i - 1)];
    in.up = keys[SDL_SCANCODE_UP];
    in.down = keys[SDL_SCANCODE_DOWN];
    in.left = keys[SDL_SCANCODE_LEFT];
    in.right = keys[SDL_SCANCODE_RIGHT];
    in.enter = keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER];
    in.space = keys[SDL_SCANCODE_SPACE];
    in.escape = keys[SDL_SCANCODE_ESCAPE];
    in.backspace = keys[SDL_SCANCODE_BACKSPACE];
    in.page_up = keys[SDL_SCANCODE_PAGEUP];
    in.page_down = keys[SDL_SCANCODE_PAGEDOWN];
    in.home = keys[SDL_SCANCODE_HOME];
    in.end = keys[SDL_SCANCODE_END];
    in.tab = keys[SDL_SCANCODE_TAB];
    in.shift = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
  }
  if (pad) {
    auto b = [&](SDL_GamepadButton button) { return SDL_GetGamepadButton(pad, button); };
    auto axis = [&](SDL_GamepadAxis a) { return SDL_GetGamepadAxis(pad, a) / 32767.0f; };
    // SDL names face buttons by position, which is what the hints show.
    in.pad_a = b(SDL_GAMEPAD_BUTTON_SOUTH);
    in.pad_b = b(SDL_GAMEPAD_BUTTON_EAST);
    in.pad_x = b(SDL_GAMEPAD_BUTTON_WEST);
    in.pad_y = b(SDL_GAMEPAD_BUTTON_NORTH);
    in.pad_lb = b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    in.pad_rb = b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    in.pad_start = b(SDL_GAMEPAD_BUTTON_START);
    in.pad_back = b(SDL_GAMEPAD_BUTTON_BACK);
    in.pad_up = b(SDL_GAMEPAD_BUTTON_DPAD_UP);
    in.pad_down = b(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    in.pad_left = b(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    in.pad_right = b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    in.pad_lx = axis(SDL_GAMEPAD_AXIS_LEFTX);
    in.pad_ly = axis(SDL_GAMEPAD_AXIS_LEFTY);
    in.pad_ry = axis(SDL_GAMEPAD_AXIS_RIGHTY);
  }
  return true;
}

ButtonLabels ButtonLabelsFor(SDL_Gamepad* pad) {
  if (!pad) return ButtonLabels::kXbox;
  switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return ButtonLabels::kPlayStation;
    default: return ButtonLabels::kXbox;
  }
}

#else

bool InstallSdlImageDecoder(IconCache&) { return false; }
bool InstallSdlWavLoader(SoundBank&) { return false; }
bool FillOverlayInput(OverlayInput&, const bool*, SDL_Gamepad*) { return false; }
ButtonLabels ButtonLabelsFor(SDL_Gamepad*) { return ButtonLabels::kXbox; }

#endif

}  // namespace recomp::achievements::ui
