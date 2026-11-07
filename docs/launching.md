# Launching games

The library app can start any game in the library, through Steam or directly. Players aren't forced
to use Steam for a game that has problems there, and a game that was first started from a script and
added to Steam later is still found. Nothing here uses the network. The API is in `launch.h` (core
target).

## What a game records

A game with the library on (`options.library.game_id`) records two things in its entry, with no code
of its own:

- **How it was started**, in `game.json` under `"launch"`:

  ```json
  "launch": {
    "os": "linux",
    "exe": "/home/deck/Games/my_game/my_game",
    "args": ["--fullscreen"],
    "cwd": "/home/deck/Games/my_game",
    "steam_game_id": "13836834917289787392"
  }
  ```

  - `exe`, `args` and `cwd` describe this process. Linux reads them from `/proc/self`, Windows from
    `GetModuleFileNameW` and the command line, and macOS from `_NSGetExecutablePath`.
  - `steam_game_id` is the Steam id (`SteamGameId`) when Steam started the game. It is a store app
    id, or a 64-bit id for a non-Steam shortcut.
  - A Windows build under Wine or Proton records host paths through Wine's `wine_get_unix_file_name`,
    adds `"wine": true` and `wine_prefix`, and adds `"proton": true` when Proton started it.

- **That it's running**, as `running.json` beside `game.json`. The file holds the process id, the
  process's start time and, on Linux, the boot id. It is removed when the game exits cleanly. A file
  left behind by a crash doesn't count, because the start time no longer matches: if the process id
  is reused, it belongs to a process that started at a different time. A game that is stopped or
  suspended still counts as running, for example one paused in the background on a Steam Deck. Under
  Wine the game reads its Unix process id through `Z:\proc\self\stat`, so an app on the Linux host
  can check it.

| `LibraryOptions` | Default | Meaning |
|---|---|---|
| `record_launch` | `true` | Record `"launch"`. Off: the app can't start the game directly. |
| `launch` | empty | Replace the recorded `exe` and `args`, and `cwd` when it is set. Use it for a game that should be started through its own launcher or script. |
| `running_marker` | `true` | Keep `running.json` while the game runs. |

Wrapper scripts can do the same without code. They set `RECOMP_ACHIEVEMENTS_LAUNCH_EXE` to
themselves, and optionally `RECOMP_ACHIEVEMENTS_LAUNCH_ARGS` with one argument per line, before
running the game:

```sh
#!/bin/sh
export RECOMP_ACHIEVEMENTS_LAUNCH_EXE="$(realpath "$0")"
exec ./build/my_game "$@"
```

## Finding Steam

The app reads Steam's `shortcuts.vdf` files each time its Play menu opens, so it sees a shortcut that
was added after the game last ran. A game first started from a bash script and added to Steam
later gets a Steam option the next time its menu opens.

`FindSteamShortcutFiles()` looks under `userdata/*/config/` in these folders:

- On Linux: `~/.local/share/Steam`, `~/.steam/steam`, `~/.steam/root` and Flatpak Steam.
- On Windows: Steam's install folder, from the registry.
- On macOS: `~/Library/Application Support/Steam`.

`PlanLaunch()` decides which Steam id to use:

1. **The id Steam started the game with last time.** A shortcut id counts only while that shortcut
   still exists; a store app id is always kept.
2. **Otherwise, a matching shortcut**, best match first:
   - its Exe is the game's executable;
   - its launch options name the executable (for example `/usr/bin/env` with
     `/path/game %command%`);
   - it is a script in the game's folder that starts there.

Steam starts the game with `steam://rungameid/<id>`. On Linux that goes through `steam`, Flatpak Steam
or `xdg-open`; on Windows through the shell; on macOS through `open`.

## Starting directly

The game's executable runs with its recorded arguments in its recorded folder. It is detached in its
own session (a double fork, or `DETACHED_PROCESS` on Windows), so it outlives the app.

- **Wine:** a Windows build recorded under plain Wine runs as `wine <exe>`, with its `WINEPREFIX`.
- **Proton:** builds recorded under Proton start only through Steam, since Proton's runtime can't be
  rebuilt from outside.
- **No record yet:** a game recorded by an older SDK has no `"launch"`. It needs one run with this
  version before it can be started directly.

Inside Steam's Game Mode the app may itself run as a Steam shortcut, and a game it starts inherits
the app's `SteamGameId`. The app then sets `RECOMP_ACHIEVEMENTS_PARENT_STEAM_ID`, so the game doesn't
record the app's id as its own.

## The player's choices

The app remembers, for each game, whether it was last started through Steam or directly, and offers
that choice first next time. The choices are stored in `launcher.json` in the player's folder for
every game (`GlobalConfigDir()`). This is the only file the app writes:

```json
{
  "schema": "recomp-achievements-launcher/1",
  "quit_current": true,
  "games": { "starfall_odyssey": { "method": "direct" } }
}
```

`quit_current` is the "Quit current game" box for starting another game from inside a game; the
in-game launcher will use it.

## In the app

On the grid, Y (triangle), Space or the selected card's **Play** button opens the Play menu. The menu
has two rows: **Launch through Steam**, with where the Steam entry came from, and **Launch
directly**, with what will run. Each row is dimmed when that way isn't available. Up and down choose,
A or Enter starts the game, and B or Esc closes the menu.

While the game starts its card shows **Starting…**. Once it is running the card shows **Running**,
and its unlocks pop up as they happen. Opening the menu for a game that is already running says so
and only offers Close, so a second copy can't be started by accident. If starting fails, the reason
stays in the menu.

## Trying it

The demo library makes a `play.sh` for each game and a Steam shortcut for Starfall Odyssey. Started
from the app, a demo game runs for about 25 seconds and unlocks two achievements:

```sh
recomp-achievements-demo /tmp/demo/library
recomp-achievements --library /tmp/demo/library --steam-home /tmp/demo/steam-home \
    --launcher-prefs /tmp/demo/launcher.json
```

Pick **Launch directly** for the demo games. Their Steam entry is made up, so Steam can't start it.
