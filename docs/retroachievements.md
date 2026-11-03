# RetroAchievements lists

RetroAchievements does not support recompilations, and the SDK never connects
to it at runtime: no login, no unlock submission, no rich presence, no
leaderboards. A recomp's achievements are unlocked and saved locally.

What the SDK can do is start from RetroAchievements' **public achievement
list** for a game, so a recomp shows the same achievements players know, with
credit to the people who designed them. `tools/ra_import.py` copies that list
into a set file once, at development time.

## What is copied

From [API_GetGameExtended](https://api-docs.retroachievements.org/v1/get-game-extended.html):

| RetroAchievements | Set file |
|---|---|
| achievement ID | `platform_id`, `id` (key `<set id>:<id>`) |
| Title, Description, Points | `name`, `description`, `points` |
| DisplayOrder | `display_order` |
| BadgeName | `icon: "ra:<badge>"`, `locked_icon: "ra:<badge>_lock"` |
| type (progression, win_condition, missable) | `_ra_type` |
| Author | `credits: [{name, role: "author", url: profile}]` |
| MemAddr (an MD5 of the conditions) | `_ra_logic_md5` |
| game title, console, id | `set.title`, `set.source` |

Fields starting with `_` are extensions the SDK's loader ignores; the UI
doesn't use them yet, but they survive `--merge`.

Unlock logic is **not** copied: the public API only gives an MD5 hash of each
achievement's conditions. Every imported achievement starts with
`"trigger": {"type": "manual"}`, and the recomp gives it its own trigger:
a built-in type (`stat`, `event`, `checklist`, `count`), a call to `Unlock()`
from game code, or a `"memory"` condition it writes itself
([memory-triggers.md](memory-triggers.md)).

`_ra_logic_md5` lets `--merge` warn when RetroAchievements changes an
achievement's conditions upstream, a hint that its meaning may have changed.

## Crediting

The set's `credits` list every achievement author (most achievements first)
plus a `RetroAchievements` credit (role `source`) linking the game page, with
a `_note` that the list was written by the RetroAchievements community. Each achievement credits
its author with a link to their profile. The UI shows both. Keep them: they
are the reason the list can be used at all.

Recomp-specific credits (`"role": "port"`, artwork, ...) can be added next to
them; `--merge` keeps them.

## Ownership and redistribution

Achievement titles, descriptions and badge images belong to their authors and
RetroAchievements. Copying them into a recomp is a courtesy to players, not a
right:

- Credit the authors and RetroAchievements (the importer does).
- Badge images are downloaded only with `--badges`, for offline use.
- RetroAchievements' Terms of Use allow titles, descriptions and similar
  metadata to be reused by community tools, but say nothing explicit about
  redistributing whole sets. **Before committing an imported set or its
  badges to a public repository, check with RetroAchievements** (and ideally
  the set's authors).
- Don't present the recomp as RetroAchievements-supported, and don't imply
  unlocks count on retroachievements.org.

## Importing

You need a RetroAchievements account's web API key
(<https://retroachievements.org/settings>). The tool reads it from the
`RA_API_KEY` environment variable or `--key-file`, never from the command
line, and never prints or writes it.

```sh
export RA_API_KEY=...            # or: --key-file ~/.config/ra_api_key
tools/ra_import.py 1234 -o sets/ra-1234.achievements.json
tools/ra_import.py 1234 -o sets/ra-1234.achievements.json --badges sets/ra-1234.badges
tools/ra_import.py 1234 --unofficial -o sets/ra-1234-unofficial.achievements.json
```

See [tools/README.md](../tools/README.md) for every option.

## Refreshing: always `--merge`

Lists change: achievements are added, retired, renamed, re-pointed. Refresh a
set with `--merge`, never by importing over it:

```sh
tools/ra_import.py 1234 --merge sets/ra-1234.achievements.json
```

`--merge` matches achievements by RetroAchievements id and

- refreshes `name`, `description`, `points`, `display_order`, `_ra_type`,
  `_ra_logic_md5` and author credits;
- keeps the recomp's triggers, custom icons (an icon still pointing at the
  old `ra:` badge follows the new badge), other credits and every extra field,
  on achievements and on the set;
- appends new achievements with a `manual` trigger;
- keeps achievements RetroAchievements removed, marked `"_ra_removed": true`,
  so saved progress isn't orphaned (`--drop-removed` deletes them);
- leaves achievements the recomp added itself (no `platform_id`) alone;
- prints what was added, removed, changed, and whose logic changed.

`--dry-run` shows the report without writing.

Achievement keys come from the RetroAchievements id, so saves survive
refreshes and reordering.
