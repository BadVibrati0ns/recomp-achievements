# sets/

Achievement set files that ship with the SDK or are kept as examples. A recomp
keeps its own sets in its own repository (typically an `achievements/` folder
next to the game data, loaded with `System::LoadDirectory`).

## Imported RetroAchievements sets

Sets made with `tools/ra_import.py` are named after the RetroAchievements game
id, with badges (if downloaded) in a folder beside them:

```
sets/
  ra-1234.achievements.json     # tools/ra_import.py 1234 -o sets/ra-1234.achievements.json
  ra-1234.badges/               #   --badges sets/ra-1234.badges
    301001.png
    301001_lock.png
```

`LoadDirectory` picks up every `*.achievements.json`. The set's
`source.badge_dir` records the badge folder relative to the set file.

- Refresh an imported set with `--merge`, never by re-importing over it: the
  recomp's triggers live in the same file. See
  [docs/retroachievements.md](../docs/retroachievements.md).
- Titles, descriptions and badges belong to their authors and
  RetroAchievements. Keep the credits, and check with RetroAchievements before
  committing an imported set or its badges to a public repository.
- An imported achievement has a `manual` trigger until the recomp gives it one.
