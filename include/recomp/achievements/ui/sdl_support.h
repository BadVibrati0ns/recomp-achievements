#pragma once

// SDL3-backed loaders and overlay input. Part of recomp::achievements_ui;
// without RECOMP_ACH_SDL3_TARGET they do nothing and return false.

#include "recomp/achievements/ui/icon_cache.h"
#include "recomp/achievements/ui/overlay.h"
#include "recomp/achievements/ui/sounds.h"

struct SDL_Gamepad;

namespace recomp::achievements::ui {

// PNG (SDL 3.4+) and BMP decoding through SDL_LoadPNG_IO / SDL_LoadBMP_IO.
bool InstallSdlImageDecoder(IconCache& cache);
// SDL_LoadWAV + SDL_ConvertAudioSamples to 48 kHz stereo float.
bool InstallSdlWavLoader(SoundBank& bank);

// Sets the keyboard and gamepad fields of in from SDL3 state: keys is
// SDL_GetKeyboardState(nullptr) (may be null), pad may be null. Mouse fields
// are left alone. Returns false without SDL3 (in is untouched).
bool FillOverlayInput(OverlayInput& in, const bool* keys, SDL_Gamepad* pad);
// PlayStation labels for PS3/PS4/PS5 pads, Xbox labels for everything else.
ButtonLabels ButtonLabelsFor(SDL_Gamepad* pad);

}  // namespace recomp::achievements::ui
