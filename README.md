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
- **An achievement viewer for any renderer:** `ui::Overlay` draws the achievement list and the popups in
  software into one image the game blends over its frame, so games without Dear ImGui (SDL2, a PSP
  recomp's Vulkan shim, ...) get the same UI. F8 or Start + LB opens it. Its last tab, All games,
  shows every game in the library and can start another one, quitting this one or leaving it running.
- **An achievement without an icon** (most custom ones) uses the game's desktop icon, in the popup and
  in the achievement list. If the game doesn't supply one, a trophy is drawn instead.
- **Saves:** progress is saved for each profile, and older save keys still load.
- **One library for every game:** each game keeps its sets, icons and progress, plus an append-only
  event log, in its own folder beside its saves, and a shared library folder next to the player's
  preferences records where each game is. A desktop app can then show every recomp's achievements
  without reading any game's data files. Copying the entry into the shared folder is an option. A Windows build under Proton
  writes to the Linux host's folder. See [docs/library.md](docs/library.md).
- **Launching games:** the app starts any game in the library through Steam or directly, and
  remembers the choice for each game. It finds a Steam shortcut added after the game first ran, and
  won't start a second copy of a game that is already running. See [docs/launching.md](docs/launching.md).
- **Player settings, in game:** the viewer's settings page turns each achievement set on and off (a set
  that's off hides its points, can't unlock and keeps its progress; sets can start off), resets chosen
  achievements, a set or everything behind a hold-to-confirm, and sets the popup sound and volume, the
  theme, and the size of the popups and of the list. The
  choices are saved for the game in `achievement_settings.json` next to the saves, so a game only passes
  its storage folder. `SplitSet()` moves achievements a recomp can't support (online-only ones) into an
  optional set of their own.

## Parts

| CMake target | What it does | Needs |
|---|---|---|
| `recomp::achievements` | Registry, JSON sets, triggers, saves, the shared achievement library (writer and reader), launching games | threads (nlohmann/json is bundled) |
| `recomp::achievements_ui` | Popups, list window, styles, sounds, icons | Dear ImGui and SDL3 targets (each optional) |
| `recomp::achievements_ui_base` | The renderer-agnostic part: popup queue, styles, synthesized sounds, WAV and PNG decoding, and the software overlay (viewer + popups, `Canvas`, fonts, the All games page) | nothing (stb_truetype and DejaVu Sans are bundled) |
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
| core | `src/system.cpp`, `src/json_format.cpp`, `src/config_dir.cpp`, `src/library.cpp`, `src/library_reader.cpp`, `src/library_util.cpp`, `src/launch.cpp` (link the platform's threads library, e.g. `-pthread`; on Windows also `shell32` and `advapi32`) | none |
| UI, base | `src/ui/style.cpp`, `notification_center.cpp`, `notification_attach.cpp`, `sounds.cpp`, `icon_cache.cpp`, `builtin_decoders.cpp`, `popup_painter.cpp`, `canvas.cpp`, `viewer_parts.cpp`, `viewer_toggle.cpp`, `overlay.cpp`, `src/app/app.cpp`, `src/app/library_view.cpp` | none |
| UI, fonts | `DejaVuSans.cpp`, `DejaVuSans-Bold.cpp`, generated from `assets/fonts` with `cmake -DINPUT=assets/fonts/DejaVuSans.ttf -DOUTPUT=DejaVuSans.cpp -DSYMBOL=kDejaVuSans -P cmake/embed_file.cmake` (and `DejaVuSans-Bold.ttf` / `kDejaVuSansBold`) | or none of them and `RECOMP_ACH_NO_EMBEDDED_FONTS=1`; then pass `OverlayOptions::font` |
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
popup_options.library.game_id = "my_game";               // the shared library (docs/library.md)
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

## The desktop app

`recomp-achievements` (`apps/achievements`) shows every recomp's achievements in one window, read
from the shared library: a grid of games with progress, totals over the sets that are on, recent
unlocks and four sort orders, and each game's page drawn by the same viewer as in game, with sets
that are off listed as Off and a profile picker. It updates live while a game runs, works with
keyboard, mouse or controller (a Steam Deck in Game Mode included), finds libraries in Proton
prefixes, starts games through Steam or directly ([docs/launching.md](docs/launching.md)), only
reads the library, and never uses the network. It is off by default:

```sh
cmake -S . -B build-app -G Ninja -DRECOMP_ACH_APP=ON -DRECOMP_ACH_SDL3_SOURCE_DIR=/path/to/SDL3
cmake --build build-app
build-app/tools/demo_library/recomp-achievements-demo /tmp/demo/library   # made-up games
build-app/apps/achievements/recomp-achievements --library /tmp/demo/library
```

See [docs/app.md](docs/app.md), including its headless mode for recording videos.

## Guides

- [JSON set format](docs/json-format.md) ([schema](schema/achievement-set.schema.json))
- [Popup styles and sounds](docs/styling.md)
- [Achievement overlay without ImGui](docs/overlay.md), and [what an ImGui game wires to match it](docs/imgui-hosts.md)
- [The shared achievement library](docs/library.md): its format, privacy, Proton and sync
- [The desktop app](docs/app.md) and [launching games](docs/launching.md)
- [Xbox 360 / rexglue](docs/xbox360.md), including [splitting off unsupported achievements](docs/xbox360.md#splitting-off-achievements-the-recomp-cant-support)
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

ImGui games get the same player settings from `AchievementListWindow` (and `PopupUi::list()`): a Settings
button with the theme and sizes, the popup sound switch and volume, a checkbox per set, and resets of chosen
achievements, a set or everything, each behind a confirmation dialog. `PopupUi` applies the sound, theme and
sizes to its popups and list.

Games without Dear ImGui link `recomp::achievements_ui_base` and use `ui::Overlay`: pass a few input
states each frame, call `Frame()`, and when it returns true upload `image()` and blend it over the frame
([docs/overlay.md](docs/overlay.md)). Games that draw their own popups feed a `NotificationCenter` and draw
its `Active()` popups, or implement `ui::Painter` to get the built-in look. WAV overrides and PNG icons
decode without SDL, and `SetSingleSlot(true)` keeps unlock and progress popups to one on screen at a time.

## License

[0BSD](LICENSE) (Zero-Clause BSD): use it for anything, no attribution needed. Bundled code keeps its
own licence: nlohmann/json (MIT), the rcheevos subset (MIT, `third_party/rcheevos/LICENSE`),
stb_image and stb_truetype (MIT or public domain) and the DejaVu Sans fonts (Bitstream Vera licence,
DejaVu changes public domain, `assets/fonts/LICENSE`).

## Credits

- [nlohmann/json](https://github.com/nlohmann/json) (MIT) is bundled in `third_party/`.
- [rcheevos](https://github.com/RetroAchievements/rcheevos) (MIT) evaluates memory conditions; its
  condition evaluator is bundled in `third_party/rcheevos`, without any of its network code.
- [stb_image](https://github.com/nothings/stb) (MIT or public domain) decodes icons without SDL, and
  stb_truetype from the same collection rasterises the overlay's text.
- [DejaVu Sans](https://dejavu-fonts.github.io/) is the overlay's bundled font.
- Imported RetroAchievements lists credit their authors in each set file. The text and badge art belong to those authors and to RetroAchievements.
