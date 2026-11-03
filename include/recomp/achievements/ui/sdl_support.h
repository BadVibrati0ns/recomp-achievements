#pragma once

// SDL3-backed loaders. Part of recomp::achievements_ui; without
// RECOMP_ACH_SDL3_TARGET they do nothing and return false.

#include "recomp/achievements/ui/icon_cache.h"
#include "recomp/achievements/ui/sounds.h"

namespace recomp::achievements::ui {

// PNG (SDL 3.4+) and BMP decoding through SDL_LoadPNG_IO / SDL_LoadBMP_IO.
bool InstallSdlImageDecoder(IconCache& cache);
// SDL_LoadWAV + SDL_ConvertAudioSamples to 48 kHz stereo float.
bool InstallSdlWavLoader(SoundBank& bank);

}  // namespace recomp::achievements::ui
