# The desktop app

`recomp-achievements` shows every recomp's achievements in one window. It reads only the
[shared library](library.md) that games using this SDK write, so it works for any of them
without knowing anything about a game's files, and it works fully offline.

It draws with the same software renderer, fonts, themes, button hints and player preferences as the
in-game viewer ([overlay.md](overlay.md)). A game's page *is* that viewer.

## Screens

**Games.** One card per game: its icon (or a trophy when it has none), title, platform, a progress
bar with the percentage, the counts (`12 / 12 · 1000 / 1000 G`, or trophies by tier for PS3 games),
how many sets are off, and when it was last played. Above the cards: totals over every game and the
latest unlocks across games. Four sort orders:

| Sort | Order |
|---|---|
| Last played | newest first |
| % complete | highest first |
| Closest to done | unfinished games by achievements left, fewest first; finished games last |
| A–Z | by title |

Every total counts only the sets that are **on** (the player's choice, stored in the library). A game
whose sets that are on are all done shows 100%, even when an optional pack that's off has more.

**A game's page** is the in-game viewer over the dimmed grid: set tabs, All / Unlocked / Locked,
categories, checklists, progress bars, secrets hidden until "Show secrets", and credits. Sets that are
off are listed too, marked **Off** and dimmed, and stay out of the totals; they are read-only here
(turning them on is the game's settings page). When more than one profile played the game, a
**Profile** button picks which one to show; it starts on the profile with the most points (the one
played last on a tie), and the game's card follows the choice.

**Live.** The library is checked about once a second (file sizes and times, so it's cheap). When a
game running alongside unlocks something or moves a checklist forward, the card, totals, recent
unlocks and an open page update in place, keeping the selection and scroll, and a popup shows what
changed.

## Controls

| Action | Controller | Keyboard | Mouse |
|---|---|---|---|
| Move | D-pad, left stick | arrows, Page Up/Down, Home, End | hover |
| Open a game | A (cross) | Enter | click a card or a recent unlock |
| Sort | X (square), LB / RB | Tab, Shift+Tab | click a sort mode |
| Scroll | right stick | | wheel |
| Fullscreen | Back / Select | F11 | |
| On a game's page | the viewer's own controls ([overlay.md](overlay.md#bindings-and-controls)); Start / F2 is the next profile, B / Esc goes back | | |

Hints follow the device used last, with Xbox labels unless the pad is a PlayStation one or the
player's `viewer.buttons` says otherwise.

## Building

The app is off by default; the SDK's own build never needs SDL3.

```sh
cmake -S . -B build-app -G Ninja -DRECOMP_ACH_APP=ON -DRECOMP_ACH_SDL3_SOURCE_DIR=/path/to/SDL3
cmake --build build-app
ctest --test-dir build-app
```

SDL3 comes from, in order: `RECOMP_ACH_SDL3_TARGET` (an SDL3 target you already have),
`RECOMP_ACH_SDL3_SOURCE_DIR` (a source tree, built static with only video, render, events and
gamepads), or an installed SDL3 (`find_package(SDL3)`). This builds:

| Target | File | |
|---|---|---|
| `recomp_achievements_app` | `recomp-achievements` | the app |
| `recomp_achievements_demo` | `recomp-achievements-demo` | writes a demo library (below) |
| `recomp_achievements_app_tests` | | headless tests over the demo library |

## Running

```sh
recomp-achievements                        # the player's library, and any in Proton prefixes
recomp-achievements --library ~/somewhere  # one folder (repeatable)
recomp-achievements --size 1280x800 --scale 1.25 --fullscreen
```

The window can be resized; the layout reflows. Text scales with the window's height (height / 1080,
but never below 1 at 800 lines, so it stays readable on a Steam Deck), times `--scale`, times the
player's `viewer.scale`. On a Steam Deck in Game Mode add it as a non-Steam game; it is usable with
the controller alone.

### Where it looks

`DefaultLibraryRoots()` ([library.md](library.md#wine-and-proton)): the player's library
(`$XDG_CONFIG_HOME/recomp-achievements/library`, `~/.config/...` by default, `%APPDATA%` on
Windows, or `$RECOMP_ACHIEVEMENTS_CONFIG_DIR/library`), plus library folders inside Wine and Proton
prefixes (Steam's `compatdata/*/pfx` for native, `~/.steam` and Flatpak Steam and every Steam library
folder, and `~/.wine`), for Windows builds that couldn't reach the host's folder. New prefixes are
noticed while it runs. A game found in more than one place shows the copy played last.

## Demo library

```sh
recomp-achievements-demo /tmp/demo/library      # five made-up games, saves in /tmp/demo/library-saves
recomp-achievements --library /tmp/demo/library
# a game "running" beside it: an unlock, or a checklist step
recomp-achievements-demo --play /tmp/demo/library starfall_odyssey unlock starfall:t19
recomp-achievements-demo --play /tmp/demo/library pocket_garden event plant Mint
```

The games are written through the real export path (`System` + `Library`, with a fake clock so
sessions spread over the past weeks): an Xbox 360-style game finished at 100% with an online pack
that starts off (made with `SplitSet`), a PS3-style game with tiers, a checklist and two profiles, a
custom game with checklists and secrets and no icons, a copy of a RetroAchievements-style list with
credits, and one barely started. Titles are made up and icons are generated shapes.

## Headless mode

```sh
RECOMP_ACH_DEMO=build-app/tools/demo_library/recomp-achievements-demo RECOMP_ACH_LIBRARY=/tmp/demo/library \
recomp-achievements --headless --library /tmp/demo/library --script apps/achievements/tour.script \
    --size 1280x800 --frames-out - |
  ffmpeg -f rawvideo -pix_fmt rgb24 -s 1280x800 -r 30 -i - -c:v libx264 -pix_fmt yuv420p tour.mp4
```

No window or display is needed. The script's commands run one after another at `--fps` (30); every
frame is written as raw RGB24, so a run is the same every time. When the script resizes the window,
it is centred in the `--out-size` frame. Commands (`apps/achievements/script.h`):

```
wait 1.5              key Down [secs]       pad a [secs]          stick ly 0.8 [secs]
mouse 640 400 [click] wheel -2              size 1280 800         scale 1.25
labels playstation    exec <shell command>  poll
```

`apps/achievements/tour.script` is the tour in the demo video.

## Privacy

The app only reads: it never writes to the library, a game's files or the player's folder (not even
the starter preferences file the games create), and it has no network code. Profile ids are shown as
the library stores them (hashed when the player turned that on). Accounts and sync will come through
`SyncProvider` (`sync.h`); nothing uses it yet.

## How it's built

The app core (`apps/achievements/app.cpp`) needs no SDL: it draws into a `ui::Canvas` and composites
the viewer's image and the popups over it; `main.cpp` uploads the changed rectangle to an SDL
texture and presents only when something changed. A game's page reuses the viewer unchanged: the app
fills a `System` without storage from the library entry (`AddSet`, `SetSetEnabled`,
`System::LoadProgress`) and opens a `ui::Overlay` on it with `show_disabled_sets` and
`SetHostButton` for the profile picker, so the in-game list and the app's list are one code path.
