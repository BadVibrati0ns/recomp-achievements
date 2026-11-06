// The library writer: layout, content, events, coalescing and failure modes.

#include <algorithm>
#include <chrono>
#include <thread>

#include "json_format.h"
#include "library_util.h"
#include "recomp/achievements/library.h"
#include "recomp/achievements/library_reader.h"
#include "recomp/achievements/system.h"
#include "test.h"

using namespace recomp::achievements;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const std::vector<uint8_t> kTinyPng = EncodePng(1, 1, std::vector<uint8_t>{10, 20, 30, 255});

Achievement Make(std::string id, uint32_t points, std::string type = "manual") {
  Achievement a;
  a.local_id = std::move(id);
  a.name = "Name " + a.local_id;
  a.description = "Description " + a.local_id;
  a.points = points;
  a.trigger.type = std::move(type);
  return a;
}

// A set with every field the export carries, and a PNG icon on disk.
AchievementSet MakeSet(const fs::path& folder) {
  test::WriteFile(folder / "icons" / "a.png", std::string(kTinyPng.begin(), kTinyPng.end()));
  AchievementSet set;
  set.id = "game";
  set.title = "My Game";
  set.platform = Platform::kPlayStation3;
  set.points_cap = 100;
  set.base_dir = folder;
  set.credits = {{"The recomp-achievements authors", "port", ""}};
  Achievement a = Make("a", 100);
  a.icon = "icons/a.png";
  a.tier = Tier::kGold;
  Achievement b = Make("b", 100, "checklist");
  b.trigger.event = "trick";
  b.checklist = {"Kickflip", "Heelflip", "Varial"};
  b.tier = Tier::kBronze;
  Achievement c = Make("c", 100);
  c.secret = true;
  c.locked_description = "Keep playing";
  Achievement d = Make("d", 100, "count");
  d.trigger.event = "land";
  d.trigger.threshold = 1000;
  set.achievements = {a, b, c, d};
  return set;
}

LibraryOptions Options(const fs::path& root) {
  LibraryOptions o;
  o.game_id = "my_game";
  o.title = "My Game";
  o.version = "1.2";
  o.root = root;
  o.wine_host = false;
  return o;
}

std::vector<json> EventLines(const fs::path& file) {
  std::vector<json> out;
  std::istringstream in(test::ReadFile(file));
  std::string line;
  while (std::getline(in, line))
    if (!line.empty()) out.push_back(json::parse(line));
  return out;
}

size_t CountFiles(const fs::path& dir, std::string_view fragment) {
  size_t n = 0;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(dir, ec))
    if (e.path().filename().string().find(fragment) != std::string::npos) ++n;
  return n;
}

}  // namespace

TEST(library_export_layout_and_content) {
  const fs::path tmp = test::TempDir("library_export");
  const fs::path root = tmp / "library";
  System sys;
  sys.AddSet(MakeSet(tmp / "game_files"));
  {
    Library lib(sys, Options(root));
    CHECK(lib.enabled());
    CHECK(lib.dir() == root / "my_game");
    CHECK(sys.Unlock("game:a"));
    sys.ReportEvent("trick", "Kickflip");
    lib.Flush();
  }

  const json install = test::ReadJson(root / "install.json");
  const std::string id = install.value("install_id", "");
  CHECK_EQ(id.size(), size_t(36));
  CHECK_EQ(id[14], '4');  // UUID version 4.

  const fs::path dir = root / "my_game";
  const json game = test::ReadJson(dir / "game.json");
  CHECK_EQ(game.value("schema", ""), std::string(kLibrarySchema));
  CHECK_EQ(game.value("game_id", ""), std::string("my_game"));
  CHECK_EQ(game.value("title", ""), std::string("My Game"));
  CHECK_EQ(game.value("game_version", ""), std::string("1.2"));
  CHECK_EQ(game.value("sdk_version", ""), std::string(kSdkVersion));
  CHECK_EQ(game.value("install_id", ""), id);
  CHECK(game["platforms"] == json::array({"ps3"}));
  CHECK(game["sets"] == json::array({"game"}));
  CHECK(game["last_played"].get<uint64_t>() > 0);
  CHECK(game["icon"].is_null());

  // The set, resolved: capped points, tiers, secrets, checklist, credits.
  const fs::path set_file = dir / "sets" / "game.achievements.json";
  const json doc = test::ReadJson(set_file);
  AchievementSet set;
  std::string error;
  CHECK(detail::ParseNativeSet(doc, set_file, set, &error));
  CHECK_EQ(set.title, std::string("My Game"));
  CHECK_EQ(set.achievements.size(), size_t(4));
  uint32_t total = 0;
  for (const auto& a : set.achievements) total += a.points;
  CHECK_EQ(total, 100u);
  CHECK(set.achievements[0].tier == Tier::kGold);
  CHECK(set.achievements[2].secret);
  CHECK_EQ(set.achievements[2].locked_description, std::string("Keep playing"));
  CHECK_EQ(set.achievements[1].checklist.size(), size_t(3));
  CHECK_EQ(set.achievements[1].trigger.type, std::string("checklist"));
  CHECK_EQ(set.credits.at(0).name, std::string("The recomp-achievements authors"));
  CHECK(doc["_library"]["enabled"] == true);
  // The PNG icon was copied into icons/ and the ref points there.
  const std::string icon = set.achievements[0].icon;
  CHECK(icon.starts_with("../icons/") && icon.ends_with(".png"));
  CHECK_EQ(test::ReadFile(dir / "sets" / icon), std::string(kTinyPng.begin(), kTinyPng.end()));

  const json progress = test::ReadJson(dir / "progress" / "default.json");
  CHECK_EQ(progress.value("profile", ""), std::string("default"));
  CHECK(progress["achievements"]["game:a"]["unlocked"] == true);
  CHECK(progress["achievements"]["game:b"]["current"] == 1);
  CHECK(progress["achievements"]["game:b"]["total"] == 3);
  CHECK(!progress["achievements"].contains("game:c"));
  CHECK(progress["totals"]["count"] == 4);
  CHECK(progress["totals"]["unlocked"] == 1);
  CHECK(progress["sets"]["game"]["enabled"] == true);

  const auto events = EventLines(dir / "events.log");
  CHECK_EQ(events.size(), size_t(2));
  CHECK_EQ(events[0].value("kind", ""), std::string("unlock"));
  CHECK_EQ(events[0].value("key", ""), std::string("game:a"));
  CHECK_EQ(events[0].value("install_id", ""), id);
  CHECK_EQ(events[0].value("game", ""), std::string("my_game"));
  CHECK_EQ(events[0].value("game_version", ""), std::string("1.2"));
  CHECK_EQ(events[0].value("v", 0), 1);
  CHECK(events[0]["t"].get<uint64_t>() > 1600000000000ull);
  CHECK_EQ(events[1].value("kind", ""), std::string("progress"));
  CHECK(events[1]["value"] == 1);
  CHECK_EQ(events[1].value("item", ""), std::string("Kickflip"));
}

TEST(library_install_id_is_made_once) {
  const fs::path root = test::TempDir("library_install") / "library";
  System sys;
  std::string first;
  {
    Library lib(sys, Options(root));
    first = lib.install_id();
  }
  LibraryOptions other = Options(root);
  other.game_id = "other_game";
  Library lib(sys, other);
  CHECK(!first.empty());
  CHECK_EQ(lib.install_id(), first);
}

TEST(library_disabled_without_game_id) {
  System sys;
  LibraryOptions o = Options(test::TempDir("library_disabled"));
  o.game_id.clear();
  Library lib(sys, o);
  CHECK(!lib.enabled());
  CHECK(lib.dir().empty());
  lib.Flush();
  LibraryOptions off = Options(test::TempDir("library_disabled2"));
  off.enabled = false;
  Library lib2(sys, off);
  CHECK(!lib2.enabled());
  CHECK(!fs::exists(off.root / "install.json"));
}

TEST(library_atomic_writes) {
  const fs::path tmp = test::TempDir("library_atomic");
  const std::string first = "{\"a\": 1}";
  CHECK(detail::WriteFileAtomic(tmp / "x" / "file.json",
                                std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(first.data()), first.size())));
  CHECK(detail::WriteJsonAtomic(tmp / "x" / "file.json", json{{"a", 2}}));
  CHECK(test::ReadJson(tmp / "x" / "file.json")["a"] == 2);
  CHECK_EQ(CountFiles(tmp, ".tmp-"), size_t(0));
  // A parent that is a file: false, no throw, no leftovers.
  test::WriteFile(tmp / "blocker", "not a folder");
  CHECK(!detail::WriteJsonAtomic(tmp / "blocker" / "file.json", json{{"a", 3}}));
  CHECK_EQ(CountFiles(tmp, ".tmp-"), size_t(0));

  // A whole library run leaves only finished files.
  System sys;
  sys.AddSet(MakeSet(tmp / "game_files"));
  {
    Library lib(sys, Options(tmp / "library"));
    for (const char* key : {"game:a", "game:c"}) sys.Unlock(key);
    lib.Flush();
  }
  CHECK_EQ(CountFiles(tmp / "library", ".tmp-"), size_t(0));
}

TEST(library_snapshots_are_coalesced) {
  const fs::path tmp = test::TempDir("library_coalesce");
  System sys;
  sys.AddSet(MakeSet(tmp / "game_files"));
  LibraryOptions o = Options(tmp / "library");
  o.write_interval = std::chrono::milliseconds(2000);
  o.poll_interval = std::chrono::milliseconds(20);
  Library lib(sys, o);
  lib.Flush();
  const uint64_t before = lib.stats().snapshot_writes;
  for (int i = 0; i < 200; ++i) sys.ReportEvent("land", "spot " + std::to_string(i));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  // 200 progress steps, at most one more snapshot inside the interval.
  CHECK(lib.stats().snapshot_writes - before <= 1);
  lib.Flush();
  const json progress = test::ReadJson(lib.dir() / "progress" / "default.json");
  CHECK(progress["achievements"]["game:d"]["current"] == 200);
  CHECK_EQ(progress["achievements"]["game:d"]["completed"].size(), size_t(200));
  CHECK_EQ(EventLines(lib.dir() / "events.log").size(), size_t(200));
  CHECK(lib.stats().snapshot_writes - before <= 2);
}

TEST(library_events_append_across_instances) {
  const fs::path tmp = test::TempDir("library_append");
  const fs::path saves = tmp / "saves";
  const fs::path log = tmp / "library" / "my_game" / "events.log";
  std::string first_text;
  {
    System sys;
    sys.SetStorage(saves);
    sys.AddSet(MakeSet(tmp / "game_files"));
    Library lib(sys, Options(tmp / "library"));
    sys.Unlock("game:a");
  }
  first_text = test::ReadFile(log);
  CHECK(!first_text.empty());
  {
    System sys;
    sys.SetStorage(saves);
    sys.AddSet(MakeSet(tmp / "game_files"));
    Library lib(sys, Options(tmp / "library"));
    CHECK(sys.IsUnlocked("game:a"));
    sys.Unlock("game:c");
  }
  const std::string second_text = test::ReadFile(log);
  CHECK(second_text.starts_with(first_text));
  const auto events = EventLines(log);
  CHECK_EQ(events.size(), size_t(2));
  CHECK_EQ(events[1].value("key", ""), std::string("game:c"));
  CHECK_EQ(events[0].value("install_id", ""), events[1].value("install_id", ""));
  // A restart with nothing new adds nothing.
  {
    System sys;
    sys.SetStorage(saves);
    sys.AddSet(MakeSet(tmp / "game_files"));
    Library lib(sys, Options(tmp / "library"));
  }
  CHECK_EQ(test::ReadFile(log), second_text);
}

TEST(library_reset_and_toggle_events) {
  const fs::path tmp = test::TempDir("library_reset");
  System sys;
  sys.AddSet(MakeSet(tmp / "game_files"));
  Library lib(sys, Options(tmp / "library"));
  sys.Unlock("game:a");
  lib.Flush();
  sys.Reset({"game:a"});
  lib.Flush();
  // Unlocked and reset between two looks: the reset still shows.
  sys.Unlock("game:c");
  sys.ResetSet("game");
  lib.Flush();
  sys.SetSetEnabled("game", false);
  lib.Flush();

  const auto events = EventLines(lib.dir() / "events.log");
  std::vector<std::string> kinds;
  for (const auto& e : events) kinds.push_back(e.value("kind", "") + " " + e.value("key", ""));
  const std::vector<std::string> expected = {"unlock game:a", "reset game:a", "unlock game:c", "reset game:c",
                                             "set_enabled game"};
  CHECK_EQ(kinds.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) CHECK_EQ(kinds[i], expected[i]);
  CHECK(events.back()["value"] == false);
  CHECK_EQ(events.back().value("profile", "x"), std::string());
  const json set = test::ReadJson(lib.dir() / "sets" / "game.achievements.json");
  CHECK(set["_library"]["enabled"] == false);
  const json progress = test::ReadJson(lib.dir() / "progress" / "default.json");
  CHECK(progress["sets"]["game"]["enabled"] == false);
  CHECK(progress["totals"]["count"] == 0);
}

TEST(library_profile_hashing) {
  const fs::path tmp = test::TempDir("library_hash");
  std::string profile = "Player One";
  System sys;
  sys.SetStorage(tmp / "saves", [&] { return profile; });
  sys.AddSet(MakeSet(tmp / "game_files"));
  LibraryOptions o = Options(tmp / "library");
  o.hash_profiles = true;
  std::string hashed;
  {
    Library lib(sys, o);
    hashed = lib.ProfileId("Player One");
    CHECK(hashed.starts_with("h-"));
    CHECK_EQ(hashed.size(), size_t(18));
    CHECK_EQ(lib.ProfileId("Player One"), hashed);
    CHECK(lib.ProfileId("Player Two") != hashed);
    sys.Unlock("game:a");
  }
  const fs::path dir = tmp / "library" / "my_game";
  CHECK(fs::exists(dir / "progress" / (hashed + ".json")));
  for (const auto& e : fs::recursive_directory_iterator(dir)) {
    if (!e.is_regular_file()) continue;
    CHECK(e.path().filename().string().find("Player") == std::string::npos);
    CHECK(test::ReadFile(e.path()).find("Player One") == std::string::npos);
  }
  CHECK_EQ(EventLines(dir / "events.log").at(0).value("profile", ""), hashed);

  // Stored as given by default; file names stay safe and distinct.
  profile = "Player/One";
  LibraryOptions plain = Options(tmp / "library2");
  {
    Library lib(sys, plain);
    CHECK_EQ(lib.ProfileId("Player/One"), std::string("Player/One"));
    sys.Unlock("game:c");
  }
  bool found = false;
  for (const auto& e : fs::directory_iterator(tmp / "library2" / "my_game" / "progress")) {
    const json doc = test::ReadJson(e.path());
    if (doc.value("profile", "") == "Player/One") {
      found = true;
      CHECK(e.path().filename().string().starts_with("Player_One-"));
    }
  }
  CHECK(found);

  // The player can force hashing for every game in install.json.
  json install = test::ReadJson(tmp / "library2" / "install.json");
  install["hash_profiles"] = true;
  test::WriteFile(tmp / "library2" / "install.json", install.dump());
  Library forced(sys, plain);
  CHECK(forced.ProfileId("Player/One").starts_with("h-"));
}

TEST(library_icons_from_a_source_once) {
  const fs::path tmp = test::TempDir("library_icons");
  AchievementSet set = MakeSet(tmp / "game_files");
  set.platform = Platform::kXbox360;
  set.achievements[1].icon = "gen:red";
  set.achievements[1].locked_icon = "gen:gray";
  set.achievements[2].icon = "gen:missing";
  System sys;
  sys.AddSet(set);
  int calls = 0;
  LibraryOptions o = Options(tmp / "library");
  o.icon_source = [&](std::string_view ref, std::string_view, const fs::path&, LibraryIcon& out) {
    if (ref == "gen:missing") return false;
    ++calls;
    out.width = 2;
    out.height = 2;
    const uint8_t v = ref == "gen:red" ? 200 : 90;
    out.rgba.assign(16, v);
    if (ref == "xdbf:32768") {
      out.rgba.clear();
      out.png = kTinyPng;
    }
    return true;
  };
  {
    Library lib(sys, o);
    lib.Flush();
    // a.png, red and gray; the title image has a.png's bytes, so it shares its file.
    CHECK_EQ(lib.stats().icon_writes, uint64_t(3));
  }
  const fs::path dir = tmp / "library" / "my_game";
  const json doc = test::ReadJson(dir / "sets" / "game.achievements.json");
  const std::string red = doc["achievements"][1].value("icon", "");
  const std::string gray = doc["achievements"][1].value("locked_icon", "");
  CHECK(red.starts_with("../icons/") && gray.starts_with("../icons/") && red != gray);
  CHECK(!doc["achievements"][2].contains("icon"));  // Not available: no icon.
  const auto png = test::ReadFile(dir / "sets" / red);
  CHECK(detail::IsPng(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(png.data()), png.size())));
  // The Xbox title image became the game icon.
  const json game = test::ReadJson(dir / "game.json");
  CHECK(game.value("icon", "").starts_with("icons/"));
  CHECK_EQ(test::ReadFile(dir / game.value("icon", "")), std::string(kTinyPng.begin(), kTinyPng.end()));

  // Next session: the index answers; only the missing one is asked again.
  calls = 0;
  Library again(sys, o);
  again.Flush();
  CHECK_EQ(again.stats().icon_writes, uint64_t(0));
  CHECK_EQ(calls, 0);
}

TEST(library_game_icon_from_file) {
  const fs::path tmp = test::TempDir("library_game_icon");
  test::WriteFile(tmp / "assets" / "icon.png", std::string(kTinyPng.begin(), kTinyPng.end()));
  System sys;
  sys.AddSet(MakeSet(tmp / "game_files"));
  LibraryOptions o = Options(tmp / "library");
  o.icon = "icon.png";
  o.icon_base_dir = tmp / "assets";
  { Library lib(sys, o); }
  const json game = test::ReadJson(tmp / "library" / "my_game" / "game.json");
  CHECK_EQ(test::ReadFile(tmp / "library" / "my_game" / game.value("icon", "")),
           std::string(kTinyPng.begin(), kTinyPng.end()));
}

TEST(library_unwritable_root_does_not_crash) {
  const fs::path tmp = test::TempDir("library_unwritable");
  test::WriteFile(tmp / "blocker", "a file where the folder should be");
  System sys;
  sys.AddSet(MakeSet(tmp / "game_files"));
  Library lib(sys, Options(tmp / "blocker" / "library"));
  CHECK(lib.enabled());
  CHECK(!lib.install_id().empty());
  sys.Unlock("game:a");
  sys.ReportEvent("trick", "Kickflip");
  sys.SetSetEnabled("game", false);
  lib.Flush();
  CHECK(lib.stats().failures > 0);
  CHECK_EQ(lib.stats().snapshot_writes, uint64_t(0));
  CHECK(sys.IsUnlocked("game:a"));
}

TEST(library_events_rotate) {
  const fs::path tmp = test::TempDir("library_rotate");
  System sys;
  sys.AddSet(MakeSet(tmp / "game_files"));
  LibraryOptions o = Options(tmp / "library");
  o.rotate_bytes = 600;
  Library lib(sys, o);
  for (int i = 0; i < 12; ++i) {
    sys.ReportEvent("land", "spot " + std::to_string(i));
    lib.Flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK(CountFiles(lib.dir(), "events-") >= 1);
  LibraryGame game;
  game.dir = lib.dir();
  CHECK_EQ(ReadEvents(game).size(), size_t(12));
}

TEST(library_waits_for_sets) {
  // A Library made before the game loads its sets picks them up later.
  const fs::path tmp = test::TempDir("library_late_sets");
  System sys;
  LibraryOptions o = Options(tmp / "library");
  o.title.clear();
  Library lib(sys, o);
  lib.Flush();
  CHECK(!fs::exists(lib.dir() / "progress" / "default.json"));
  sys.AddSet(MakeSet(tmp / "game_files"));
  lib.Flush();
  CHECK(fs::exists(lib.dir() / "sets" / "game.achievements.json"));
  CHECK(fs::exists(lib.dir() / "progress" / "default.json"));
  CHECK_EQ(test::ReadJson(lib.dir() / "game.json").value("title", ""), std::string("My Game"));
}

TEST(library_png_encoder) {
  const std::vector<uint8_t> rgba(4 * 300 * 300, 7);
  const auto png = EncodePng(300, 300, rgba);  // More than one stored block.
  CHECK(detail::IsPng(png));
  CHECK(png.size() > rgba.size());
  CHECK(EncodePng(0, 1, rgba).empty());
  CHECK(EncodePng(2, 2, std::vector<uint8_t>(3)).empty());
  CHECK_EQ(detail::Sha256Hex(std::string_view("abc")),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(detail::Sha256Hex(std::string_view("")),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  CHECK_EQ(detail::Sha256Hex(std::string(56, 'a')).size(), size_t(64));
}
