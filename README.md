# recomp-achievements

Achievements, trophies and popups for static recompilations of console games.
One system for every recomp, whether the game shipped with Xbox 360
achievements or PS3 trophies, has a public RetroAchievements list, or adds a
list of its own.

- **Detects the game's own list:**
  - Xbox 360 achievements come from the title's XDBF data and the newest title update's `spa.bin`.
  - PS3 trophies come from a decrypted `TROPHY.TRP`.
  - Unlocks the game makes itself are hooked: XAM on rexglue, sceNpTrophy calls on PS3.
- **Copies public RetroAchievements lists:** names, descriptions, points, badges, and credit to the original authors. The recomp never connects to RetroAchievements, which doesn't support recomps. Each recomp adds its own unlock conditions, and can write them in RetroAchievements' condition syntax to be checked against guest memory.
- **Custom achievements, JSON first:** a game or a mod writes its own list in a hand-writable JSON format, with built-in triggers for stats, events, checklists and counts, plus types the game defines.
- **Popups:** unlock popups and progress popups (for example "Kickflip 3/14"), with sounds synthesized in the style of each platform. Original console sounds and artwork aren't included. Games and players can override any style or sound with JSON and WAV files.
- **Saves:** progress is saved for each profile, and older save keys still load.

## Parts

| CMake target | What it does | Needs |
|---|---|---|
| `recomp::achievements` | Registry, JSON sets, triggers, saves | nothing (nlohmann/json is bundled) |
| `recomp::achievements_ui` | Popups, list window, styles, sounds, icons | Dear ImGui and SDL3 targets (each optional) |
| `recomp::achievements_xbox` | XDBF / SPA reader, title update search | nothing |
| `recomp::achievements_trophy` | `TROPHY.TRP` / `TROPCONF.SFM` reader, sceNpTrophy backend | nothing |
| `recomp::achievements_memory` | Memory triggers in RetroAchievements' condition syntax | rcheevos v11.6.0 (fetched; evaluation code only) |
| `recomp::achievements_rexglue` | Binding for rexglue (Xbox 360) recomps | rexglue with `patches/rexglue-achievements.patch` |

```cmake
set(RECOMP_ACH_IMGUI_TARGET imgui)      # optional
set(RECOMP_ACH_SDL3_TARGET SDL3::SDL3)  # optional
set(RECOMP_ACH_REXGLUE_TARGET rex::runtime)  # rexglue recomps only
add_subdirectory(third_party/recomp-achievements)
target_link_libraries(my_game PRIVATE recomp::achievements recomp::achievements_ui)
```

### Without CMake

Every part is plain C++20 (the rexglue binding is C++23), with `include/` and `third_party/` on the include
path. Compile these sources and set these defines:

| Part | Sources | Defines |
|---|---|---|
| core | `src/system.cpp`, `src/json_format.cpp` | none |
| UI, base | `src/ui/style.cpp`, `notification_center.cpp`, `notification_attach.cpp`, `sounds.cpp`, `icon_cache.cpp` | none |
| UI, SDL3 | `src/ui/sound_player.cpp`, `sdl_support.cpp` | `RECOMP_ACH_HAS_SDL3=1` (0 builds them as no-ops) |
| UI, ImGui | `src/ui/imgui_renderer.cpp`, `achievement_list.cpp`, `popup_ui.cpp` | none (needs `imgui.h` on the include path) |
| Xbox 360 | `src/sources/xdbf.cpp`, `xam.cpp` | none |
| PS3 | `src/sources/trophy.cpp`, `trophy_context.cpp`, `mini_xml.cpp` | none |
| rexglue | `src/rexglue/rexglue_binding.cpp` | `RECOMP_ACH_REXGLUE_KERNEL_HOOKS=1` with the patch, `0` for stock rexglue |
| memory | `src/memory/*.cpp`, plus rcheevos' `alloc`, `condition`, `condset`, `memref`, `operand`, `trigger`, `value`, `rc_compat` and `rc_util` | `RC_DISABLE_LUA` |

CMake also defines `RECOMP_ACH_HAS_IMGUI` for consumers, but no SDK source reads it.

## Quick start

```cpp
#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/popup_ui.h"
using namespace recomp::achievements;

System achievements;
achievements.SetStorage(user_dir / "profiles", [] { return CurrentProfileName(); });
achievements.LoadSetFile("assets/my_game.achievements.json");
achievements.LoadDirectory(user_dir / "mods");   // mod packs with their own lists

ui::PopupUiOptions popup_options;
popup_options.user_dir = user_dir;                       // achievements_style.json, sounds/*.wav
popup_options.create_texture = UploadRgbaTexture;        // your renderer
ui::PopupUi popups(achievements, popup_options);

// In game code:
achievements.ReportStat("grind_distance", meters);
achievements.ReportEvent("trick_landed", "Kickflip");
achievements.Unlock("my_game:secret_ending");

// Every frame, inside the ImGui frame:
popups.Draw(&show_achievement_list);
```

A minimal set file:

```json
{
  "schema": "recomp-achievements/1",
  "set": { "id": "my_game", "title": "My Game", "credits": ["Your Name"] },
  "achievements": [
    { "id": "long_grind", "name": "Rail Rider", "description": "Grind 100 m in one go",
      "points": 20, "trigger": { "stat": "grind_distance", "threshold": 100 } },
    { "id": "flip_book", "name": "Flip Book", "description": "Land every flip trick",
      "points": 30, "trigger": { "type": "checklist", "event": "trick_landed" },
      "checklist": ["Kickflip", "Heelflip", "Varial Kickflip", "360 Flip"] }
  ]
}
```

## Guides

- [JSON set format](docs/json-format.md) ([schema](schema/achievement-set.schema.json))
- [Popup styles and sounds](docs/styling.md)
- [Xbox 360 / rexglue](docs/xbox360.md)
- [PS3 trophies](docs/ps3.md)
- [RetroAchievements lists](docs/retroachievements.md) and the [import tool](tools/README.md)
- [Memory triggers](docs/memory-triggers.md)

## Building and testing

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
python3 -m unittest discover -s tools/tests
```

The rcheevos fetch needs network access the first time; turn it off with
`-DRECOMP_ACH_MEMORY=OFF`.

## Credits

- [nlohmann/json](https://github.com/nlohmann/json) (MIT) is bundled in `third_party/`.
- [rcheevos](https://github.com/RetroAchievements/rcheevos) (MIT) evaluates memory conditions.
- Imported RetroAchievements lists credit their authors in each set file. The text and badge art belong to those authors and to RetroAchievements.
