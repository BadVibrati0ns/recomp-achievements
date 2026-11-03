# tools/

## ra_import.py

Copies a game's public RetroAchievements achievement list into a
recomp-achievements set file (Python 3.9+, standard library only). It runs at
development time; the SDK itself never contacts RetroAchievements. What it
copies, crediting and redistribution: [docs/retroachievements.md](../docs/retroachievements.md).

```sh
export RA_API_KEY=...        # your web API key, https://retroachievements.org/settings
tools/ra_import.py <game id> -o sets/ra-<game id>.achievements.json
```

The game id is the number in the game page URL
(`https://retroachievements.org/game/<id>`).

| Option | |
|---|---|
| `-o, --output FILE` | set file to write (atomically) |
| `--merge FILE` | refresh an existing set, keeping the recomp's triggers, icons and extra fields; writes back to FILE unless `-o` is given; prints added / removed / changed achievements |
| `--drop-removed` | with `--merge`, delete achievements RetroAchievements no longer lists (default: keep, marked `_ra_removed`) |
| `--key-file FILE` | read the API key from FILE's first non-comment line instead of `RA_API_KEY` |
| `--input FILE` | use a saved `API_GetGameExtended` response instead of fetching (no key needed) |
| `--unofficial` | import the unofficial achievements (`f=5`); default set id `ra-<id>-unofficial` |
| `--set-id ID` | set id (default `ra-<game id>`); prefixes every achievement key |
| `--category NAME` | default category (default: the game title) |
| `--ra-hash HASH` | record the RetroAchievements hash of the game version the recomp is built from |
| `--badges DIR` | download `<badge>.png` and `<badge>_lock.png` into DIR for offline use (existing files are skipped) |
| `--refresh-badges` | re-download badges that already exist |
| `--delay SECONDS` | minimum gap between requests (default 1.0) |
| `--dry-run` | fetch and report, write nothing |
| `-q, --quiet` | errors only |

The key is never accepted as a command-line argument (it would land in shell
history), and is never printed, logged or written to the output. Requests carry
a `User-Agent` naming the tool, are spaced by `--delay`, and back off on HTTP
429 / 5xx (honouring `Retry-After`).

Exit status: 0 on success, 1 on any error (message on stderr), 2 for bad
arguments.

### Tests

```sh
python3 -m unittest discover -s tools/tests
```

The tests use saved responses in `tools/tests/fixtures/` (a fictional game
shaped like real `API_GetGameExtended` output, in PascalCase and camelCase)
and a fake HTTP opener; they never touch the network.
