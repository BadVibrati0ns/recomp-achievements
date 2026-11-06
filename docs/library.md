# The achievement library

Every game that uses this SDK can keep an entry in one shared folder: its achievement sets as it
resolved them, their icons, each profile's progress and a log of what happened. A desktop app can then
show every recomp's achievements in one place (a grid of games, totals, recent unlocks, each game's
list) without ever reading a game's own files: no XEX, XDBF, `TROPHY.TRP` or save layouts. The
library is local; its event log is shaped so an accounts/sync service can be added later.

The writer is `recomp::achievements::Library` (`library.h`); the reader is `library_reader.h`. Both
are in the core target, `recomp::achievements`, and need no UI.

## Turning it on

With the overlay or `PopupUi`, set a game id. Its icons then come from the UI's `IconCache` (so
`xdbf:`, `trophy:` and `ra:` refs work through the providers the game already registered) and its
game icon defaults to `fallback_icon`:

```cpp
ui::OverlayOptions options;                  // or ui::PopupUiOptions
options.fallback_icon = "assets/icon.png";   // also the library's game icon
options.library.game_id = "my_game";          // the folder name; keep it stable
options.library.title = "My Game";           // optional: else the first set's title
options.library.version = "1.0.607";         // optional: recorded with every event
ui::Overlay overlay(achievements, options);  // overlay.library() is the Library
```

Without the UI, run a `Library` beside the `System` (destroy it first):

```cpp
#include "recomp/achievements/library.h"

LibraryOptions library_options;
library_options.game_id = "my_game";
library_options.icon = "assets/icon.png";
library_options.icon_source = MyIconSource;  // optional, for refs that aren't PNG files
Library library(achievements, library_options);
```

Leaving `game_id` empty, or `enabled = false`, turns it off. Games that want to ask the player first
can keep it off until they say yes.

### Options

| `LibraryOptions` | Default | Meaning |
|---|---|---|
| `game_id` | `""` | The entry's folder. Characters other than `A-Z a-z 0-9 . _ -` become `_`. Empty: off. |
| `title` | first set's title | Display name. |
| `version` | `""` | The game's version or build. |
| `icon`, `icon_base_dir` | the platform's title image | A file or icon ref. Empty tries `xdbf:32768` (Xbox 360) or `trophy:ICON0.PNG` (PS3) through the icon source. |
| `root` | `DefaultLibraryRoot()` | The library folder. |
| `enabled` | `true` | |
| `hash_profiles` | `false` | Store profile ids as salted hashes ([Privacy](#privacy)). |
| `wine_host` | `true` | Under Wine/Proton, write to the Linux host's library ([Proton](#wine-and-proton)). |
| `icon_source` | none | Resolves icon refs to PNG bytes or RGBA. The UI passes `ui::LibraryIconSourceFor(icons)`. Without one, only icons that are PNG files are exported. |
| `write_interval` | 500 ms | Progress snapshots are written at most this often. |
| `poll_interval` | 250 ms | How often resets, set toggles and new sets are looked for. |
| `rotate_bytes` | 4 MiB | `events.log` is renamed to `events-<unix ms>.log` past this size. 0: never. |
| `clock` | system clock | Unix milliseconds for event times, snapshots and `last_played`; for tests and demo data. |

## Where it is

`DefaultLibraryRoot()` is `library/` inside the player's folder for every game (`GlobalConfigDir()`,
`config_dir.h`, the same folder as the global popup style):

| | |
|---|---|
| Windows | `%APPDATA%\recomp-achievements\library` |
| Linux, macOS | `$XDG_CONFIG_HOME/recomp-achievements/library`, default `~/.config/recomp-achievements/library` |
| Override | `$RECOMP_ACHIEVEMENTS_CONFIG_DIR/library` |
| Windows build under Wine/Proton | the Linux host's `~/.config/recomp-achievements/library` through `Z:` ([below](#wine-and-proton)) |

## Layout

```
library/
├── install.json                     one random id for this library folder
└── my_game/                          <game id>
    ├── game.json
    ├── sets/
    │   ├── my_game.achievements.json  one per loaded set, in the SDK's set format
    │   └── my_game_online.achievements.json
    ├── icons/
    │   ├── index.json                what was exported from where (hashed keys)
    │   ├── 0cd50707a57a1041c9720274.png
    │   └── acb178672605ee2cc2d2e392.png
    ├── progress/
    │   ├── default.json              <profile id>.json
    │   └── h-3f9c2a71d04be655.json   a hashed profile id
    ├── events.log                    JSON Lines, append-only
    └── events-1793992905970.log      a rotated log
```

Every JSON file is written to a temporary file and renamed over the old one, so a reader sees the old
file or the new one, never half of one. Readers must skip files whose names contain `.tmp-`, and
ignore unknown keys everywhere.

### `install.json`

```json
{
  "schema": "recomp-achievements-install/1",
  "install_id": "dad51cb8-0275-4a34-9050-238b11687327",
  "created": 1793992905
}
```

A random UUID v4, made once by the first game that writes to this library, and shared by every game
after. It tells events from different machines (or Wine prefixes) apart. It is not tied to the player
or the hardware. The player may add `"hash_profiles": true` to hash profile ids for every game.

### `game.json`

```json
{
  "schema": "recomp-achievements-library/1",
  "game_id": "my_game",
  "title": "My Game",
  "platforms": ["xbox360"],
  "game_version": "1.0.607",
  "icon": "icons/acb178672605ee2cc2d2e392.png",
  "sdk_version": "0.1.0",
  "install_id": "dad51cb8-0275-4a34-9050-238b11687327",
  "sets": ["my_game", "my_game_online"],
  "last_played": 1793992905
}
```

`icon` is a path inside the entry, or null. `sets` lists the sets the game had loaded last, in load
order: a set file whose id isn't listed belongs to a set the game no longer loads (a removed mod) and
is kept but not shown. `last_played` (unix seconds) is written when the game starts and when it exits.
`"wine": true` marks a Windows build that ran under Wine or Proton.

### `sets/<set id>.achievements.json`

Each loaded set, fully resolved, in the [native set format](json-format.md): names and descriptions as
the game resolved them (translations, title update text), points after the set's cap, tiers, secret
flags and secret text, categories, credits, checklists and triggers. Secret achievements are in the
file like any other; hiding them is the app's job. Icon refs are rewritten to the exported files
(`"../icons/<file>.png"`, relative to the set file); an icon that couldn't be exported is left out, and
the app shows the game icon instead. One extra key, ignored by the set parser like every `_` key:

```json
"_library": { "enabled": true }
```

the player's current choice for the set (System's settings); `set.enabled` keeps the set's own default.

### `icons/`

PNGs named by the first 24 hex digits of their SHA-256, so identical icons share one file. Files that
already are PNGs are copied as they are; other images (and providers that hand back pixels) are
encoded as uncompressed PNGs. Each ref is fetched once per change, not on every start: file icons are
checked by size and modification time, provider refs by game version. `index.json` remembers this with
hashed keys, so it doesn't record where the game's files are. Icons that can't be fetched yet (a
provider registered late, a badge still downloading) are asked for again every 30 seconds and on
`Flush()`.

### `progress/<profile id>.json`

The active profile's progress, written whenever it changes (at most every `write_interval`):

```json
{
  "schema": "recomp-achievements-library-progress/1",
  "game": "my_game",
  "profile": "default",
  "updated": 1793992905,
  "totals": { "count": 4, "points": 100, "unlocked": 1, "unlocked_points": 25 },
  "sets": { "my_game": { "count": 4, "enabled": true, "points": 100, "unlocked": 1, "unlocked_points": 25 } },
  "achievements": {
    "my_game:a": { "unlock_time": 1793992905, "unlocked": true },
    "my_game:b": { "completed": ["Kickflip"], "current": 1, "total": 3, "unlock_time": 0, "unlocked": false }
  }
}
```

Each achievement entry is the same as in the game's own saves (`unlocked`, `unlock_time` in unix
seconds, `completed`, `count`, `best_value`), plus `current` and `total` when it has a progress bar, as
the game showed it. Only achievements with progress are listed. `totals` counts the sets that are on;
`sets` has every set, on or off. The file name is the profile id with unsafe characters replaced (and a
short hash added when anything was replaced); the `profile` field has the id itself.

### `events.log`

One JSON object per line, appended as things happen and never rewritten:

```json
{"v":1,"t":1793992905970,"install_id":"dad51cb8-…","game":"my_game","profile":"default","key":"my_game:a","kind":"unlock","game_version":"1.0.607"}
{"v":1,"t":1793992905970,"install_id":"dad51cb8-…","game":"my_game","profile":"default","key":"my_game:b","kind":"progress","value":1,"item":"Kickflip","game_version":"1.0.607"}
```

| Field | Meaning |
|---|---|
| `v` | Event format version, 1. |
| `t` | Unix milliseconds. |
| `install_id`, `game`, `profile`, `game_version` | Where it came from. |
| `key` | The achievement key; the set id for `set_enabled`. |
| `kind` | `unlock`, `progress`, `reset` or `set_enabled`. |
| `value` | `progress`: the new count. `set_enabled`: true or false. Absent otherwise. |
| `item` | `progress`: the checklist item or target that was completed, when there is one. |

Unlocks and progress come from System's listeners (test popups aren't logged). Resets and set toggles
have no listener: the library notices them when `System::generation()` changes, so their `t` is when
they were noticed (within `poll_interval`). A set toggle belongs to the game, not a profile: its
`profile` is empty. Past `rotate_bytes` the log is renamed to `events-<unix ms>.log` and a new one is
started; readers read `events.log` and every `events-*.log`.

## When it writes

The game thread only queues: the listeners add an event to a list and wake a small worker thread,
which appends events, writes snapshots, exports sets and icons, and looks for resets and toggles.
Bursts are coalesced: two hundred progress steps in a frame are one append and at most one snapshot.
`Flush()` does everything pending at once and returns when it's done; the destructor flushes. Icon
sources are called from the worker thread (or from `Flush()`'s caller).

An unwritable folder never stops the game: writes that fail are counted in `stats().failures` and
skipped, nothing throws, and the game's own saves are unaffected. Several games, or several copies of
one, can write to the same library at the same time: JSON files are replaced atomically and each batch
of events is one append.

## Privacy

The library is in the player's own config folder and never leaves the machine. What it holds:

- **Profile ids are the game's profile names** (gamertags, PSN-style names, whatever the game passes
  to `SetStorage`), stored as given. With `hash_profiles` (or `"hash_profiles": true` in
  `install.json`, the player's switch for every game) they are stored as `h-` plus 16 hex digits of
  SHA-256 over the install id and the name. The install id salts it, so a name can't be found by
  hashing a list of names without the install id, and the same name hashes differently on another
  machine.
- Secret achievements and their text are stored in full; the app should hide them until unlocked.
- No paths: icon refs are rewritten, the icon index is hashed, and nothing records where the game or
  its saves are installed.
- `install_id` is random and made once per library folder; delete `install.json` to get a new one.

## Wine and Proton

A Windows build running under Wine or Proton sees `%APPDATA%` inside its prefix
(`.../compatdata/<appid>/pfx/drive_c/users/steamuser/AppData/Roaming`), where a Linux app wouldn't
look. So with `wine_host` (on by default) the library detects Wine (ntdll exports `wine_get_version`)
and writes to the Linux host's `~/.config/recomp-achievements/library` through the `Z:` drive, which Wine
maps to `/`. The host folder is found from, in order: `XDG_CONFIG_HOME`, `WINEHOMEDIR`
(`\??\Z:\home\<user>`), `HOME`, then `Z:\home\<USER or LOGNAME>` (Proton's `steamuser` is skipped).
The first whose home folder exists wins; if none does, the library stays in the prefix.
`RECOMP_ACHIEVEMENTS_CONFIG_DIR` still wins, and a Unix path in it is mapped through `Z:` too.

For entries that ended up in a prefix anyway, `FindWinePrefixLibraries(home)` lists the library folders
in Steam's `compatdata/*/pfx` (native Steam, `~/.steam/steam`, Flatpak Steam, and extra Steam library
folders from `libraryfolders.vdf`) and in `~/.wine`. `DefaultLibraryRoots()` is the host library plus
those, and `ListLibrary(roots)` reads them all, keeping the copy played last when a game is in two.

## Reading it

```cpp
#include "recomp/achievements/library_reader.h"
using namespace recomp::achievements;

auto games = ListLibrary(DefaultLibraryRoots());  // grid: title, icon, totals per profile
LibraryTotals all = SumLibrary(games);            // each game counted by its best profile
auto recent = RecentUnlocks(games, 20);
auto game = LoadLibraryGame(games[0].dir.parent_path(), games[0].id);  // + sets
for (const LibrarySet& s : game->sets) { /* s.set.achievements, s.enabled */ }
auto events = ReadEvents(*game, /*since_ms=*/0);
```

`ListLibrary` returns every folder with a `game.json`, `sets/` or `progress/` (no sets, to stay fast);
`LoadLibraryGame` adds the sets, with icon refs turned into absolute paths inside the entry. Both
tolerate partial and old entries: a missing or broken `game.json` falls back to the folder name, an
unreadable set or progress file is skipped with a note in `warnings`, a progress file without totals
still counts its unlocks, and a newer `schema` is read as far as it goes. Paths in an entry that point
outside it are ignored.

The desktop app ([app.md](app.md)) is this reader with a window. To show a game with the viewer, it
fills a `System` that has no storage from the entry (`AddSet`, `SetSetEnabled`, then
`System::LoadProgress()`, which replaces progress in memory without saving or calling listeners).

## Sync, later

Nothing in the SDK uses the network. `sync.h` has the seam an app would implement over its own service:

```cpp
class SyncProvider {
  virtual bool signed_in() const = 0;
  virtual std::string account() const = 0;
  virtual bool Push(std::span<const LibraryEvent> events) = 0;
  virtual std::vector<LibraryEvent> Pull(uint64_t since_ms) = 0;
};
```

and `NullSyncProvider`, which is never signed in. The design: the app pushes local events it hasn't
pushed (`ReadEvents(game, last_push)`), pulls the account's events from other installs, and merges with
`MergeEvents()`, the union deduplicated by `(install_id, t, key, kind)`. Because `events.log` is
append-only and every event names its install, merging is order-independent and pushing twice is
harmless. Unlock state is then the latest `unlock` or `reset` per key; progress takes the highest
`value` and the union of `item`s. Games never sync: only the app does, so a game needs no account and
no network code.

### Trust model

Unlocks are self-reported. The game decides when an achievement unlocks, and the library files are
plain JSON the player can edit. An account service can therefore display a player's achievements and
totals, but it cannot verify them: a pushed `unlock` proves only that some install said so. Anything
that needs trust (leaderboards, rarity, "verified" badges) would need server-side checks the SDK
doesn't have, such as replaying memory-trigger conditions against recorded input. A service should
present synced unlocks as what they are, and never as proof.

## Versions

Every file carries a `schema` (`recomp-achievements-library/1`, `recomp-achievements-install/1`,
`recomp-achievements-library-progress/1`) and every event a `v`. New fields may appear without a
version change; readers ignore what they don't know. A major version change means a field changed
meaning; the reader still reads such entries as far as it can and notes it in `warnings`.
