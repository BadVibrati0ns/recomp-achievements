#!/usr/bin/env python3
"""Copy a public RetroAchievements achievement list into a recomp-achievements set file.

Only the public list is copied (titles, descriptions, points, badges, display
order, type and authors), with credit to the authors. RetroAchievements' web
API does not publish unlock logic, so every imported achievement gets a
"manual" trigger; the recomp supplies its own triggers afterwards. Use --merge
to refresh a set later without losing them.

The web API key is read from the RA_API_KEY environment variable or from
--key-file, never from the command line, and is never printed or written.

Examples:
  RA_API_KEY=... tools/ra_import.py 1234 -o sets/ra-1234.achievements.json
  RA_API_KEY=... tools/ra_import.py 1234 --merge sets/ra-1234.achievements.json
  tools/ra_import.py 1234 --input saved-response.json -o out.json
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Callable, Iterable

TOOL_VERSION = "1.0"
SCHEMA = "recomp-achievements/1"
API_URL = "https://retroachievements.org/API/API_GetGameExtended.php"
BADGE_URL = "https://media.retroachievements.org/Badge/{name}.png"
SITE = "https://retroachievements.org"
USER_AGENT = (
    f"recomp-achievements-ra_import/{TOOL_VERSION} "
    "(offline achievement-list import for static recompilations; python-urllib)"
)
FLAG_OFFICIAL = 3
FLAG_UNOFFICIAL = 5
_BADGE_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
_MD5_RE = re.compile(r"^[0-9a-fA-F]{32}$")
# Fields the import owns. On --merge they are refreshed; everything else is
# the recomp's and is kept.
_REFRESHED_FIELDS = ("name", "description", "points", "display_order", "_ra_type", "_ra_logic_md5")


class ImportError_(Exception):
    """A problem worth a one-line message rather than a traceback."""


# ---------------------------------------------------------------------------
# API key


def read_api_key(key_file: str | None, environ: dict[str, str] | None = None) -> str:
    if environ is None:
        environ = dict(os.environ)
    if key_file:
        try:
            with open(key_file, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#"):
                        return line
        except OSError as e:
            raise ImportError_(f"cannot read key file {key_file}: {e.strerror}") from None
        raise ImportError_(f"key file {key_file} is empty")
    key = environ.get("RA_API_KEY", "").strip()
    if not key:
        raise ImportError_(
            "no web API key: set RA_API_KEY or pass --key-file "
            "(find yours at https://retroachievements.org/settings)"
        )
    return key


class Redactor:
    """Keeps the API key out of every message and log line."""

    def __init__(self, secret: str = ""):
        self.secret = secret

    def __call__(self, text: str) -> str:
        if self.secret:
            text = text.replace(self.secret, "<key>")
            text = text.replace(urllib.parse.quote(self.secret, safe=""), "<key>")
        return text


# ---------------------------------------------------------------------------
# HTTP


class Http:
    """urllib with a User-Agent, a minimum gap between requests and retries."""

    def __init__(
        self,
        min_interval: float = 1.0,
        timeout: float = 30.0,
        retries: int = 3,
        opener: Callable[..., Any] | None = None,
        sleep: Callable[[float], None] = time.sleep,
        clock: Callable[[], float] = time.monotonic,
        redact: Redactor | None = None,
        log: Callable[[str], None] | None = None,
    ):
        self.min_interval = min_interval
        self.timeout = timeout
        self.retries = retries
        self.opener = opener or urllib.request.urlopen
        self.sleep = sleep
        self.clock = clock
        self.redact = redact or Redactor()
        self.log = log or (lambda msg: None)
        self._last = None  # type: float | None

    def _wait_turn(self) -> None:
        if self._last is not None:
            gap = self.min_interval - (self.clock() - self._last)
            if gap > 0:
                self.sleep(gap)
        self._last = self.clock()

    def get(self, url: str, what: str) -> bytes:
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        attempt = 0
        while True:
            self._wait_turn()
            try:
                with self.opener(request, timeout=self.timeout) as response:
                    return response.read()
            except urllib.error.HTTPError as e:
                code = e.code
                if code == 429 or 500 <= code < 600:
                    if attempt < self.retries:
                        delay = _retry_after(e) or (2.0 ** attempt) * max(self.min_interval, 1.0)
                        self.log(f"{what}: HTTP {code}, retrying in {delay:.0f}s")
                        self.sleep(delay)
                        attempt += 1
                        continue
                raise HttpError(code, f"{what}: HTTP {code} {self.redact(str(e.reason))}") from None
            except urllib.error.URLError as e:
                if attempt < self.retries:
                    self.log(f"{what}: {self.redact(str(e.reason))}, retrying")
                    self.sleep((2.0 ** attempt) * max(self.min_interval, 1.0))
                    attempt += 1
                    continue
                raise ImportError_(f"{what}: network error: {self.redact(str(e.reason))}") from None
            except TimeoutError:
                if attempt < self.retries:
                    attempt += 1
                    continue
                raise ImportError_(f"{what}: timed out") from None


class HttpError(ImportError_):
    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def _retry_after(error: urllib.error.HTTPError) -> float | None:
    value = error.headers.get("Retry-After") if error.headers else None
    try:
        return min(float(value), 300.0) if value else None
    except ValueError:
        return None


def api_url(game_id: int, key: str, unofficial: bool) -> str:
    params = {"i": str(game_id), "y": key}
    if unofficial:
        params["f"] = str(FLAG_UNOFFICIAL)
    return API_URL + "?" + urllib.parse.urlencode(params)


def fetch_game(http: Http, game_id: int, key: str, unofficial: bool) -> dict:
    try:
        body = http.get(api_url(game_id, key, unofficial), f"game {game_id}")
    except HttpError as e:
        if e.code in (401, 403):
            raise ImportError_(
                f"game {game_id}: the API key was rejected (HTTP {e.code}); "
                "check RA_API_KEY / --key-file"
            ) from None
        if e.code == 404:
            raise ImportError_(f"game {game_id}: not found on RetroAchievements") from None
        raise
    return parse_response(body, f"game {game_id}")


def parse_response(body: bytes, what: str) -> dict:
    try:
        data = json.loads(body.decode("utf-8-sig"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise ImportError_(f"{what}: the response is not JSON ({e})") from None
    if not isinstance(data, dict):
        raise ImportError_(f"{what}: unexpected response (expected a JSON object)")
    # The API answers unknown ids with an empty object or a null ID.
    if not data or _get(data, "ID") in (None, 0, "0", ""):
        message = _get(data, "Error") or _get(data, "Message")
        if message:
            raise ImportError_(f"{what}: {message}")
        raise ImportError_(f"{what}: no such game (empty response)")
    return data


# ---------------------------------------------------------------------------
# Response normalisation (PascalCase and camelCase)


def _alt_names(name: str) -> Iterable[str]:
    yield name
    yield name[:1].lower() + name[1:]  # camelCase
    yield name.lower()
    if name.endswith("ID"):  # ConsoleID -> consoleId
        yield name[:1].lower() + name[1:-2] + "Id"
    if name == "ID":
        yield "id"


def _get(obj: dict, name: str, default: Any = None) -> Any:
    for candidate in _alt_names(name):
        if candidate in obj:
            return obj[candidate]
    return default


def _int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        try:
            return int(float(value))
        except (TypeError, ValueError):
            return default


def _text(value: Any) -> str:
    return "" if value is None else str(value).strip()


def user_url(name: str) -> str:
    return f"{SITE}/user/{urllib.parse.quote(name, safe='')}"


def game_url(game_id: int) -> str:
    return f"{SITE}/game/{game_id}"


def achievement_url(achievement_id: int) -> str:
    return f"{SITE}/achievement/{achievement_id}"


def normalize_achievements(game: dict) -> list[dict]:
    raw = _get(game, "Achievements") or {}
    # PHP serialises an empty map as [], a full one as {"<id>": {...}}.
    items = list(raw.values()) if isinstance(raw, dict) else list(raw)
    out = []
    for item in items:
        if not isinstance(item, dict):
            continue
        ach_id = _int(_get(item, "ID"))
        if ach_id <= 0:
            continue
        badge = _text(_get(item, "BadgeName"))
        if badge and not _BADGE_RE.match(badge):
            raise ImportError_(f"achievement {ach_id}: unexpected badge name {badge!r}")
        mem = _text(_get(item, "MemAddr"))
        out.append(
            {
                "id": ach_id,
                "title": _text(_get(item, "Title")),
                "description": _text(_get(item, "Description")),
                "points": max(0, _int(_get(item, "Points"))),
                "display_order": max(0, _int(_get(item, "DisplayOrder"))),
                "badge": badge,
                "author": _text(_get(item, "Author")),
                "type": _text(_get(item, "Type")),
                "logic_md5": mem.lower() if _MD5_RE.match(mem) else "",
            }
        )
    out.sort(key=lambda a: (a["display_order"], a["id"]))
    return out


# ---------------------------------------------------------------------------
# Building the set


def author_credit(author: str) -> dict:
    return {"name": author, "role": "author", "url": user_url(author)}


def ra_credit(game_id: int) -> dict:
    return {
        "name": "RetroAchievements",
        "role": "source",
        "url": game_url(game_id),
        "_note": "Achievement list written by the RetroAchievements community; "
        "titles, descriptions and badges belong to their authors.",
    }


def build_achievement(a: dict) -> dict:
    entry: dict[str, Any] = {
        "id": str(a["id"]),
        "platform_id": a["id"],
        "name": a["title"],
        "description": a["description"],
        "points": a["points"],
        "display_order": a["display_order"],
    }
    if a["badge"]:
        entry["icon"] = f"ra:{a['badge']}"
        entry["locked_icon"] = f"ra:{a['badge']}_lock"
    entry["credits"] = [author_credit(a["author"])] if a["author"] else []
    if a["type"]:
        entry["_ra_type"] = a["type"]
    if a["logic_md5"]:
        entry["_ra_logic_md5"] = a["logic_md5"]
    entry["trigger"] = {"type": "manual"}
    return entry


def set_credits(achievements: list[dict], game_id: int) -> list[dict]:
    # Authors in order of how many achievements they wrote, then name.
    counts: dict[str, int] = {}
    for a in achievements:
        if a["author"]:
            counts[a["author"]] = counts.get(a["author"], 0) + 1
    authors = sorted(counts, key=lambda n: (-counts[n], n.lower()))
    return [author_credit(n) for n in authors] + [ra_credit(game_id)]


def build_source(game: dict, game_id: int, unofficial: bool, fetched: str,
                 ra_hash: str | None, badge_dir: str | None) -> dict:
    source: dict[str, Any] = {
        "kind": "retroachievements",
        "game_id": game_id,
        "console": _text(_get(game, "ConsoleName")),
        "console_id": _int(_get(game, "ConsoleID")),
        "url": game_url(game_id),
        "fetched": fetched,
        "achievement_flags": FLAG_UNOFFICIAL if unofficial else FLAG_OFFICIAL,
        "tool": f"ra_import.py {TOOL_VERSION}",
    }
    parent = _int(_get(game, "ParentGameID"))
    if parent:
        source["parent_game_id"] = parent
    if ra_hash:
        source["ra_hash"] = ra_hash
    if badge_dir:
        source["badge_dir"] = badge_dir
    return source


def build_set(game: dict, *, set_id: str | None = None, category: str | None = None,
              unofficial: bool = False, fetched: str | None = None,
              ra_hash: str | None = None, badge_dir: str | None = None) -> dict:
    game_id = _int(_get(game, "ID"))
    achievements = normalize_achievements(game)
    title = _text(_get(game, "Title")) or f"RetroAchievements game {game_id}"
    return {
        "schema": SCHEMA,
        "set": {
            "id": set_id or default_set_id(game_id, unofficial),
            "title": title,
            "platform": "retroachievements",
            "category": category if category is not None else title,
            "credits": set_credits(achievements, game_id),
            "source": build_source(game, game_id, unofficial, fetched or now_iso(),
                                   ra_hash, badge_dir),
        },
        "achievements": [build_achievement(a) for a in achievements],
    }


def default_set_id(game_id: int, unofficial: bool) -> str:
    return f"ra-{game_id}" + ("-unofficial" if unofficial else "")


def now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace(
        "+00:00", "Z")


# ---------------------------------------------------------------------------
# Merging into an existing set


class MergeReport:
    def __init__(self) -> None:
        self.added: list[dict] = []
        self.removed: list[dict] = []
        self.changed: list[tuple[dict, list[str]]] = []
        self.logic_changed: list[dict] = []

    def lines(self) -> list[str]:
        out = []
        for a in self.added:
            out.append(f"added    {a['platform_id']:>8}  {a['name']}")
        for a in self.removed:
            out.append(f"removed  {a.get('platform_id', '?'):>8}  {a.get('name', '')}")
        for a, fields in self.changed:
            out.append(f"changed  {a['platform_id']:>8}  {a['name']} ({', '.join(fields)})")
        for a in self.logic_changed:
            out.append(
                f"logic    {a['platform_id']:>8}  {a['name']}: RetroAchievements changed "
                "this achievement's conditions; check the recomp's trigger"
            )
        out.append(
            f"{len(self.added)} added, {len(self.removed)} removed, "
            f"{len(self.changed)} changed, {len(self.logic_changed)} with new logic"
        )
        return out


def _platform_id(entry: dict) -> int:
    # Only entries with a platform_id came from RetroAchievements; the recomp's
    # own extra achievements have none and are never touched.
    return max(0, _int(entry.get("platform_id")))


def _merge_credits(existing: list | None, fresh: list[dict], owned_roles: set[str],
                   owned_names: set[str] = frozenset()) -> list:
    # The import owns "author" credits (and the RetroAchievements one); credits
    # the recomp added (port, artwork...) stay after them.
    kept = [
        c for c in (existing or [])
        if not (isinstance(c, dict) and (c.get("role") in owned_roles or
                                         c.get("name") in owned_names))
    ]
    return fresh + kept


def merge_sets(existing: dict, fresh: dict, drop_removed: bool = False) -> tuple[dict, MergeReport]:
    if not isinstance(existing, dict) or not isinstance(existing.get("achievements"), list):
        raise ImportError_("--merge file is not a recomp-achievements set (no achievements list)")
    report = MergeReport()
    result = json.loads(json.dumps(existing))  # Deep copy; keeps unknown fields.

    old_set = result.setdefault("set", {})
    new_set = fresh["set"]
    old_source = old_set.get("source") if isinstance(old_set.get("source"), dict) else {}
    if old_source.get("kind") == "retroachievements" and \
            _int(old_source.get("game_id")) not in (0, new_set["source"]["game_id"]):
        raise ImportError_(
            f"--merge file is for RetroAchievements game {old_source.get('game_id')}, "
            f"not {new_set['source']['game_id']}"
        )
    result.setdefault("schema", SCHEMA)
    for field in ("id", "title", "platform", "category"):
        old_set.setdefault(field, new_set[field])
    old_set["credits"] = _merge_credits(old_set.get("credits"), new_set["credits"],
                                        {"author", "source"}, {"RetroAchievements"})
    source = dict(old_source)
    source.update(new_set["source"])
    if "badge_dir" in old_source and "badge_dir" not in new_set["source"]:
        source["badge_dir"] = old_source["badge_dir"]
    old_set["source"] = source

    fresh_by_id = {a["platform_id"]: a for a in fresh["achievements"]}
    seen: set[int] = set()
    merged = []
    for entry in result["achievements"]:
        if not isinstance(entry, dict):
            merged.append(entry)
            continue
        pid = _platform_id(entry)
        new = fresh_by_id.get(pid)
        if new is None:
            if pid > 0:
                report.removed.append(entry)
                if drop_removed:
                    continue
                entry["_ra_removed"] = True
            merged.append(entry)
            continue
        seen.add(pid)
        entry.pop("_ra_removed", None)
        changed = [f for f in ("name", "description", "points") if entry.get(f) != new[f]]
        if entry.get("_ra_logic_md5") and new.get("_ra_logic_md5") and \
                entry["_ra_logic_md5"] != new["_ra_logic_md5"] and \
                (entry.get("trigger") or {}).get("type", "manual") != "manual":
            report.logic_changed.append(new)
        for field in _REFRESHED_FIELDS:
            if field in new:
                entry[field] = new[field]
            else:
                entry.pop(field, None)
        entry["platform_id"] = pid
        # Icons are the recomp's unless they still point at the old RA badge.
        old_badge_icon = _is_ra_badge(entry.get("icon"))
        if "icon" in new and (old_badge_icon or "icon" not in entry):
            if entry.get("icon") != new["icon"]:
                changed.append("badge")
            entry["icon"] = new["icon"]
            if _is_ra_badge(entry.get("locked_icon")) or "locked_icon" not in entry:
                entry["locked_icon"] = new["locked_icon"]
        entry["credits"] = _merge_credits(entry.get("credits"), new["credits"], {"author"})
        entry.setdefault("trigger", {"type": "manual"})
        if changed:
            report.changed.append((new, changed))
        merged.append(entry)

    for new in fresh["achievements"]:
        if new["platform_id"] not in seen:
            merged.append(new)
            report.added.append(new)
    result["achievements"] = merged
    return result, report


def _is_ra_badge(icon: Any) -> bool:
    return isinstance(icon, str) and icon.startswith("ra:")



# ---------------------------------------------------------------------------
# Badges


def download_badges(http: Http, badges: Iterable[str], out_dir: str, refresh: bool = False,
                    log: Callable[[str], None] = lambda m: None) -> tuple[int, int]:
    """Saves <badge>.png and <badge>_lock.png. Returns (downloaded, skipped)."""
    os.makedirs(out_dir, exist_ok=True)
    downloaded = skipped = 0
    for badge in sorted(set(badges)):
        if not _BADGE_RE.match(badge):
            raise ImportError_(f"refusing unexpected badge name {badge!r}")
        for name in (badge, badge + "_lock"):
            path = os.path.join(out_dir, name + ".png")
            if not refresh and os.path.exists(path):
                skipped += 1
                continue
            data = http.get(BADGE_URL.format(name=name), f"badge {name}")
            if not data.startswith(b"\x89PNG\r\n\x1a\n"):
                raise ImportError_(f"badge {name}: the download is not a PNG")
            _write_atomic(path, data)
            downloaded += 1
            log(f"badge {name}.png")
    return downloaded, skipped


# ---------------------------------------------------------------------------
# Output


def _write_atomic(path: str, data: bytes) -> None:
    folder = os.path.dirname(os.path.abspath(path))
    os.makedirs(folder, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=folder, prefix=".ra_import-", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def dump_set(doc: dict) -> bytes:
    return (json.dumps(doc, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def load_json_file(path: str, what: str) -> Any:
    try:
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except OSError as e:
        raise ImportError_(f"cannot read {what} {path}: {e.strerror}") from None
    except json.JSONDecodeError as e:
        raise ImportError_(f"{what} {path} is not valid JSON: {e}") from None


# ---------------------------------------------------------------------------
# Command line


_FORBIDDEN_FLAGS = ("--key", "--api-key", "--apikey", "-y", "--y")


def _refuse_key_arguments(argv: list[str]) -> None:
    # Checked before argparse, whose "unrecognized arguments" error would echo
    # the key back to the terminal.
    for arg in argv:
        flag = arg.split("=", 1)[0]
        if flag in _FORBIDDEN_FLAGS:
            raise ImportError_(
                "the API key is not accepted on the command line (it would end up in "
                "shell history); set RA_API_KEY or use --key-file"
            )


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="ra_import.py",
        description="Copy a public RetroAchievements achievement list into a "
        "recomp-achievements set file. Imported achievements have no unlock "
        "logic; the recomp adds its own triggers.",
        allow_abbrev=False,
    )
    p.add_argument("game_id", type=int, help="RetroAchievements game id (from the game page URL)")
    p.add_argument("-o", "--output", help="set file to write (default with --merge: the merged file)")
    p.add_argument("--merge", metavar="FILE",
                   help="existing set to refresh: names, descriptions, points and credits are "
                   "updated; triggers, icons and extra fields are kept")
    p.add_argument("--drop-removed", action="store_true",
                   help="with --merge, delete achievements RetroAchievements no longer lists "
                   "(default: keep them, marked _ra_removed)")
    p.add_argument("--key-file", help="file whose first line is the web API key (default: $RA_API_KEY)")
    p.add_argument("--input", metavar="FILE",
                   help="read a saved API_GetGameExtended response instead of fetching")
    p.add_argument("--unofficial", action="store_true",
                   help="import the unofficial (flag 5) achievements instead of the core set")
    p.add_argument("--set-id", help="set id (default ra-<game id>)")
    p.add_argument("--category", help="default category (default: the game title)")
    p.add_argument("--ra-hash", help="RetroAchievements hash of the game version the recomp is "
                   "built from, recorded in source.ra_hash")
    p.add_argument("--badges", metavar="DIR",
                   help="download badge PNGs (<badge>.png, <badge>_lock.png) into DIR")
    p.add_argument("--refresh-badges", action="store_true", help="re-download existing badges")
    p.add_argument("--delay", type=float, default=1.0,
                   help="minimum seconds between requests (default 1.0)")
    p.add_argument("--dry-run", action="store_true", help="print what would change, write nothing")
    p.add_argument("-q", "--quiet", action="store_true")
    return p


def main(argv: list[str] | None = None, *, opener: Callable[..., Any] | None = None,
         environ: dict[str, str] | None = None, sleep: Callable[[float], None] = time.sleep,
         stderr=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    stderr = stderr or sys.stderr
    redact = Redactor()

    def say(msg: str) -> None:
        print(redact(msg), file=stderr)

    try:
        _refuse_key_arguments(argv)
        args = build_parser().parse_args(argv)
        log = (lambda m: None) if args.quiet else say
        if args.game_id <= 0:
            raise ImportError_("game_id must be a positive number")
        if args.delay < 0:
            raise ImportError_("--delay must not be negative")
        output = args.output or args.merge
        if not output and not args.dry_run:
            raise ImportError_("pass -o/--output (or --merge to update a set in place)")

        need_network = not args.input or args.badges
        key = ""
        if not args.input:
            key = read_api_key(args.key_file, environ)
            redact.secret = key
        http = Http(min_interval=args.delay, opener=opener, sleep=sleep, redact=redact, log=log) \
            if need_network else None

        if args.input:
            with open(args.input, "rb") as f:
                game = parse_response(f.read(), args.input)
            if _int(_get(game, "ID")) != args.game_id:
                raise ImportError_(
                    f"{args.input} is game {_get(game, 'ID')}, not {args.game_id}")
        else:
            log(f"fetching game {args.game_id} from RetroAchievements")
            game = fetch_game(http, args.game_id, key, args.unofficial)

        badge_dir = None
        if args.badges and output:
            badge_dir = os.path.relpath(os.path.abspath(args.badges),
                                        os.path.dirname(os.path.abspath(output)))
            badge_dir = badge_dir.replace(os.sep, "/")
        fresh = build_set(game, set_id=args.set_id, category=args.category,
                          unofficial=args.unofficial, ra_hash=args.ra_hash, badge_dir=badge_dir)
        if not fresh["achievements"]:
            log(f"warning: game {args.game_id} has no "
                f"{'unofficial' if args.unofficial else 'core'} achievements")

        doc = fresh
        if args.merge:
            existing = load_json_file(args.merge, "--merge file")
            doc, report = merge_sets(existing, fresh, drop_removed=args.drop_removed)
            for line in report.lines():
                log(line)

        if args.dry_run:
            log(f"dry run: {len(doc['achievements'])} achievements, nothing written")
            return 0

        if args.badges:
            badges = [a["badge"] for a in normalize_achievements(game) if a["badge"]]
            done, skipped = download_badges(http, badges, args.badges, args.refresh_badges, log)
            log(f"badges: {done} downloaded, {skipped} already present")

        _write_atomic(output, dump_set(doc))
        log(f"wrote {output}: {len(doc['achievements'])} achievements "
            f"({sum(_int(a.get('points')) for a in doc['achievements'] if isinstance(a, dict))} points)")
        return 0
    except ImportError_ as e:
        say(f"ra_import: error: {e}")
        return 1
    except OSError as e:
        say(f"ra_import: error: {e.filename or ''}: {e.strerror}")
        return 1
    except KeyboardInterrupt:
        say("ra_import: interrupted")
        return 130


if __name__ == "__main__":
    sys.exit(main())
