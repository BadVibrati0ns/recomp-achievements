// The registry, trigger engine, saves and listeners.

#include <fstream>
#include <atomic>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "recomp/achievements/system.h"
#include "test.h"

using namespace recomp::achievements;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

Achievement Make(std::string id, std::string type = "manual") {
  Achievement a;
  a.local_id = std::move(id);
  a.name = a.local_id;
  a.trigger.type = std::move(type);
  return a;
}

Achievement Stat(std::string id, std::string stat, double threshold) {
  Achievement a = Make(std::move(id), "stat");
  a.trigger.stat = std::move(stat);
  a.trigger.threshold = threshold;
  return a;
}

Achievement Event(std::string id, std::string event, std::string target = {}) {
  Achievement a = Make(std::move(id), "event");
  a.trigger.event = std::move(event);
  a.trigger.target = std::move(target);
  return a;
}

Achievement Checklist(std::string id, std::string event, std::vector<std::string> items) {
  Achievement a = Make(std::move(id), "checklist");
  a.trigger.event = std::move(event);
  a.checklist = std::move(items);
  return a;
}

Achievement Count(std::string id, std::string event, double threshold, bool distinct = true) {
  Achievement a = Make(std::move(id), "count");
  a.trigger.event = std::move(event);
  a.trigger.threshold = threshold;
  if (!distinct) a.trigger.params["distinct"] = false;
  return a;
}

AchievementSet Set(std::string id, std::vector<Achievement> list) {
  AchievementSet set;
  set.id = std::move(id);
  set.achievements = std::move(list);
  return set;
}

// Records every listener call in order, as "unlock:<key>" / "progress:<key>:<item>".
struct Recorder {
  std::mutex mutex;
  std::vector<std::string> log;
  std::vector<UnlockEvent> unlocks;
  std::vector<ProgressEvent> progress;

  void Attach(System& sys) {
    sys.AddUnlockListener([this](const UnlockEvent& e) {
      std::lock_guard lock(mutex);
      log.push_back("unlock:" + e.view.achievement->key);
      unlocks.push_back(e);
    });
    sys.AddProgressListener([this](const ProgressEvent& e) {
      std::lock_guard lock(mutex);
      log.push_back("progress:" + e.view.achievement->key + ":" + e.item);
      progress.push_back(e);
    });
  }
};

using Log = std::vector<std::string>;

}  // namespace

TEST(normalize_name) {
  CHECK_EQ(NormalizeName("360 Flip!"), "360flip");
  CHECK_EQ(NormalizeName("  Miracle-Whip "), "miraclewhip");
  CHECK_EQ(NormalizeName("___"), "");
  CHECK_EQ(NormalizeName("Ünïcode"), "Ünïcode");  // Non-ASCII bytes are kept as is.
  CHECK_EQ(NormalizeName("トロフィー A"), "トロフィーa");
}

TEST(add_set_fills_keys_categories_and_order) {
  System sys;
  AchievementSet set = Set("g", {Make("b"), Make(""), Make("a")});
  set.category = "Main";
  set.achievements[1].platform_id = 9;
  set.achievements[0].display_order = 5;
  set.achievements[2].category = "Side";
  sys.AddSet(set);
  CHECK(sys.Find("g:b") != nullptr);
  CHECK(sys.Find("g:9") != nullptr);
  CHECK_EQ(sys.Find("g:b")->set_id, "g");
  CHECK_EQ(sys.Find("g:b")->category, "Main");
  CHECK_EQ(sys.Find("g:a")->category, "Side");
  CHECK_EQ(sys.FindSet("g")->title, "g");

  const auto views = sys.Snapshot();
  CHECK_EQ(views.size(), 3u);
  CHECK_EQ(views[0].achievement->key, "g:9");
  CHECK_EQ(views[1].achievement->key, "g:a");
  CHECK_EQ(views[2].achievement->key, "g:b");
  CHECK(views[0].set == sys.FindSet("g"));

  // Duplicate ids: the later one replaces the earlier in place.
  Achievement dup = Make("b");
  dup.name = "Second";
  sys.AddSet(Set("d", {Make("b"), Make("c"), dup}));
  CHECK_EQ(sys.FindSet("d")->achievements.size(), 2u);
  CHECK_EQ(sys.Find("d:b")->name, "Second");
  CHECK_EQ(sys.FindSet("d")->achievements[0].local_id, "b");

  // A caller-supplied key without a local id.
  Achievement keyed;
  keyed.key = "k:thing";
  sys.AddSet(Set("k", {keyed}));
  CHECK(sys.Find("k:thing") != nullptr);
}

TEST(add_set_applies_points_cap) {
  System sys;
  AchievementSet set = Set("p", {Make("a"), Make("b")});
  set.points_cap = 100;
  set.achievements[0].points = 150;
  set.achievements[1].points = 50;
  sys.AddSet(set);
  CHECK_EQ(sys.Find("p:a")->points, 75u);
  CHECK_EQ(sys.Find("p:b")->points, 25u);
  const auto totals = sys.GetTotals("p");
  CHECK_EQ(totals.points, 100u);
  CHECK_EQ(totals.count, 2u);
}

TEST(manual_unlock_and_lookup) {
  System sys;
  Achievement a = Make("first");
  a.platform_id = 15;
  a.legacy_keys = {"15"};
  a.points = 10;
  sys.AddSet(Set("g", {a, Make("second")}));
  Recorder rec;
  rec.Attach(sys);

  CHECK(!sys.IsUnlocked("g:first"));
  CHECK(sys.Find("15") == sys.Find("g:first"));  // Legacy keys resolve too.
  CHECK(sys.FindByPlatformId("g", 15) == sys.Find("g:first"));
  CHECK(sys.FindByPlatformId("g", 99) == nullptr);
  CHECK(sys.FindByPlatformId("nope", 15) == nullptr);

  const uint64_t before = static_cast<uint64_t>(std::time(nullptr));
  CHECK(sys.Unlock("g", 15));
  CHECK(!sys.Unlock("g:first"));
  CHECK(!sys.Unlock("g:missing"));
  CHECK(!sys.Unlock("g", 0));
  CHECK(sys.IsUnlocked("g:first"));
  CHECK(sys.IsUnlocked("g", 15));
  CHECK(sys.GetProgress("g:first")->unlock_time >= before);
  CHECK(!sys.GetProgress("g:missing").has_value());
  CHECK_EQ(rec.log, (Log{"unlock:g:first"}));
  CHECK(!rec.unlocks[0].replay);
  CHECK(rec.unlocks[0].view.progress.unlocked);
  CHECK_EQ(rec.unlocks[0].view.achievement->points, 10u);

  const auto totals = sys.GetTotals();
  CHECK_EQ(totals.count, 2u);
  CHECK_EQ(totals.unlocked, 1u);
  CHECK_EQ(totals.unlocked_points, 10u);
}

TEST(stat_trigger) {
  System sys;
  sys.AddSet(Set("g", {Stat("speed", "top_speed", 60), Stat("small", "air_time", 1.5)}));
  Recorder rec;
  rec.Attach(sys);

  sys.ReportStat("top_speed", 30);
  sys.ReportStat("top_speed", 20);  // Lower values don't replace the best.
  CHECK_EQ(sys.GetProgress("g:speed")->best_value, 30.0);
  CHECK(!sys.IsUnlocked("g:speed"));
  sys.ReportStat("Top Speed", 61);  // Names are normalised.
  CHECK(sys.IsUnlocked("g:speed"));
  CHECK_EQ(sys.GetProgress("g:speed")->best_value, 61.0);
  sys.ReportStat("top_speed", 99);  // Unlocked: best value frozen.
  CHECK_EQ(sys.GetProgress("g:speed")->best_value, 61.0);

  sys.ReportStat("air_time", 1.5);  // Equal to the threshold unlocks.
  CHECK(sys.IsUnlocked("g:small"));
  sys.ReportStat("unrelated", 1000);
  CHECK_EQ(rec.log, (Log{"unlock:g:speed", "unlock:g:small"}));
}

TEST(stat_aliases_apply_both_ways) {
  System sys;
  sys.AddSet(Set("g", {Stat("chain", "tricks_chained", 10), Stat("combo", "combo", 5),
                       Stat("speed", "speed", 60)}));
  sys.AddStatAlias("combo", "tricks_chained");
  sys.AddStatAlias("top_speed", "speed");
  sys.AddStatAlias("velocity", "top_speed");  // Chains through top_speed.

  sys.ReportStat("combo", 10);
  CHECK(sys.IsUnlocked("g:chain"));
  CHECK(sys.IsUnlocked("g:combo"));

  sys.Reset();
  sys.ReportStat("tricks_chained", 6);
  CHECK(sys.IsUnlocked("g:combo"));
  CHECK(!sys.IsUnlocked("g:chain"));
  CHECK_EQ(sys.GetProgress("g:chain")->best_value, 6.0);

  sys.ReportStat("velocity", 70);
  CHECK(sys.IsUnlocked("g:speed"));
}

TEST(event_trigger) {
  System sys;
  sys.AddSet(Set("g", {Event("whip", "trick", "Miracle Whip"), Event("any", "cheat", "any"),
                       Event("empty", "spot"), Make("manual")}));
  Recorder rec;
  rec.Attach(sys);

  sys.ReportEvent("trick", "Kickflip");
  CHECK(!sys.IsUnlocked("g:whip"));
  sys.ReportEvent("TRICK", "miracle whip!");
  CHECK(sys.IsUnlocked("g:whip"));
  sys.ReportEvent("cheat", "mcfly");
  CHECK(sys.IsUnlocked("g:any"));
  sys.ReportEvent("spot");
  CHECK(sys.IsUnlocked("g:empty"));
  sys.ReportEvent("manual");
  CHECK(!sys.IsUnlocked("g:manual"));
  CHECK_EQ(rec.log, (Log{"unlock:g:whip", "unlock:g:any", "unlock:g:empty"}));
}

TEST(checklist_trigger) {
  System sys;
  sys.AddSet(Set("g", {Checklist("flips", "trick", {"Kickflip", "Heelflip", "360 Flip"})}));
  Recorder rec;
  rec.Attach(sys);

  sys.ReportEvent("trick", "kickflip");
  sys.ReportEvent("trick", "Kickflip");  // Already done: nothing.
  sys.ReportEvent("trick", "Varial");
  sys.ReportEvent("other", "Heelflip");
  CHECK_EQ(sys.GetProgress("g:flips")->completed, std::vector<std::string>{"Kickflip"});

  auto view = sys.Snapshot()[0];
  CHECK_EQ(view.progress_current, 1u);
  CHECK_EQ(view.progress_total, 3u);

  sys.ReportEvent("trick", "Heel Flip");
  sys.ReportEvent("trick", "360 flip");
  CHECK(sys.IsUnlocked("g:flips"));
  // A progress event per new item while incomplete; the last one only unlocks.
  CHECK_EQ(rec.log, (Log{"progress:g:flips:Kickflip", "progress:g:flips:Heelflip", "unlock:g:flips"}));
  CHECK_EQ(rec.progress[0].view.progress_current, 1u);
  CHECK_EQ(rec.progress[1].view.progress_current, 2u);
  CHECK_EQ(rec.progress[1].view.progress_total, 3u);
  CHECK_EQ(rec.unlocks[0].view.progress_current, 3u);
  view = sys.Snapshot()[0];
  CHECK_EQ(view.progress_current, 3u);
}

TEST(checklist_one_report_can_complete_several_items) {
  System sys;
  sys.AddSet(Set("g", {Checklist("dup", "trick", {"Manual", "manual", "Nose Manual"})}));
  Recorder rec;
  rec.Attach(sys);
  sys.ReportEvent("trick", "MANUAL");
  CHECK_EQ(rec.log, (Log{"progress:g:dup:Manual"}));
  CHECK_EQ(sys.Snapshot()[0].progress_current, 2u);  // Both spellings count.
  sys.ReportEvent("trick", "nose manual");
  CHECK(sys.IsUnlocked("g:dup"));
}

TEST(count_trigger_distinct_and_occurrences) {
  System sys;
  Achievement filtered = Count("plaza", "spot", 2);
  filtered.trigger.target = "plaza";
  sys.AddSet(Set("g", {Count("spots", "spot", 3), Count("bails", "bail", 3, /*distinct=*/false), filtered}));
  Recorder rec;
  rec.Attach(sys);

  sys.ReportEvent("spot", "a");
  sys.ReportEvent("spot", "A");  // Same target, normalised.
  sys.ReportEvent("spot");       // No target: nothing distinct to count.
  CHECK_EQ(sys.GetProgress("g:spots")->completed, std::vector<std::string>{"a"});
  sys.ReportEvent("spot", "b");
  auto views = sys.Snapshot();
  CHECK_EQ(views[0].progress_current, 2u);
  CHECK_EQ(views[0].progress_total, 3u);
  sys.ReportEvent("spot", "c");
  CHECK(sys.IsUnlocked("g:spots"));

  sys.ReportEvent("bail");
  sys.ReportEvent("bail", "same");
  CHECK_EQ(sys.GetProgress("g:bails")->count, 2u);
  CHECK(!sys.IsUnlocked("g:bails"));
  CHECK_EQ(sys.Snapshot()[1].progress_current, 2u);
  sys.ReportEvent("bail", "same");
  CHECK(sys.IsUnlocked("g:bails"));

  // The target filters which reports count; distinct then means once.
  sys.ReportEvent("spot", "Plaza");
  sys.ReportEvent("spot", "plaza");
  CHECK_EQ(sys.GetProgress("g:plaza")->completed.size(), 1u);

  CHECK_EQ(rec.log, (Log{"progress:g:spots:a", "progress:g:spots:b", "unlock:g:spots",
                         "progress:g:bails:", "progress:g:bails:same", "unlock:g:bails",
                         "progress:g:plaza:Plaza"}));
}

TEST(custom_name_matcher) {
  System sys;
  sys.AddSet(Set("g", {Event("tre", "trick", "360 Flip"), Checklist("list", "trick", {"Indy"})}));
  sys.SetNameMatcher([](std::string_view reported, std::string_view expected) {
    const std::string r = NormalizeName(reported), e = NormalizeName(expected);
    if (r == "boom") throw std::runtime_error("matcher failure");
    return r == e || (r == "treflip" && e == "360flip") || (r == "stiffy" && e == "indy");
  });
  sys.ReportEvent("trick", "boom");  // A throwing matcher is a non-match.
  CHECK(!sys.IsUnlocked("g:tre"));
  sys.ReportEvent("trick", "Tre Flip");
  CHECK(sys.IsUnlocked("g:tre"));
  sys.ReportEvent("trick", "Stiffy");
  CHECK(sys.IsUnlocked("g:list"));
  CHECK_EQ(sys.GetProgress("g:list")->completed, std::vector<std::string>{"Indy"});
}

TEST(complete_item_and_game_defined_triggers) {
  System sys;
  Achievement kots = Make("kots", "own_spot_count");
  kots.trigger.threshold = 3;
  Achievement group = Make("group", "trick_group");  // Checklist filled by the game.
  group.checklist = {"Kickflip", "Heelflip"};
  Achievement gap = Make("gap", "gap");
  gap.trigger.threshold = 10;
  sys.AddSet(Set("g", {kots, group, gap, Count("bails", "bail", 2, false), Make("plain")}));
  Recorder rec;
  rec.Attach(sys);

  CHECK(sys.CompleteItem("g:kots", "spot1"));
  CHECK(!sys.CompleteItem("g:kots", "SPOT 1"));
  CHECK(sys.CompleteItem("g:kots", "spot2"));
  CHECK_EQ(sys.Snapshot()[0].progress_current, 2u);
  CHECK_EQ(sys.Snapshot()[0].progress_total, 3u);
  CHECK(sys.CompleteItem("g:kots", "spot3"));
  CHECK(sys.IsUnlocked("g:kots"));
  CHECK(!sys.CompleteItem("g:kots", "spot4"));

  CHECK(!sys.CompleteItem("g:group", "Varial"));  // Not on the list.
  CHECK(sys.CompleteItem("g:group", "kickflip"));
  CHECK(sys.CompleteItem("g:group", "heelflip"));
  CHECK(sys.IsUnlocked("g:group"));

  CHECK(sys.CompleteItem("g:bails", "x"));
  CHECK(sys.CompleteItem("g:bails", "x"));  // Occurrences always count.
  CHECK(sys.IsUnlocked("g:bails"));

  CHECK(sys.CompleteItem("g:plain", "x"));  // Recorded, but nothing to complete.
  CHECK(!sys.IsUnlocked("g:plain"));
  CHECK(!sys.CompleteItem("g:missing", "x"));

  // Built-in triggers ignore game-defined types.
  sys.ReportEvent("gap", "x");
  sys.ReportStat("gap", 100);
  CHECK(!sys.IsUnlocked("g:gap"));
  sys.RecordValue("g:gap", 4.7);
  sys.RecordValue("g:gap", 2);
  CHECK_EQ(sys.GetProgress("g:gap")->best_value, 4.7);
  const auto view = sys.Snapshot()[2];
  CHECK_EQ(view.progress_current, 4u);
  CHECK_EQ(view.progress_total, 10u);

  CHECK_EQ(rec.log, (Log{"progress:g:kots:spot1", "progress:g:kots:spot2", "unlock:g:kots",
                         "progress:g:group:Kickflip", "unlock:g:group", "progress:g:bails:x",
                         "unlock:g:bails", "progress:g:plain:x"}));
}

TEST(progress_bars_in_snapshot) {
  System sys;
  Achievement hidden = Stat("hidden", "speed", 30);
  hidden.trigger.params["progress_bar"] = false;
  sys.AddSet(Set("g", {Stat("big", "score", 2500), Stat("tiny", "air", 1.5), Make("manual"),
                       Event("event", "e"), hidden}));
  sys.ReportStat("score", 1234.9);
  sys.ReportStat("air", 1.2);
  sys.ReportStat("speed", 10);
  auto views = sys.Snapshot();
  CHECK_EQ(views[0].progress_current, 1234u);
  CHECK_EQ(views[0].progress_total, 2500u);
  CHECK_EQ(views[1].progress_total, 0u);  // Threshold under 2: no bar.
  CHECK_EQ(views[2].progress_total, 0u);
  CHECK_EQ(views[3].progress_total, 0u);
  CHECK_EQ(views[4].progress_total, 0u);
  CHECK_EQ(views[4].progress.best_value, 10.0);
  sys.Unlock("g:big");
  CHECK_EQ(sys.Snapshot()[0].progress_current, 2500u);
}

TEST(for_each_pending) {
  System sys;
  Achievement loc1 = Make("loc1", "reach_location");
  loc1.trigger.params["radius"] = 8;
  sys.AddSet(Set("g", {loc1, Make("loc2", "reach_location"), Make("other", "gap")}));
  CHECK(sys.HasPending("reach_location"));
  CHECK(!sys.HasPending("nothing"));
  std::vector<std::string> seen;
  sys.ForEachPending("reach_location", [&](const Achievement& a, const Progress& p) {
    seen.push_back(a.key);
    CHECK(!p.unlocked);
    if (a.trigger.params.contains("radius")) sys.Unlock(a.key);  // Allowed: runs on a copy.
  });
  CHECK_EQ(seen, (std::vector<std::string>{"g:loc1", "g:loc2"}));
  seen.clear();
  sys.ForEachPending("reach_location", [&](const Achievement& a, const Progress&) { seen.push_back(a.key); });
  CHECK_EQ(seen, (std::vector<std::string>{"g:loc2"}));
}

TEST(test_popup_replays_without_saving) {
  const auto dir = ::test::TempDir("test_popup");
  System sys;
  sys.SetStorage(dir);
  sys.AddSet(Set("g", {Make("a"), Make("b")}));
  Recorder rec;
  rec.Attach(sys);

  sys.TestPopup();
  CHECK_EQ(rec.log, (Log{"unlock:g:a"}));
  CHECK(rec.unlocks[0].replay);
  CHECK(!sys.IsUnlocked("g:a"));
  CHECK(!fs::exists(dir / "default_achievements.json"));

  sys.Unlock("g:a");
  sys.TestPopup();  // First locked one.
  sys.TestPopup("g:a");
  sys.Unlock("g:b");
  sys.TestPopup();  // All unlocked: the first.
  sys.TestPopup("missing");
  CHECK_EQ(rec.log, (Log{"unlock:g:a", "unlock:g:a", "unlock:g:b", "unlock:g:a", "unlock:g:b", "unlock:g:a"}));
  CHECK(rec.unlocks[4].view.progress.unlocked);

  System empty;
  empty.TestPopup();  // Nothing loaded: no crash, no event.
}

TEST(listener_order_and_exceptions) {
  System sys;
  sys.AddSet(Set("g", {Checklist("list", "trick", {"a", "b"}), Event("ev", "trick", "b"), Make("m")}));
  Log log;
  sys.AddUnlockListener([&](const UnlockEvent& e) { log.push_back("u1:" + e.view.achievement->local_id); });
  const int thrower = sys.AddUnlockListener([](const UnlockEvent&) { throw std::runtime_error("listener"); });
  sys.AddProgressListener([&](const ProgressEvent& e) { log.push_back("p1:" + e.item); });
  const int second = sys.AddUnlockListener([&](const UnlockEvent& e) { log.push_back("u2:" + e.view.achievement->local_id); });
  sys.AddProgressListener([](const ProgressEvent&) { throw 42; });

  sys.ReportEvent("trick", "a");
  sys.ReportEvent("trick", "b");  // Completes the checklist and fires the event trigger.
  CHECK_EQ(log, (Log{"p1:a", "u1:list", "u2:list", "u1:ev", "u2:ev"}));

  sys.RemoveListener(second);
  sys.RemoveListener(thrower);
  sys.RemoveListener(12345);
  log.clear();
  sys.Unlock("g:m");
  CHECK_EQ(log, (Log{"u1:m"}));
}

TEST(listeners_run_outside_the_lock) {
  System sys;
  sys.AddSet(Set("g", {Make("a"), Make("b")}));
  bool chained = false, saw_unlocked = false;
  size_t snapshot_size = 0;
  sys.AddUnlockListener([&](const UnlockEvent& e) {
    // Calling back in would deadlock if the lock were held.
    if (e.view.achievement->local_id != "a") return;
    saw_unlocked = sys.IsUnlocked(e.view.achievement->key);
    snapshot_size = sys.Snapshot().size();
    chained = sys.Unlock("g:b");
  });
  CHECK(sys.Unlock("g:a"));
  CHECK(saw_unlocked);
  CHECK_EQ(snapshot_size, 2u);
  CHECK(chained);
  CHECK(sys.IsUnlocked("g:b"));
}

TEST(event_views_survive_set_replacement) {
  System sys;
  sys.AddSet(Set("g", {Make("a")}));
  std::string name;
  std::string set_id;
  sys.AddUnlockListener([&](const UnlockEvent& e) {
    sys.AddSet(Set("g", {Make("a")}));  // Replace the set while handling its event.
    name = e.view.achievement->name;    // Still readable for this event.
    set_id = e.view.set->id;
  });
  sys.Unlock("g:a");
  CHECK_EQ(name, "a");
  CHECK_EQ(set_id, "g");
  CHECK(sys.IsUnlocked("g:a"));  // Progress carried over to the new set.
}

TEST(saves_per_profile) {
  const auto dir = ::test::TempDir("saves_per_profile");
  std::string profile = "alice";
  System sys;
  sys.AddSet(Set("g", {Make("a"), Make("b"), Stat("s", "speed", 100)}));
  sys.SetStorage(dir, [&] { return profile; });
  CHECK_EQ(sys.active_profile(), "alice");

  CHECK(sys.Unlock("g:a"));
  CHECK(fs::exists(dir / "alice_achievements.json"));

  // A profile change is noticed on the next unlock: bob starts fresh.
  profile = "bob";
  CHECK_EQ(sys.active_profile(), "alice");
  CHECK(sys.Unlock("g:b"));
  CHECK_EQ(sys.active_profile(), "bob");
  CHECK(!sys.IsUnlocked("g:a"));
  CHECK(sys.IsUnlocked("g:b"));
  CHECK(sys.Unlock("g:a"));  // Locked for bob, so it unlocks again.

  const json alice = ::test::ReadJson(dir / "alice_achievements.json");
  CHECK_EQ(alice["profile"], "alice");
  CHECK(alice["achievements"].contains("g:a"));
  CHECK(!alice["achievements"].contains("g:b"));

  profile = "alice";
  sys.ReloadProgress();
  CHECK(sys.IsUnlocked("g:a"));
  CHECK(!sys.IsUnlocked("g:b"));

  // Unsaved best values follow their profile on a switch.
  sys.ReportStat("speed", 40);
  profile = "bob";
  sys.ReloadProgress();
  CHECK_EQ(sys.GetProgress("g:s")->best_value, 0.0);
  CHECK_EQ(::test::ReadJson(dir / "alice_achievements.json")["achievements"]["g:s"]["best_value"], 40.0);

  // Empty or odd profile ids still map to safe file names.
  profile = "";
  sys.ReloadProgress();
  CHECK_EQ(sys.active_profile(), "default");
  profile = "a/b:c";
  sys.Unlock("g:a");
  CHECK(fs::exists(dir / "a_b_c_achievements.json"));
  CHECK_EQ(::test::ReadJson(dir / "a_b_c_achievements.json")["profile"], "a/b:c");
}

TEST(profile_switch_detected_by_event_triggers) {
  const auto dir = ::test::TempDir("profile_switch_events");
  std::string profile = "one";
  int calls = 0;
  System sys;
  sys.AddSet(Set("g", {Event("e", "go"), Checklist("c", "item", {"x", "y"})}));
  sys.SetStorage(dir, [&] {
    ++calls;
    return profile;
  });
  sys.ReportEvent("item", "x");
  profile = "two";
  const int before = calls;
  sys.ReportEvent("nothing");  // Irrelevant reports don't ask for the profile.
  CHECK_EQ(calls, before);
  sys.ReportEvent("go");
  CHECK_EQ(sys.active_profile(), "two");
  CHECK(sys.IsUnlocked("g:e"));
  CHECK(sys.GetProgress("g:c")->completed.empty());
  const json one = ::test::ReadJson(dir / "one_achievements.json");
  CHECK_EQ(one["achievements"]["g:c"]["completed"], json::array({"x"}));
  CHECK(!one["achievements"].contains("g:e"));
}

TEST(best_values_saved_on_destruction) {
  const auto dir = ::test::TempDir("best_on_destruction");
  {
    System sys;
    sys.AddSet(Set("g", {Stat("s", "speed", 100)}));
    sys.SetStorage(dir);
    sys.ReportStat("speed", 55);
    CHECK(!fs::exists(dir / "default_achievements.json"));  // Not saved per report.
  }
  CHECK_EQ(::test::ReadJson(dir / "default_achievements.json")["achievements"]["g:s"]["best_value"], 55.0);
  System again;
  again.SetStorage(dir);
  again.AddSet(Set("g", {Stat("s", "speed", 100)}));
  CHECK_EQ(again.GetProgress("g:s")->best_value, 55.0);
}

TEST(in_memory_progress_without_storage) {
  std::string profile = "p1";
  System sys;
  sys.SetStorage({}, [&] { return profile; });
  sys.AddSet(Set("g", {Make("a"), Make("b")}));
  sys.Unlock("g:a");
  profile = "p2";
  sys.Unlock("g:b");
  CHECK(!sys.IsUnlocked("g:a"));
  profile = "p1";
  sys.ReloadProgress();
  CHECK(sys.IsUnlocked("g:a"));
  CHECK(!sys.IsUnlocked("g:b"));
}

TEST(sets_can_be_removed_and_added_back) {
  System sys;
  sys.AddSet(Set("base", {Make("a")}));
  sys.AddSet(Set("mod", {Make("m"), Stat("s", "x", 10)}));
  sys.Unlock("mod:m");
  sys.ReportStat("x", 3);
  sys.RemoveSet("mod");
  CHECK(sys.FindSet("mod") == nullptr);
  CHECK(sys.Find("mod:m") == nullptr);
  CHECK_EQ(sys.sets().size(), 1u);
  sys.RemoveSet("nope");
  sys.AddSet(Set("mod", {Make("m"), Stat("s", "x", 10)}));
  CHECK(sys.IsUnlocked("mod:m"));
  CHECK_EQ(sys.GetProgress("mod:s")->best_value, 3.0);

  // Replacing keeps load order.
  sys.AddSet(Set("base", {Make("a"), Make("new")}));
  CHECK_EQ(sys.sets()[0]->id, "base");
  CHECK_EQ(sys.sets()[0]->achievements.size(), 2u);

  sys.ClearSets();
  CHECK(sys.sets().empty());
  CHECK(sys.Snapshot().empty());
  sys.AddSet(Set("mod", {Make("m")}));
  CHECK(sys.IsUnlocked("mod:m"));
}

TEST(removed_sets_keep_their_save_entries) {
  const auto dir = ::test::TempDir("removed_sets");
  System sys;
  sys.SetStorage(dir);
  sys.AddSet(Set("base", {Make("a")}));
  sys.AddSet(Set("mod", {Make("m")}));
  sys.Unlock("mod:m");
  sys.RemoveSet("mod");
  sys.Unlock("base:a");
  const json saved = ::test::ReadJson(dir / "default_achievements.json");
  CHECK(saved["achievements"].contains("mod:m"));
  CHECK(saved["achievements"].contains("base:a"));
}

TEST(reset) {
  const auto dir = ::test::TempDir("reset");
  ::test::WriteFile(dir / "default_achievements.json",
                    R"({"profile": "default", "achievements": {"gone:x": {"unlocked": true, "unlock_time": 1}}})");
  System sys;
  sys.SetStorage(dir);
  sys.AddSet(Set("g", {Make("a"), Make("b"), Checklist("c", "t", {"1", "2"})}));
  sys.Unlock("g:a");
  sys.Unlock("g:b");
  sys.ReportEvent("t", "1");

  sys.Reset({"g:a", "gone:x"});
  CHECK(!sys.IsUnlocked("g:a"));
  CHECK(sys.IsUnlocked("g:b"));
  json saved = ::test::ReadJson(dir / "default_achievements.json");
  CHECK(!saved["achievements"].contains("g:a"));
  CHECK(!saved["achievements"].contains("gone:x"));
  CHECK(saved["achievements"].contains("g:b"));

  sys.Reset(std::vector<std::string>{});
  CHECK(sys.IsUnlocked("g:b"));

  sys.Reset();
  CHECK(!sys.IsUnlocked("g:b"));
  CHECK(sys.GetProgress("g:c")->completed.empty());
  saved = ::test::ReadJson(dir / "default_achievements.json");
  CHECK_EQ(saved["achievements"], json::object());
  // Unlocks work again after a reset.
  CHECK(sys.Unlock("g:a"));
}

TEST(reset_drops_stale_legacy_entries) {
  const auto dir = ::test::TempDir("reset_legacy");
  ::test::WriteFile(dir / "default_achievements.json",
                    R"({"achievements": {"7": {"unlocked": true, "unlock_time": 3}}})");
  Achievement a = Make("seven");
  a.legacy_keys = {"7"};
  System sys;
  sys.AddSet(Set("g", {a}));
  sys.SetStorage(dir);
  CHECK(sys.IsUnlocked("g:seven"));
  sys.Reset({"g:seven"});
  sys.ReloadProgress();
  CHECK(!sys.IsUnlocked("g:seven"));  // The old "7" entry can't bring it back.
}

TEST(corrupt_save_is_backed_up) {
  const auto dir = ::test::TempDir("corrupt_save");
  ::test::WriteFile(dir / "default_achievements.json", "{ not json");
  System sys;
  sys.AddSet(Set("g", {Make("a")}));
  sys.SetStorage(dir);
  CHECK(!sys.IsUnlocked("g:a"));
  CHECK(sys.Unlock("g:a"));
  CHECK_EQ(::test::ReadFile(dir / "default_achievements.json.corrupt"), "{ not json");
  CHECK(::test::ReadJson(dir / "default_achievements.json")["achievements"].contains("g:a"));
}

TEST(load_set_file_reports_errors) {
  const auto dir = ::test::TempDir("load_set_file");
  ::test::WriteFile(dir / "bad.achievements.json", R"({"achievements": [{"id": "a"}]})");
  System sys;
  std::string error;
  CHECK(!sys.LoadSetFile(dir / "bad.achievements.json", &error));
  CHECK_CONTAINS(error, "bad.achievements.json: achievements[0]: needs a \"name\"");
  CHECK(sys.sets().empty());
  CHECK(sys.LoadSetFile(::test::DataDir() / "native" / "example.achievements.json", &error));
  CHECK(sys.Find("my_game:combo") != nullptr);
  CHECK(sys.Find("15") == sys.Find("my_game:first_board"));
}

TEST(thread_safety_smoke) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 40;
  std::vector<Achievement> list;
  for (int i = 0; i < kThreads * kPerThread; ++i) {
    const std::string id = std::to_string(i);
    switch (i % 4) {
      case 0: list.push_back(Make("m" + id)); break;
      case 1: list.push_back(Stat("s" + id, "stat" + id, 5)); break;
      case 2: list.push_back(Event("e" + id, "event", id)); break;
      default: list.push_back(Checklist("c" + id, "item" + id, {"x", "y", "z"})); break;
    }
  }
  const auto dir = ::test::TempDir("thread_safety");
  System sys;
  sys.AddSet(Set("g", list));
  sys.SetStorage(dir);
  std::atomic<int> unlocks{0}, progress{0};
  sys.AddUnlockListener([&](const UnlockEvent& e) {
    if (!e.replay) ++unlocks;
  });
  sys.AddProgressListener([&](const ProgressEvent&) { ++progress; });

  std::atomic<bool> stop{false};
  std::thread reader([&] {
    while (!stop) {
      for (const auto& view : sys.Snapshot()) (void)view.progress_current;
      (void)sys.GetTotals();
      sys.TestPopup();
    }
  });
  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&, t] {
      // Every worker hits every achievement, so each is raced for.
      for (int round = 0; round < 2; ++round) {
        for (int k = 0; k < kThreads * kPerThread; ++k) {
          const int i = (k + t * kPerThread) % (kThreads * kPerThread);
          const std::string id = std::to_string(i);
          switch (i % 4) {
            case 0: sys.Unlock("g:m" + id); break;
            case 1: sys.ReportStat("stat" + id, 2 + t); break;
            case 2: sys.ReportEvent("event", id); break;
            default:
              for (const char* item : {"x", "y", "z"}) sys.ReportEvent("item" + id, item);
              break;
          }
        }
      }
    });
  }
  for (auto& w : workers) w.join();
  stop = true;
  reader.join();

  CHECK_EQ(unlocks.load(), kThreads * kPerThread);  // Exactly once each.
  CHECK_EQ(progress.load(), (kThreads * kPerThread / 4) * 2);
  CHECK_EQ(sys.GetTotals().unlocked, static_cast<uint32_t>(kThreads * kPerThread));
  System reloaded;
  reloaded.AddSet(Set("g", list));
  reloaded.SetStorage(dir);
  CHECK_EQ(reloaded.GetTotals().unlocked, static_cast<uint32_t>(kThreads * kPerThread));
}

TEST(platform_id_zero_is_valid_for_ps3_sets) {
  System sys;
  AchievementSet trophies = Set("np", {});
  trophies.platform = Platform::kPlayStation3;
  Achievement plat;  // No local id: taken from the trophy id, even 0.
  plat.name = "Platinum";
  plat.tier = Tier::kPlatinum;
  Achievement bronze = Make("bronze");
  bronze.platform_id = 1;
  trophies.achievements = {plat, bronze};
  sys.AddSet(trophies);
  CHECK(sys.Find("np:0") != nullptr);
  CHECK(sys.FindByPlatformId("np", 0) == sys.Find("np:0"));
  CHECK(sys.FindByPlatformId("np", 1) == sys.Find("np:bronze"));
  CHECK(!sys.IsUnlocked("np", 0));
  CHECK(sys.Unlock("np", 0));
  CHECK(sys.IsUnlocked("np", 0));
  CHECK(sys.IsUnlocked("np:0"));
  CHECK(!sys.Unlock("np", 0));

  // Custom sets: 0 still means "no platform id".
  sys.AddSet(Set("custom", {Make("a"), Make("b")}));
  CHECK(sys.FindByPlatformId("custom", 0) == nullptr);
  CHECK(!sys.Unlock("custom", 0));
  CHECK(!sys.IsUnlocked("custom", 0));
  CHECK(!sys.IsUnlocked("custom:a"));
}

TEST(generation_tracks_definition_and_progress_changes) {
  const auto dir = ::test::TempDir("generation");
  std::string profile = "one";
  System sys;
  uint64_t last = sys.generation();
  const auto bumped = [&] {
    const uint64_t now = sys.generation();
    const bool changed = now > last;
    last = now;
    return changed;
  };

  sys.AddSet(Set("g", {Make("a"), Stat("s", "x", 10)}));
  CHECK(bumped());
  sys.AddSet(Set("g", {Make("a"), Stat("s", "x", 10)}));  // Replaced.
  CHECK(bumped());
  sys.AddSet(Set("h", {Make("b")}));
  CHECK(bumped());

  // Ordinary progress doesn't count: that's what listeners are for.
  sys.Unlock("g:a");
  sys.ReportStat("x", 3);
  sys.RecordValue("g:s", 4);
  CHECK(!bumped());
  (void)sys.Snapshot();
  CHECK(!bumped());

  sys.SetStorage(dir, [&] { return profile; });
  CHECK(bumped());
  profile = "two";
  sys.Unlock("h:b");  // Notices the profile switch.
  CHECK(bumped());
  sys.ReloadProgress();
  CHECK(bumped());

  sys.Reset({"g:a"});
  CHECK(bumped());
  sys.Reset();
  CHECK(bumped());
  sys.RemoveSet("nope");
  CHECK(!bumped());
  sys.RemoveSet("h");
  CHECK(bumped());
  sys.ClearSets();
  CHECK(bumped());
}

TEST(record_value_with_total_overrides_the_bar) {
  const auto dir = ::test::TempDir("record_value_total");
  System sys;
  sys.SetStorage(dir);
  Achievement measured = Make("measured", "memory");
  measured.trigger.params["condition"] = "0xH0000=1";
  measured.legacy_keys = {"old"};
  sys.AddSet(Set("g", {measured, Stat("s", "x", 50), Make("m")}));

  CHECK_EQ(sys.Snapshot()[0].progress_total, 0u);
  sys.RecordValue("g:measured", 3.6, 12);
  auto views = sys.Snapshot();
  CHECK_EQ(views[0].progress_current, 3u);
  CHECK_EQ(views[0].progress_total, 12u);
  CHECK_EQ(sys.GetProgress("g:measured")->best_value, 3.6);

  // The total changes even when the value isn't a new best.
  sys.RecordValue("old", 1, 20);  // Legacy keys resolve to the same achievement.
  views = sys.Snapshot();
  CHECK_EQ(views[0].progress_current, 3u);
  CHECK_EQ(views[0].progress_total, 20u);

  // Overrides a trigger's own bar; total <= 0 restores it.
  sys.RecordValue("g:s", 7, 9);
  CHECK_EQ(sys.Snapshot()[1].progress_total, 9u);
  sys.RecordValue("g:s", 8, 0);
  CHECK_EQ(sys.Snapshot()[1].progress_total, 50u);
  CHECK_EQ(sys.Snapshot()[1].progress_current, 8u);

  // A manual achievement gets a bar only through an override.
  sys.RecordValue("g:m", 1, 4);
  CHECK_EQ(sys.Snapshot()[2].progress_total, 4u);
  sys.Unlock("g:m");
  CHECK_EQ(sys.Snapshot()[2].progress_current, 4u);
  sys.RecordValue("g:missing", 1, 4);  // Unknown key: ignored.

  // Not saved: a fresh System has the best value but not the total.
  sys.RecordValue("g:measured", 5, 12);
  sys.Unlock("g:s");  // Forces a save.
  const json saved = ::test::ReadJson(dir / "default_achievements.json");
  CHECK_EQ(saved["achievements"]["g:measured"]["best_value"], 5.0);
  CHECK_EQ(saved["achievements"]["g:measured"].size(), 3u);  // unlocked, unlock_time, best_value.
  System again;
  again.SetStorage(dir);
  again.AddSet(Set("g", {measured, Stat("s", "x", 50), Make("m")}));
  CHECK_EQ(again.Snapshot()[0].progress_total, 0u);

  // Reset drops the override with the progress.
  sys.Reset({"g:measured"});
  CHECK_EQ(sys.Snapshot()[0].progress_total, 0u);
}

TEST(progress_popups_option) {
  System sys;
  Achievement quiet = Count("quiet", "combo", 5, /*distinct=*/false);
  quiet.trigger.params["progress_popups"] = false;
  Achievement every2 = Count("every2", "combo", 5, /*distinct=*/false);
  every2.trigger.params["progress_popups"] = 2;
  Achievement list = Checklist("list", "trick", {"a", "b", "c"});
  list.trigger.params["progress_popups"] = false;
  sys.AddSet(Set("g", {quiet, every2, list}));
  Recorder rec;
  rec.Attach(sys);

  for (int i = 0; i < 5; ++i) sys.ReportEvent("combo");
  sys.ReportEvent("trick", "a");
  sys.ReportEvent("trick", "b");
  sys.ReportEvent("trick", "c");
  // No steps for "quiet" or "list", steps 2 and 4 for "every2"; every unlock still pops.
  CHECK_EQ(rec.log, (Log{"progress:g:every2:", "progress:g:every2:", "unlock:g:quiet", "unlock:g:every2",
                         "unlock:g:list"}));
  CHECK_EQ(sys.GetProgress("g:quiet")->count, 5u);
}

TEST(load_directory_reports_errors) {
  const auto dir = ::test::TempDir("load_dir_errors");
  std::ofstream(dir / "good.achievements.json") << R"({"achievements": [{"id": "a", "name": "A"}]})";
  std::ofstream(dir / "typo.achievements.json")
      << R"({"achievements": [{"id": "b", "name": "B", "progress_bar": false}]})";
  std::ofstream(dir / "broken.achievements.json") << "{ not json";
  System sys;
  std::vector<std::string> errors;
  CHECK_EQ(sys.LoadDirectory(dir, &errors), 1u);
  CHECK_EQ(errors.size(), 2u);
  bool typo = false, broken = false;
  for (const auto& e : errors) {
    typo |= e.find("typo.achievements.json") != std::string::npos && e.find("progress_bar") != std::string::npos;
    broken |= e.find("broken.achievements.json") != std::string::npos;
  }
  CHECK(typo);
  CHECK(broken);
  CHECK_EQ(System().LoadDirectory(dir), 1u);  // Without a vector the bad files are just skipped.
}

TEST(progress_popups_from_json) {
  System sys;
  std::string error;
  CHECK(sys.LoadSetJson(R"({"achievements": [{"id": "c", "name": "Fifty Combos",
      "trigger": {"type": "count", "event": "combo", "threshold": 50, "distinct": false, "progress_popups": 10}}]})",
                        "my_game.achievements.json", &error));
  const Achievement* a = sys.Find("my_game:c");
  CHECK(a && a->trigger.params.value("progress_popups", 0) == 10);
}

TEST(load_progress_mirrors_without_saving_or_listeners) {
  System sys;
  Achievement list = Make("list", "checklist");
  list.trigger.event = "trick";
  list.checklist = {"Kickflip", "Heelflip"};
  sys.AddSet(Set("g", {Make("a"), list}));
  Recorder rec;
  rec.Attach(sys);
  const uint64_t gen = sys.generation();

  Progress a;
  a.unlocked = true;
  a.unlock_time = 1700000000;
  Progress l;
  l.completed = {"Kickflip"};
  sys.LoadProgress({{"g:a", a}, {"g:list", l}, {"other:x", a}}, "player2");
  CHECK(sys.generation() > gen);
  CHECK_EQ(sys.active_profile(), std::string("player2"));
  CHECK(sys.IsUnlocked("g:a"));
  CHECK_EQ(sys.GetProgress("g:a")->unlock_time, uint64_t(1700000000));
  CHECK_EQ(sys.Snapshot()[1].progress_current, 1u);
  CHECK(rec.log.empty());

  // Replaces rather than merges.
  sys.LoadProgress({}, "");
  CHECK(!sys.IsUnlocked("g:a"));
  CHECK_EQ(sys.active_profile(), std::string("default"));
}

TEST(set_clock_stamps_unlocks) {
  System sys;
  sys.AddSet(Set("g", {Make("a"), Make("b")}));
  sys.SetClock([] { return uint64_t(1234); });
  CHECK(sys.Unlock("g:a"));
  CHECK_EQ(sys.GetProgress("g:a")->unlock_time, uint64_t(1234));
  sys.SetClock(nullptr);
  CHECK(sys.Unlock("g:b"));
  CHECK(sys.GetProgress("g:b")->unlock_time > uint64_t(1234));
}
