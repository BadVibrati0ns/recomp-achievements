"""Tests for tools/ra_import.py. Offline: every response comes from fixtures/.

Run: python3 -m unittest discover -s tools/tests
"""

from __future__ import annotations

import copy
import email.message
import io
import json
import os
import sys
import tempfile
import unittest
import urllib.error
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import ra_import  # noqa: E402

FIXTURES = os.path.join(HERE, "fixtures")
FETCHED = "2026-11-03T12:00:00Z"
SECRET = "Zx9SecretKeyForTests0000"
PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 24


def fixture_bytes(name: str) -> bytes:
    with open(os.path.join(FIXTURES, name), "rb") as f:
        return f.read()


def fixture(name: str) -> dict:
    return json.loads(fixture_bytes(name))


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class FakeOpener:
    """Stands in for urllib.request.urlopen. `routes` maps a URL prefix to
    bytes, an exception, or a list of either (one per call)."""

    def __init__(self, routes: dict):
        self.routes = routes
        self.requests = []

    def __call__(self, request, timeout=None):
        url = request.full_url
        self.requests.append(request)
        for prefix, result in self.routes.items():
            if url.startswith(prefix):
                if isinstance(result, list):
                    result = result.pop(0)
                if isinstance(result, Exception):
                    raise result
                return FakeResponse(result)
        raise urllib.error.HTTPError(url, 404, "Not Found", email.message.Message(), None)


def http_error(code: int, url: str, retry_after: str | None = None) -> urllib.error.HTTPError:
    headers = email.message.Message()
    if retry_after:
        headers["Retry-After"] = retry_after
    return urllib.error.HTTPError(url, code, "error", headers, None)


def run_main(argv, opener=None, environ=None):
    err = io.StringIO()
    sleeps = []
    code = ra_import.main(argv, opener=opener, environ=environ or {}, sleep=sleeps.append,
                          stderr=err)
    return code, err.getvalue(), sleeps


class KeyTests(unittest.TestCase):
    def test_env(self):
        self.assertEqual(ra_import.read_api_key(None, {"RA_API_KEY": " abc \n"}), "abc")

    def test_key_file_first_line_skips_comments(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "key")
            with open(path, "w") as f:
                f.write("# my key\n\n  filekey  \nignored\n")
            self.assertEqual(ra_import.read_api_key(path, {"RA_API_KEY": "env"}), "filekey")

    def test_missing(self):
        with self.assertRaises(ra_import.ImportError_):
            ra_import.read_api_key(None, {})
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(ra_import.ImportError_):
                ra_import.read_api_key(os.path.join(d, "nope"), {})

    def test_refused_on_command_line_without_echo(self):
        for argv in (["1234", "--key", SECRET], ["1234", f"--api-key={SECRET}"],
                     ["1234", "-y", SECRET]):
            code, err, _ = run_main(argv + ["-o", os.devnull])
            self.assertEqual(code, 1)
            self.assertNotIn(SECRET, err)
            self.assertIn("RA_API_KEY", err)

    def test_redactor(self):
        r = ra_import.Redactor("a+b/c")
        self.assertEqual(r("x a+b/c y a%2Bb%2Fc"), "x <key> y <key>")


class ResponseTests(unittest.TestCase):
    def test_api_url(self):
        url = ra_import.api_url(1234, "k&y", unofficial=False)
        q = urllib.parse.parse_qs(urllib.parse.urlparse(url).query)
        self.assertEqual(q, {"i": ["1234"], "y": ["k&y"]})
        self.assertTrue(url.startswith("https://retroachievements.org/API/API_GetGameExtended.php?"))
        q = urllib.parse.parse_qs(urllib.parse.urlparse(
            ra_import.api_url(1, "k", unofficial=True)).query)
        self.assertEqual(q["f"], ["5"])

    def test_unknown_game(self):
        for body in (b"{}", b'{"ID": null}', b'{"id": 0, "title": null}'):
            with self.assertRaises(ra_import.ImportError_):
                ra_import.parse_response(body, "game 1")

    def test_not_json(self):
        with self.assertRaisesRegex(ra_import.ImportError_, "not JSON"):
            ra_import.parse_response(b"<html>Bad gateway</html>", "game 1")
        with self.assertRaises(ra_import.ImportError_):
            ra_import.parse_response(b"[1, 2]", "game 1")

    def test_bad_badge_name_rejected(self):
        game = fixture("game_1234_pascal.json")
        game["Achievements"]["90001"]["BadgeName"] = "../../etc/passwd"
        with self.assertRaisesRegex(ra_import.ImportError_, "badge"):
            ra_import.normalize_achievements(game)


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.doc = ra_import.build_set(fixture("game_1234_pascal.json"), fetched=FETCHED)

    def test_set_header(self):
        s = self.doc["set"]
        self.assertEqual(self.doc["schema"], "recomp-achievements/1")
        self.assertEqual(s["id"], "ra-1234")
        self.assertEqual(s["title"], "Star Quest 64")
        self.assertEqual(s["platform"], "retroachievements")
        self.assertEqual(s["category"], "Star Quest 64")
        self.assertEqual(s["source"], {
            "kind": "retroachievements",
            "game_id": 1234,
            "console": "Nintendo 64",
            "console_id": 2,
            "url": "https://retroachievements.org/game/1234",
            "fetched": FETCHED,
            "achievement_flags": 3,
            "tool": f"ra_import.py {ra_import.TOOL_VERSION}",
        })

    def test_set_credits(self):
        credits = self.doc["set"]["credits"]
        # Most achievements first, then by name; RetroAchievements last.
        self.assertEqual([c["name"] for c in credits],
                         ["PixelScribe", "memwatch", "Señor Bits", "RetroAchievements"])
        self.assertEqual(credits[0], {"name": "PixelScribe", "role": "author",
                                      "url": "https://retroachievements.org/user/PixelScribe"})
        self.assertEqual(credits[2]["url"], "https://retroachievements.org/user/Se%C3%B1or%20Bits")
        ra = credits[-1]
        self.assertEqual(ra["url"], "https://retroachievements.org/game/1234")
        self.assertIn("RetroAchievements community", ra["_note"])

    def test_achievements(self):
        achs = self.doc["achievements"]
        # Display order, then id for ties.
        self.assertEqual([a["platform_id"] for a in achs], [90001, 90002, 90003, 90004])
        first = achs[0]
        self.assertEqual(first, {
            "id": "90001",
            "platform_id": 90001,
            "name": "First Steps",
            "description": "Complete the tutorial",
            "points": 5,
            "display_order": 1,
            "icon": "ra:301001",
            "locked_icon": "ra:301001_lock",
            "credits": [{"name": "PixelScribe", "role": "author",
                         "url": "https://retroachievements.org/user/PixelScribe"}],
            "_ra_type": "progression",
            "_ra_logic_md5": first["_ra_logic_md5"],
            "trigger": {"type": "manual"},
        })
        self.assertRegex(first["_ra_logic_md5"], "^[0-9a-f]{32}$")
        self.assertNotIn("_ra_type", achs[2])  # type null in the response
        self.assertEqual(achs[3]["_ra_type"], "missable")
        for a in achs:
            self.assertEqual(a["trigger"], {"type": "manual"})

    def test_camel_case_matches_pascal_case(self):
        camel = ra_import.build_set(fixture("game_1234_camel.json"), fetched=FETCHED)
        self.assertEqual(camel, self.doc)

    def test_options(self):
        doc = ra_import.build_set(fixture("game_1234_pascal.json"), fetched=FETCHED,
                                  set_id="sq64", category="Main", unofficial=True,
                                  ra_hash="0123456789abcdef0123456789abcdef", badge_dir="badges")
        self.assertEqual(doc["set"]["id"], "sq64")
        self.assertEqual(doc["set"]["category"], "Main")
        self.assertEqual(doc["set"]["source"]["achievement_flags"], 5)
        self.assertEqual(doc["set"]["source"]["ra_hash"], "0123456789abcdef0123456789abcdef")
        self.assertEqual(doc["set"]["source"]["badge_dir"], "badges")
        self.assertEqual(ra_import.default_set_id(1234, True), "ra-1234-unofficial")

    def test_empty_list(self):
        doc = ra_import.build_set(fixture("game_5678_empty.json"), fetched=FETCHED)
        self.assertEqual(doc["achievements"], [])
        self.assertEqual([c["name"] for c in doc["set"]["credits"]], ["RetroAchievements"])

    def test_string_numbers_and_missing_fields(self):
        game = {"ID": "77", "Title": "T", "Achievements": {
            "5": {"ID": "5", "Title": "A", "Points": "10", "DisplayOrder": None},
        }}
        a = ra_import.build_set(game, fetched=FETCHED)["achievements"][0]
        self.assertEqual((a["platform_id"], a["points"], a["display_order"]), (5, 10, 0))
        self.assertEqual(a["credits"], [])
        self.assertNotIn("icon", a)


class MergeTests(unittest.TestCase):
    def setUp(self):
        self.old = ra_import.build_set(fixture("game_1234_pascal.json"), fetched=FETCHED)
        # What a recomp does to an imported set.
        by_id = {a["platform_id"]: a for a in self.old["achievements"]}
        by_id[90001]["trigger"] = {"type": "event", "event": "tutorial_done"}
        by_id[90003]["trigger"] = {"type": "memory",
                                   "params": {"condition": "M:0xH0040>=10_R:0xH0041=1"}}
        by_id[90004]["icon"] = "icons/hidden_room.png"
        by_id[90004]["secret"] = True
        by_id[90004]["credits"].append({"name": "Porter", "role": "port"})
        self.old["achievements"].append({
            "id": "recomp_bonus", "name": "Recomp Bonus", "description": "Our own",
            "points": 0, "trigger": {"type": "manual"}})
        self.old["set"]["credits"].append({"name": "Porter", "role": "port"})
        self.old["set"]["points_cap"] = 1000
        self.old["set"]["id"] = "starquest"
        self.old["extra"] = {"keep": True}
        self.fresh = ra_import.build_set(fixture("game_1234_updated.json"),
                                         fetched="2026-12-01T00:00:00Z")

    def merged(self, **kw):
        return ra_import.merge_sets(copy.deepcopy(self.old), self.fresh, **kw)

    def test_keeps_recomp_fields(self):
        doc, _ = self.merged()
        by_id = {a.get("platform_id", a["id"]): a for a in doc["achievements"]}
        self.assertEqual(by_id[90001]["trigger"], {"type": "event", "event": "tutorial_done"})
        self.assertEqual(by_id[90003]["trigger"]["type"], "memory")
        self.assertEqual(by_id[90004]["icon"], "icons/hidden_room.png")
        self.assertTrue(by_id[90004]["secret"])
        self.assertEqual(by_id[90004]["credits"][-1], {"name": "Porter", "role": "port"})
        self.assertEqual(by_id["recomp_bonus"]["name"], "Recomp Bonus")
        self.assertEqual(doc["extra"], {"keep": True})
        self.assertEqual(doc["set"]["points_cap"], 1000)
        self.assertEqual(doc["set"]["id"], "starquest")
        self.assertEqual(doc["set"]["credits"][-1], {"name": "Porter", "role": "port"})
        self.assertEqual(doc["set"]["source"]["fetched"], "2026-12-01T00:00:00Z")

    def test_refreshes_list_fields(self):
        doc, report = self.merged()
        by_id = {a.get("platform_id"): a for a in doc["achievements"]}
        self.assertEqual(by_id[90001]["name"], "First Steps!")
        self.assertEqual(by_id[90001]["points"], 10)
        # An icon still pointing at RA's badge follows the new badge.
        self.assertEqual(by_id[90003]["icon"], "ra:301099")
        self.assertEqual(by_id[90003]["locked_icon"], "ra:301099_lock")
        changed = {a["platform_id"]: fields for a, fields in report.changed}
        self.assertEqual(changed, {90001: ["name", "points"], 90003: ["badge"]})

    def test_reports_added_removed_logic(self):
        doc, report = self.merged()
        self.assertEqual([a["platform_id"] for a in report.added], [90005])
        self.assertEqual([a["platform_id"] for a in report.removed], [90002])
        # Logic changes matter only where the recomp wrote a trigger.
        self.assertEqual([a["platform_id"] for a in report.logic_changed], [90003])
        removed = [a for a in doc["achievements"] if a.get("platform_id") == 90002][0]
        self.assertTrue(removed["_ra_removed"])
        added = doc["achievements"][-1]
        self.assertEqual((added["platform_id"], added["trigger"]), (90005, {"type": "manual"}))
        text = "\n".join(report.lines())
        self.assertIn("1 added, 1 removed, 2 changed, 1 with new logic", text)

    def test_drop_removed(self):
        doc, _ = self.merged(drop_removed=True)
        ids = [a.get("platform_id") for a in doc["achievements"]]
        self.assertNotIn(90002, ids)
        self.assertIn(None, ids)  # The recomp's own achievement survives.

    def test_readded_achievement_loses_removed_flag(self):
        doc, _ = self.merged()
        original = ra_import.build_set(fixture("game_1234_pascal.json"), fetched=FETCHED)
        again, report = ra_import.merge_sets(doc, original)
        entry = [a for a in again["achievements"] if a.get("platform_id") == 90002][0]
        self.assertNotIn("_ra_removed", entry)
        self.assertEqual(report.added, [])

    def test_idempotent(self):
        doc, _ = self.merged()
        again, report = ra_import.merge_sets(copy.deepcopy(doc), self.fresh)
        self.assertEqual(again, doc)
        self.assertEqual((report.added, report.changed, report.logic_changed), ([], [], []))
        # Still listed as removed: it stays in the file until --drop-removed.
        self.assertEqual([a["platform_id"] for a in report.removed], [90002])

    def test_wrong_game(self):
        other = copy.deepcopy(self.fresh)
        other["set"]["source"]["game_id"] = 999
        with self.assertRaisesRegex(ra_import.ImportError_, "game 1234"):
            ra_import.merge_sets(self.old, other)

    def test_not_a_set(self):
        with self.assertRaises(ra_import.ImportError_):
            ra_import.merge_sets({"foo": 1}, self.fresh)


class HttpTests(unittest.TestCase):
    def test_rate_limit_and_user_agent(self):
        opener = FakeOpener({"https://x/": [b"a", b"b"]})
        now = [100.0]
        sleeps = []
        http = ra_import.Http(min_interval=1.5, opener=opener, sleep=sleeps.append,
                              clock=lambda: now[0])
        http.get("https://x/1", "one")
        now[0] += 0.5
        http.get("https://x/2", "two")
        self.assertEqual(sleeps, [1.0])
        ua = opener.requests[0].get_header("User-agent")
        self.assertIn("recomp-achievements-ra_import", ua)

    def test_retry_after_429(self):
        url = "https://x/1"
        opener = FakeOpener({url: [http_error(429, url, "7"), b"ok"]})
        sleeps = []
        http = ra_import.Http(min_interval=0, opener=opener, sleep=sleeps.append)
        self.assertEqual(http.get(url, "one"), b"ok")
        self.assertIn(7.0, sleeps)

    def test_gives_up(self):
        url = "https://x/1"
        opener = FakeOpener({url: [http_error(503, url)] * 4})
        http = ra_import.Http(min_interval=0, retries=3, opener=opener, sleep=lambda s: None)
        with self.assertRaisesRegex(ra_import.ImportError_, "HTTP 503"):
            http.get(url, "one")
        self.assertEqual(len(opener.requests), 4)

    def test_network_error(self):
        url = "https://x/1"
        opener = FakeOpener({url: [urllib.error.URLError("no route")] * 2})
        http = ra_import.Http(min_interval=0, retries=1, opener=opener, sleep=lambda s: None)
        with self.assertRaisesRegex(ra_import.ImportError_, "network error"):
            http.get(url, "one")


class CliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name

    def tearDown(self):
        self.tmp.cleanup()

    def path(self, name):
        return os.path.join(self.dir, name)

    def read(self, name):
        with open(self.path(name), encoding="utf-8") as f:
            return f.read()

    def test_input_file(self):
        code, err, _ = run_main(["1234", "--input", os.path.join(FIXTURES, "game_1234_pascal.json"),
                                 "-o", self.path("out.json")])
        self.assertEqual(code, 0, err)
        doc = json.loads(self.read("out.json"))
        self.assertEqual(len(doc["achievements"]), 4)
        self.assertIn("wrote", err)

    def test_input_wrong_game(self):
        code, err, _ = run_main(["99", "--input", os.path.join(FIXTURES, "game_1234_pascal.json"),
                                 "-o", self.path("out.json")])
        self.assertEqual(code, 1)
        self.assertIn("not 99", err)

    def test_fetch_keeps_key_secret(self):
        opener = FakeOpener({ra_import.API_URL: fixture_bytes("game_1234_pascal.json")})
        code, err, _ = run_main(["1234", "-o", self.path("out.json")], opener=opener,
                                environ={"RA_API_KEY": SECRET})
        self.assertEqual(code, 0, err)
        query = urllib.parse.parse_qs(urllib.parse.urlparse(opener.requests[0].full_url).query)
        self.assertEqual(query, {"i": ["1234"], "y": [SECRET]})
        self.assertNotIn(SECRET, err)
        self.assertNotIn(SECRET, self.read("out.json"))

    def test_key_file(self):
        with open(self.path("key"), "w") as f:
            f.write(SECRET + "\n")
        opener = FakeOpener({ra_import.API_URL: fixture_bytes("game_1234_camel.json")})
        code, err, _ = run_main(["1234", "--key-file", self.path("key"), "--unofficial",
                                 "-o", self.path("out.json")], opener=opener)
        self.assertEqual(code, 0, err)
        self.assertIn("f=5", opener.requests[0].full_url)
        self.assertEqual(json.loads(self.read("out.json"))["set"]["id"], "ra-1234-unofficial")

    def test_rejected_key(self):
        url = ra_import.api_url(1234, SECRET, False)
        opener = FakeOpener({ra_import.API_URL: http_error(401, url)})
        code, err, _ = run_main(["1234", "-o", self.path("out.json")], opener=opener,
                                environ={"RA_API_KEY": SECRET})
        self.assertEqual(code, 1)
        self.assertIn("rejected", err)
        self.assertNotIn(SECRET, err)
        self.assertFalse(os.path.exists(self.path("out.json")))

    def test_unknown_game(self):
        opener = FakeOpener({ra_import.API_URL: b"{}"})
        code, err, _ = run_main(["1", "-o", self.path("out.json")], opener=opener,
                                environ={"RA_API_KEY": SECRET})
        self.assertEqual(code, 1)
        self.assertIn("no such game", err)

    def test_merge_in_place(self):
        with open(self.path("set.json"), "w", encoding="utf-8") as f:
            json.dump(ra_import.build_set(fixture("game_1234_pascal.json"), fetched=FETCHED), f)
        code, err, _ = run_main(["1234", "--input", os.path.join(FIXTURES, "game_1234_updated.json"),
                                 "--merge", self.path("set.json")])
        self.assertEqual(code, 0, err)
        self.assertIn("added       90005  Speed Demon", err)
        self.assertIn("removed     90002  Castle Cleared", err)
        doc = json.loads(self.read("set.json"))
        self.assertEqual(len(doc["achievements"]), 5)

    def test_dry_run_writes_nothing(self):
        code, err, _ = run_main(["1234", "--input", os.path.join(FIXTURES, "game_1234_pascal.json"),
                                 "--dry-run"])
        self.assertEqual(code, 0, err)
        self.assertEqual(os.listdir(self.dir), [])

    def test_needs_output(self):
        code, err, _ = run_main(["1234", "--input", os.path.join(FIXTURES, "game_1234_pascal.json")])
        self.assertEqual(code, 1)
        self.assertIn("--output", err)

    def test_badges(self):
        opener = FakeOpener({
            ra_import.API_URL: fixture_bytes("game_1234_pascal.json"),
            "https://media.retroachievements.org/Badge/": PNG,
        })
        env = {"RA_API_KEY": SECRET}
        argv = ["1234", "-o", self.path("sets/sq.json"), "--badges", self.path("badges"),
                "--delay", "0.25"]
        code, err, sleeps = run_main(argv, opener=opener, environ=env)
        self.assertEqual(code, 0, err)
        files = sorted(os.listdir(self.path("badges")))
        self.assertEqual(len(files), 8)
        self.assertIn("301001.png", files)
        self.assertIn("301001_lock.png", files)
        badge_urls = [r.full_url for r in opener.requests[1:]]
        self.assertIn("https://media.retroachievements.org/Badge/301001_lock.png", badge_urls)
        doc = json.loads(self.read("sets/sq.json"))
        self.assertEqual(doc["set"]["source"]["badge_dir"], "../badges")

        # Existing badges are not fetched again.
        opener.requests.clear()
        code, err, _ = run_main(argv, opener=opener, environ=env)
        self.assertEqual(code, 0, err)
        self.assertEqual(len(opener.requests), 1)
        self.assertIn("8 already present", err)

    def test_badge_not_png(self):
        opener = FakeOpener({
            ra_import.API_URL: fixture_bytes("game_1234_pascal.json"),
            "https://media.retroachievements.org/Badge/": b"<html>",
        })
        code, err, _ = run_main(["1234", "-o", self.path("o.json"), "--badges", self.path("b")],
                                opener=opener, environ={"RA_API_KEY": SECRET})
        self.assertEqual(code, 1)
        self.assertIn("not a PNG", err)


if __name__ == "__main__":
    unittest.main()
