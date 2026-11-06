// Reading the library back: written entries, partial and old ones, events,
// totals, and the sync stub.

#include "recomp/achievements/library.h"
#include "recomp/achievements/library_reader.h"
#include "recomp/achievements/sync.h"
#include "recomp/achievements/system.h"
#include "test.h"

using namespace recomp::achievements;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

AchievementSet SmallSet(const fs::path& folder) {
  const auto png = EncodePng(1, 1, std::vector<uint8_t>{1, 2, 3, 255});
  test::WriteFile(folder / "a.png", std::string(png.begin(), png.end()));
  AchievementSet set;
  set.id = "small";
  set.title = "Small Game";
  set.platform = Platform::kXbox360;
  set.base_dir = folder;
  Achievement a;
  a.local_id = "a";
  a.name = "First";
  a.points = 10;
  a.icon = "a.png";
  Achievement b;
  b.local_id = "b";
  b.name = "Flips";
  b.points = 20;
  b.trigger.type = "checklist";
  b.trigger.event = "trick";
  b.checklist = {"Kickflip", "Heelflip"};
  Achievement c;
  c.local_id = "c";
  c.name = "Hidden";
  c.points = 30;
  c.secret = true;
  set.achievements = {a, b, c};
  return set;
}

LibraryOptions Options(const fs::path& root, std::string id = "small_game") {
  LibraryOptions o;
  o.game_id = std::move(id);
  o.version = "2.0";
  o.root = root;
  o.wine_host = false;
  return o;
}

LibraryEvent Event(uint64_t t, std::string install, std::string key, std::string kind) {
  LibraryEvent e;
  e.t = t;
  e.install_id = std::move(install);
  e.key = std::move(key);
  e.kind = std::move(kind);
  return e;
}

}  // namespace

TEST(reader_reads_a_written_entry) {
  const fs::path tmp = test::TempDir("reader_written");
  const fs::path root = tmp / "library";
  std::string profile = "one";
  System sys;
  sys.SetStorage(tmp / "saves", [&] { return profile; });
  sys.AddSet(SmallSet(tmp / "files"));
  {
    Library lib(sys, Options(root));
    sys.Unlock("small:a");
    sys.ReportEvent("trick", "Kickflip");
    lib.Flush();
    profile = "two";
    sys.Unlock("small:c");
    sys.Unlock("small:a");
  }

  const auto games = ListLibrary(root);
  CHECK_EQ(games.size(), size_t(1));
  const LibraryGame& g = games[0];
  CHECK_EQ(g.id, std::string("small_game"));
  CHECK_EQ(g.schema, 1);
  CHECK_EQ(g.title, std::string("Small Game"));
  CHECK_EQ(g.game_version, std::string("2.0"));
  CHECK(g.platforms.size() == 1 && g.platforms[0] == Platform::kXbox360);
  CHECK(g.sets.empty());  // ListLibrary leaves the sets out.
  CHECK_EQ(g.profiles.size(), size_t(2));
  CHECK(g.warnings.empty());
  const LibraryProfile* best = g.BestProfile();
  CHECK(best && best->id == "two");
  CHECK_EQ(best->totals.unlocked_points, 40u);
  CHECK_EQ(best->totals.points, 60u);

  const auto full = LoadLibraryGame(root, "small_game");
  CHECK(full.has_value());
  CHECK_EQ(full->sets.size(), size_t(1));
  const AchievementSet& set = full->sets[0].set;
  CHECK(full->sets[0].enabled);
  CHECK_EQ(set.achievements[0].key, std::string("small:a"));
  CHECK(set.achievements[2].secret);
  CHECK(fs::is_regular_file(set.achievements[0].icon));
  CHECK(fs::path(set.achievements[0].icon).is_absolute());
  const LibraryProfile& one = full->profiles[0];
  CHECK_EQ(one.id, std::string("one"));
  CHECK(one.progress.at("small:a").progress.unlocked);
  CHECK(one.progress.at("small:a").progress.unlock_time > 0);
  CHECK_EQ(one.progress.at("small:b").current, 1u);
  CHECK_EQ(one.progress.at("small:b").total, 2u);
  CHECK_EQ(one.set_totals.at("small").unlocked, 1u);

  const auto events = ReadEvents(*full);
  CHECK_EQ(events.size(), size_t(4));
  CHECK_EQ(events[0].kind, std::string("unlock"));
  CHECK_EQ(events[1].item, std::string("Kickflip"));
  CHECK_EQ(events[2].profile, std::string("two"));
  CHECK(ReadEvents(*full, events[3].t).size() >= 1);
  CHECK(ReadEvents(*full, events[3].t + 1).empty());

  const auto recent = RecentUnlocks(games, 2);
  CHECK_EQ(recent.size(), size_t(2));
  CHECK(recent[0].time >= recent[1].time);
  const LibraryTotals sum = SumLibrary(games);
  CHECK_EQ(sum.unlocked, 2u);
  CHECK_EQ(sum.count, 3u);
}

TEST(reader_tolerates_partial_and_old_entries) {
  const fs::path root = test::TempDir("reader_partial");
  // Progress only, an old file without totals.
  test::WriteFile(root / "progress_only" / "progress" / "p.json",
                  R"({"achievements": {"x:1": {"unlocked": true, "unlock_time": 5}, "x:2": {"count": 2}}})");
  // Garbage game.json and an invalid set next to a good one.
  test::WriteFile(root / "broken" / "game.json", "{not json");
  test::WriteFile(root / "broken" / "sets" / "bad.achievements.json", "[1, 2");
  test::WriteFile(root / "broken" / "sets" / "good.achievements.json",
                  R"({"schema": "recomp-achievements/1", "set": {"id": "good"},
                      "achievements": [{"id": "a", "name": "A", "icon": "../../../../outside.png"}]})");
  // A newer schema with fields this reader doesn't know.
  test::WriteFile(root / "future" / "game.json",
                  R"({"schema": "recomp-achievements-library/2", "title": "Future", "new_field": [1],
                      "platforms": ["xbox360", "dreamcast"], "icon": "../elsewhere.png", "last_played": 9})");
  // Not entries: an empty folder, a hidden one and a file.
  fs::create_directories(root / "empty");
  test::WriteFile(root / ".hidden" / "game.json", "{}");
  test::WriteFile(root / "install.json", R"({"install_id": "x"})");
  // Progress that isn't an object.
  test::WriteFile(root / "progress_only" / "progress" / "zz.json", "[]");

  const auto games = ListLibrary(root);
  CHECK_EQ(games.size(), size_t(3));
  CHECK_EQ(games[0].id, std::string("broken"));
  CHECK_EQ(games[1].id, std::string("future"));
  CHECK_EQ(games[2].id, std::string("progress_only"));

  const LibraryGame& only = games[2];
  CHECK_EQ(only.schema, 0);
  CHECK_EQ(only.title, std::string("progress_only"));
  CHECK_EQ(only.profiles.size(), size_t(1));
  CHECK_EQ(only.profiles[0].id, std::string("p"));
  CHECK_EQ(only.profiles[0].totals.unlocked, 1u);
  CHECK(!only.warnings.empty());

  const LibraryGame& future = games[1];
  CHECK_EQ(future.schema, 2);
  CHECK_EQ(future.title, std::string("Future"));
  CHECK_EQ(future.platforms.size(), size_t(1));
  CHECK(future.icon.empty());  // Outside the entry: ignored.
  CHECK(!future.warnings.empty());

  const auto broken = LoadLibraryGame(root, "broken");
  CHECK(broken.has_value());
  CHECK_EQ(broken->sets.size(), size_t(1));
  CHECK_EQ(broken->sets[0].set.id, std::string("good"));
  CHECK(broken->sets[0].set.achievements[0].icon.empty());
  CHECK(broken->warnings.size() >= 2);
  CHECK(ReadEvents(*broken).empty());

  CHECK(!LoadLibraryGame(root, "missing").has_value());
  CHECK(!LoadLibraryGame(root, "../reader_partial").has_value());
  CHECK(ListLibrary(root / "nowhere").empty());
}

TEST(reader_lists_several_roots) {
  const fs::path tmp = test::TempDir("reader_roots");
  test::WriteFile(tmp / "a" / "game1" / "game.json", R"({"title": "Old", "last_played": 10})");
  test::WriteFile(tmp / "b" / "game1" / "game.json", R"({"title": "New", "last_played": 20})");
  test::WriteFile(tmp / "b" / "game2" / "game.json", R"({"title": "Two"})");
  const auto games = ListLibrary(std::vector<fs::path>{tmp / "a", tmp / "b", tmp / "a", tmp / "none"});
  CHECK_EQ(games.size(), size_t(2));
  CHECK_EQ(games[0].title, std::string("New"));
  CHECK(games[0].dir == tmp / "b" / "game1");
  CHECK_EQ(games[1].title, std::string("Two"));
}

TEST(reader_events_skip_bad_lines_and_dedupe) {
  const fs::path dir = test::TempDir("reader_events") / "g";
  const json line1 = EventToJson(Event(100, "i1", "s:a", "unlock"));
  const json line2 = EventToJson(Event(50, "i1", "s:b", "progress"));
  test::WriteFile(dir / "events.log", line1.dump() + "\n{broken\n\n" + R"({"v":1,"key":"no time"})" + "\n" +
                                          line1.dump() + "\n");
  test::WriteFile(dir / "events-1.log", line2.dump() + "\n");
  LibraryGame g;
  g.dir = dir;
  const auto events = ReadEvents(g);
  CHECK_EQ(events.size(), size_t(2));
  CHECK_EQ(events[0].t, uint64_t(50));
  CHECK_EQ(events[1].key, std::string("s:a"));
  CHECK_EQ(ReadEvents(g, 60).size(), size_t(1));

  // A sync merge is a union deduplicated by (install_id, t, key, kind).
  const std::vector<LibraryEvent> other = {Event(100, "i1", "s:a", "unlock"), Event(100, "i2", "s:a", "unlock"),
                                           Event(70, "i2", "s:a", "reset")};
  const auto merged = MergeEvents(events, other);
  CHECK_EQ(merged.size(), size_t(4));
  CHECK_EQ(merged[1].kind, std::string("reset"));

  LibraryEvent round;
  CHECK(EventFromJson(EventToJson(merged[0]), round));
  CHECK(round.SameAs(merged[0]));
  CHECK(!EventFromJson(json::array(), round));
}

TEST(sync_provider_stub) {
  NullSyncProvider sync;
  SyncProvider& provider = sync;
  CHECK(!provider.signed_in());
  CHECK(provider.account().empty());
  const std::vector<LibraryEvent> events = {Event(1, "i", "k", "unlock")};
  CHECK(!provider.Push(events));
  CHECK(provider.Pull(0).empty());
}
