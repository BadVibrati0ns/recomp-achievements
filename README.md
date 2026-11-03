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
- **Popups:** unlock popups and progress popups (for example "Kickflip 3/14"), with sounds synthesized in the style of each platform. Original console sounds and artwork aren't included. Games and players can override any style or sound with JSON and WAV files. Players can set preferences once for every game, in `%APPDATA%\recomp-achievements` on Windows or `~/.config/recomp-achievements` elsewhere.
- **An achievement without an icon** (most custom ones) uses the game's desktop icon, in the popup and
  in the achievement list. If the game doesn't supply one, a trophy is drawn instead.
- **Saves:** progress is saved for each profile, and older save keys still load.

## Parts

| CMake target | What it does | Needs |
|---|---|---|
| `recomp::achievements` | Registry, JSON sets, triggers, saves | nothing (nlohmann/json is bundled) |
| `recomp::achievements_ui` | Popups, list window, styles, sounds, icons | Dear ImGui and SDL3 targets (each optional) |
| `recomp::achievements_ui_base` | The renderer-agnostic part: popup queue, styles, synthesized sounds, WAV and PNG decoding | nothing |
| `recomp::achievements_xbox` | XDBF / SPA reader, title update search | nothing |
| `recomp::achievements_trophy` | `TROPHY.TRP` / `TROPCONF.SFM` reader, sceNpTrophy backend | nothing |
| `recomp::achievements_memory` | Memory triggers in RetroAchievements' condition syntax | nothing (a subset of rcheevos v11.6.0 is bundled) |
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
| UI, base | `src/ui/style.cpp`, `notification_center.cpp`, `notification_attach.cpp`, `sounds.cpp`, `icon_cache.cpp`, `builtin_decoders.cpp` | none |
| UI, SDL3 | `src/ui/sound_player.cpp`, `sdl_support.cpp` | `RECOMP_ACH_HAS_SDL3=1` (0 builds them as no-ops) |
| UI, ImGui | `src/ui/imgui_renderer.cpp`, `achievement_list.cpp`, `popup_ui.cpp` | none (needs `imgui.h` on the include path) |
| Xbox 360 | `src/sources/xdbf.cpp`, `xam.cpp` | none |
| PS3 | `src/sources/trophy.cpp`, `trophy_context.cpp`, `mini_xml.cpp` | none |
| rexglue | `src/rexglue/rexglue_binding.cpp` | `RECOMP_ACH_REXGLUE_KERNEL_HOOKS=1` with the patch, `0` for stock rexglue |
| memory | `src/memory/*.cpp`, plus the bundled `third_party/rcheevos/src` files (C99, with `third_party/rcheevos/include` and `third_party/rcheevos/src` on the include path) | `RC_DISABLE_LUA` |

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
popup_options.fallback_icon = "assets/icon.png";         // the game's desktop icon, for custom achievements
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

Everything builds offline.

Games that draw their own popups (no Dear ImGui) link `recomp::achievements_ui_base`, feed a
`NotificationCenter` and draw its `Active()` popups. WAV overrides and PNG icons decode without SDL, and
`SetSingleSlot(true)` keeps unlock and progress popups to one on screen at a time.

## License

[0BSD](LICENSE) (Zero-Clause BSD): use it for anything, no attribution needed. Bundled code keeps its
own licence: nlohmann/json (MIT), the rcheevos subset (MIT, `third_party/rcheevos/LICENSE`) and
stb_image (MIT or public domain).

## Credits

- [nlohmann/json](https://github.com/nlohmann/json) (MIT) is bundled in `third_party/`.
- [rcheevos](https://github.com/RetroAchievements/rcheevos) (MIT) evaluates memory conditions; its
  condition evaluator is bundled in `third_party/rcheevos`, without any of its network code.
- [stb_image](https://github.com/nothings/stb) (MIT or public domain) decodes icons without SDL.
- Imported RetroAchievements lists credit their authors in each set file. The text and badge art belong to those authors and to RetroAchievements.
