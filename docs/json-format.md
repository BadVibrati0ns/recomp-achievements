# Achievement set JSON format

An achievement set is one JSON file named `<anything>.achievements.json`. The
schema is in [`schema/achievement-set.schema.json`](../schema/achievement-set.schema.json)
(JSON Schema draft 2020-12); point `"$schema"` at it to get completion and
checking in editors. Load a file with `System::LoadSetFile()`, or a folder of
them with `System::LoadDirectory()`.

The format is meant to be written by hand. Almost everything is optional, a
trigger can be a single word, and `//` and `/* */` comments are accepted (the
SDK strips them; strict JSON tools won't). Keys starting with `$` or `_` are
ignored everywhere, so `"_note": "..."` works as a comment too. Any other
unknown key is an error, which catches typos like `"descripton"`.

## A small set

```json
{
  "$schema": "../schema/achievement-set.schema.json",
  "schema": "recomp-achievements/1",
  "set": { "id": "my_game", "title": "My Game", "platform": "xbox360" },
  "achievements": [
    { "id": "first_board", "name": "Board Owner", "points": 10 },
    {
      "id": "combo",
      "name": "Combo Breaker",
      "description": "Chain 10 tricks in one line.",
      "points": 25,
      "trigger": { "stat": "tricks_chained", "threshold": 10 }
    },
    {
      "id": "flips",
      "name": "Flip Book",
      "points": 30,
      "checklist": ["Kickflip", "Heelflip", "360 Flip"],
      "trigger": { "event": "trick" }
    }
  ]
}
```

## Top level

| Key            | Type   | Default | Meaning |
|----------------|--------|---------|---------|
| `schema`       | string | `"recomp-achievements/1"` | Format version. A newer major version is rejected. |
| `set`          | object | `{}`    | The set's details (below). |
| `achievements` | array  | required | The achievements, in display order. |

## `set`

| Key          | Type    | Default | Meaning |
|--------------|---------|---------|---------|
| `id`         | string  | file name without `.achievements.json` | Unique id. Every achievement key is `<set id>:<achievement id>`. |
| `title`      | string  | the id  | Display name. |
| `platform`   | string  | `"custom"` | `custom`, `xbox360`, `ps3` or `retroachievements`. Picks the default popup style and how `platform_id` is read. |
| `category`   | string  | `""`    | Category for achievements that don't set their own. |
| `points_cap` | integer | `0`     | If the points add up to more than this they are scaled to exactly this total (see [Points cap](#points-cap)). `0`: no cap. |
| `points_total` | integer | `0`   | The points are scaled up or down to exactly this total, e.g. `1000` (see [Points cap](#points-cap)). Beats `points_cap`. `0`: off. |
| `enabled`    | boolean | `true`  | Whether the set counts until the player turns it on or off in the settings (see [Sets that are off](#sets-that-are-off)). `false` suits optional packs, such as achievements that need servers that are gone. |
| `credits`    | array   | `[]`    | [Credits](#credits) for the whole set. |
| `source`     | object  | `{}`    | Free-form provenance: game id, URL, export date, tool. |

## Achievements

| Key                  | Type            | Default | Meaning |
|----------------------|-----------------|---------|---------|
| `id`                 | string or integer | `platform_id` | Unique within the set. Saves are keyed by `<set id>:<id>`, so don't change it once released (use `legacy_keys` if you must). |
| `platform_id`        | integer         | `0`     | XDBF achievement id, trophy id or RetroAchievements id. |
| `name`               | string          | required | |
| `description`        | string          | `""`    | |
| `locked_description` | string          | `""`    | Shown while locked instead of `description`. Leave empty to let the UI decide (it hides secret ones). |
| `category`           | string          | set's `category` | |
| `points`             | integer         | `0`     | Gamerscore or points. |
| `tier`               | string          | `"none"` | `bronze`, `silver`, `gold`, `platinum` for trophies. |
| `secret`             | boolean         | `false` | |
| `display_order`      | integer         | position in the file | Sort key within the set. |
| `icon`, `locked_icon`| string          | `""`    | A path relative to the set file, `xdbf:<image id>`, `trophy:<file>` or `ra:<badge>`. |
| `legacy_keys`        | array           | `[]`    | Older save keys (e.g. `"105"` from before the set had an id). Read when the current key isn't in a save; saving always uses the current key. |
| `credits`            | array           | `[]`    | [Credits](#credits) for this achievement. |
| `checklist`          | array of strings | `[]`   | Items for a `checklist` trigger, in display order. |
| `trigger`            | string or object | `"manual"` | How it unlocks (below). |

## Triggers

`"trigger": "manual"` is short for `"trigger": { "type": "manual" }`. If
`type` is left out of an object it is inferred: `stat` when `stat` is set,
`memory` when `condition` is, `checklist` when `event` is set and the
achievement has a `checklist`, `event` when only `event` is, else `manual`.

| Type        | Fields | Unlocks when |
|-------------|--------|--------------|
| `manual`    | none   | The game calls `Unlock()`. |
| `stat`      | `stat`, `threshold` | `ReportStat(stat, v)` with `v >= threshold`. The best value seen while locked is saved and drives the progress bar when `threshold >= 2`. Stat aliases (`AddStatAlias`) work both ways. |
| `event`     | `event`, optional `target` | `ReportEvent(event, t)` where `target` is empty, `"any"`, or matches `t`. |
| `checklist` | `event`, the achievement's `checklist` | Every checklist item has been reported as a target of `event`. Each new item sends a progress event until the last, which unlocks. |
| `count`     | `event`, `threshold`, optional `target`, `distinct` | `threshold` distinct targets were reported (or `threshold` reports at all with `"distinct": false`). `target` limits which reports count. |
| `memory`    | `condition` | A RetroAchievements-syntax condition over guest memory is true (see `memory_triggers.h`). |
| anything else | anything | Game-defined. The game checks it itself, usually with `ForEachPending(type, ...)`, and calls `Unlock()`, `CompleteItem()` or `RecordValue()`. |

Names are compared with the system's name matcher. The default compares
`NormalizeName()` of both sides, so `"360 Flip"`, `"360flip"` and `"360-FLIP"`
are the same; the game can install its own (`SetNameMatcher`) for aliases
such as "Tre Flip" = "360 Flip".

Any field in a trigger object that isn't listed above is kept in
`Trigger::params`, together with the contents of an optional `params` object,
so game-defined triggers can carry their own data:

```json
"trigger": { "type": "gap", "takeoff": [415, 69, -210], "landing": [415, 65, -181], "radius": 8.5 }
```

`"progress_bar": false` hides the progress bar the UI would otherwise show
(for a speed record, say, where "31 / 60" means little).

`"progress_popups"` sets how often a checklist or count pops a progress popup: `true` (the default) on
every step, `false` never, or a number N on every Nth step. The unlock popup always shows. For "land 50
combos", `"progress_popups": 10` pops at 10, 20, 30 and 40. Both keys go inside `trigger`.

## Credits

Either a name, or an object with `name` (required), `role` and `url`:

```json
"credits": ["Jane Doe", { "name": "Port Team", "role": "port", "url": "https://example.com" }]
```

## Points cap

When a set's points add up to more than `points_cap`, each non-zero value is
scaled by `cap / total` and rounded (never below 1). If rounding leaves the
total off by a little, the largest value (the first, on ties) is adjusted one
point at a time until the total is exactly the cap. This is the same scaling
the older mod packs used.

`points_total` scales in both directions, so a list worth 690 points with
`"points_total": 1000` is worth exactly 1000, and one worth 1500 is too. Each
value is rounded down (never below 1), and the points still missing go one each
to the values that lost the most to rounding (the larger value, then the first,
on ties), so they spread across the list instead of piling onto one
achievement. A set already at the total, or with no points at all, is left
alone. When a set has both keys, `points_total` is used and `points_cap`
ignored.

## Errors

Errors name the file, the achievement and the field:

```
mods/extra.achievements.json: achievements[3] ("Flip Book").trigger ("checklist"): needs a non-empty "checklist" on the achievement
```

`LoadSetFile()` returns `false` and fills its `error` argument; nothing is
loaded from a file with an error.

## Folders and old mod packs

`LoadDirectory(dir)` loads every `*.achievements.json` in `dir`, then each
subfolder (folders ending in `.disabled`, `.off`, `.bak` or `.tmp` are
skipped). A subfolder is a mod: its `*.achievements.json` files are loaded if
it has any, otherwise its `achievements.json`, otherwise its `pack.json`.
Relative icon paths resolve from the folder that holds the file.

`achievements.json` and `pack.json` may also be in the older pack
format, which is still read as before:

```json
{
  "name": "Sample Achievement Mod",
  "category": "Sample Achievement Mod",
  "enabled": true,
  "achievements": [
    { "id": "sample_speed", "name": "Breeze", "gamerscore": 50, "trigger": "speed", "threshold": 30.0 },
    { "id": "sample_flips", "name": "Flips", "gamerscore": 50, "trigger": "trick_group", "group": "flips" }
  ]
}
```

- The set id is `category`, else `name`, else the folder name, so keys
  (`"Sample Achievement Mod:sample_speed"`) match existing saves. A pack with
  `"enabled": false` or no achievements is skipped: in this old format
  `enabled` means "don't load", unlike `set.enabled` in the native format.
  `achievements.json` may also be a bare array.
- The achievement id is `id` (string or number), else `name`.
- Old names map to the new fields: `gamerscore` → `points` (at most 500 each,
  and the pack is capped at 1000), `is_secret` → `secret`, `unachieved_desc` →
  `locked_description` (default: the description), `retail_id` →
  `platform_id`, `tricks` → `checklist`, `trigger` → the trigger type,
  `spot`/`target`/`group`/`code` (first present) → `target`, and
  `threshold`/`count`/`score` (first present) → `threshold`.
- Every other field (`radius`, `position`, `takeoff`, `landing`, `group`, ...)
  is kept in `Trigger::params`. The old trigger types (`own_spot`, `speed`,
  `trick_group`, ...) are game-defined: the game evaluates them.

The new names (`points`, `secret`, `checklist`, a `trigger` object, ...) are
also accepted in old-format packs.

## Save files

Progress is saved per profile as `<dir>/<profile>_achievements.json`:

```json
{
  "profile": "player",
  "achievements": {
    "my_game:combo": { "unlocked": true, "unlock_time": 1792116513 },
    "my_game:flips": { "unlocked": false, "unlock_time": 0, "completed": ["Kickflip"] },
    "my_game:spots": { "unlocked": false, "unlock_time": 0, "count": 3 },
    "my_game:speed": { "unlocked": false, "unlock_time": 0, "best_value": 41.5 }
  }
}
```

`completed_tricks` and `best_stat` from older saves are read as `completed`
and `best_value`. Entries for sets that aren't loaded are kept, entries with
no progress are left out, and fields the SDK doesn't know are preserved.

## Sets that are off

The player can turn each set on and off (`System::SetSetEnabled`, or the settings page of the viewers).
A set that's off can't unlock or move forward: `Unlock()` returns false and triggers skip it, so no popup
can come from it. It is left out of `Snapshot()`, `GetTotals()`, `enabled_sets()` and the viewers' tabs and
totals, and its saved progress stays in the save file, so turning it back on brings everything back. Any
number of sets can be on. `"set": {"enabled": false}` makes a set start off until the player turns it on.

## Settings file

The player's choices for the game (not per profile) are saved as `<storage dir>/achievement_settings.json`,
or wherever `System::SetSettingsPath()` points; with no storage they are kept in memory. It is written the
same way as saves (to a `.tmp` file, then renamed):

```json
{
  "schema": "recomp-achievements-settings/1",
  "sets": {
    "my_game_online": { "enabled": true }
  },
  "settings": {
    "popup_sound": { "enabled": true, "volume": 0.7 },
    "appearance": { "theme": "", "popup_size": 1.0, "list_size": 1.15, "popup_seconds": 0 }
  }
}
```

`sets` holds only choices the player made (other sets use their `enabled`), including ones for sets that
aren't loaded. `settings` is free-form for the UI layer (`System::GetSetting` / `SetSetting`);
`popup_sound` is the popup sound switch and volume (`ui::GetPopupSoundSettings`), and `appearance` the
player's theme, sizes and popup time (`ui::GetAppearanceSettings`, [styling.md](styling.md#the-players-theme-and-sizes)).
Unknown fields are kept,
and an unreadable file is copied to `achievement_settings.json.corrupt` before it is replaced.
