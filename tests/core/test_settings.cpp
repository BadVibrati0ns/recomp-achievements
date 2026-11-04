// Turning sets on and off, the settings file, and splitting sets.

#include <algorithm>

#include "json_format.h"
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
  a.points = 10;
  a.trigger.type = std::move(type);
  return a;
}

AchievementSet TwoSets(System& sys) {
  AchievementSet base;
  base.id = "base";
  base.achievements = {Make("a"), Make("b")};
  Achievement stat = Make("speed", "stat");
  stat.trigger.stat = "speed";
  stat.trigger.threshold = 30;
  Achievement event = Make("trick", "event");
  event.trigger.event = "trick";
  Achievement list = Make("flips", "checklist");
  list.trigger.event = "flip";
  list.checklist = {"Kickflip", "Heelflip"};
  AchievementSet extra;
  extra.id = "extra";
  extra.achievements = {Make("x"), stat, event, list};
  sys.AddSet(base);
  sys.AddSet(extra);
  return extra;
}

std::vector<std::string> Keys(const std::vector<AchievementView>& views) {
  std::vector<std::string> out;
  for (const auto& v : views) out.push_back(v.achievement->key);
  return out;
}

}  // namespace

TEST(set_disabled_cannot_unlock_and_is_hidden) {
  System sys;
  TwoSets(sys);
  sys.Unlock("extra:x");
  CHECK(sys.IsSetEnabled("extra"));
  const uint64_t gen = sys.generation();
  sys.SetSetEnabled("extra", false);
  CHECK(sys.generation() > gen);
  CHECK(!sys.IsSetEnabled("extra"));

  // No unlocks, no trigger progress, no test popups.
  int events = 0;
  sys.AddUnlockListener([&](const UnlockEvent&) { ++events; });
  sys.AddProgressListener([&](const ProgressEvent&) { ++events; });
  sys.ReportStat("speed", 99);
  sys.ReportEvent("trick");
  sys.ReportEvent("flip", "Kickflip");
  CHECK(!sys.CompleteItem("extra:flips", "Heelflip"));
  CHECK(!sys.Unlock("extra:speed"));
  sys.RecordValue("extra:speed", 12);
  sys.TestPopup("extra:x");
  CHECK_EQ(events, 0);
  CHECK(!sys.IsUnlocked("extra:speed"));
  CHECK(!sys.IsUnlocked("extra:trick"));
  CHECK_EQ(sys.GetProgress("extra:speed")->best_value, 0.0);
  CHECK(sys.GetProgress("extra:flips")->completed.empty());
  CHECK(!sys.HasPending("stat"));

  // Left out of Snapshot() and the totals, unless asked.
  CHECK_EQ(Keys(sys.Snapshot()), (std::vector<std::string>{"base:a", "base:b"}));
  CHECK_EQ(sys.Snapshot(true).size(), 6u);
  CHECK_EQ(sys.GetTotals().count, 2u);
  CHECK_EQ(sys.GetTotals().points, 20u);
  CHECK_EQ(sys.GetTotals("extra").count, 4u);
  CHECK_EQ(sys.GetTotals("extra").unlocked, 1u);
  CHECK_EQ(sys.sets().size(), 2u);
  CHECK_EQ(sys.enabled_sets().size(), 1u);
  // Test popups without a key skip it too.
  sys.Unlock("base:a");
  sys.Unlock("base:b");
  events = 0;
  std::string shown;
  sys.AddUnlockListener([&](const UnlockEvent& e) { shown = e.view.achievement->key; });
  sys.TestPopup();
  CHECK_EQ(shown, std::string("base:a"));

  // Progress is kept and back once it's on again.
  CHECK(sys.IsUnlocked("extra:x"));
  sys.SetSetEnabled("extra", true);
  CHECK_EQ(sys.GetTotals().unlocked, 3u);
  CHECK(sys.Unlock("extra:speed"));
  sys.ReportEvent("flip", "Kickflip");
  CHECK_EQ(sys.GetProgress("extra:flips")->completed.size(), 1u);
}

TEST(set_enabled_by_default_and_choices_before_load) {
  System sys;
  AchievementSet off;
  off.id = "online";
  off.enabled_by_default = false;
  off.achievements = {Make("o")};
  sys.AddSet(off);
  CHECK(!sys.IsSetEnabled("online"));
  CHECK(!sys.Unlock("online:o"));
  // A choice for a set that isn't loaded yet applies when it is.
  sys.SetSetEnabled("later", false);
  CHECK(!sys.IsSetEnabled("later"));
  AchievementSet later;
  later.id = "later";
  later.achievements = {Make("l")};
  sys.AddSet(later);
  CHECK(!sys.IsSetEnabled("later"));
  sys.SetSetEnabled("online", true);
  CHECK(sys.Unlock("online:o"));
  // Replacing a set keeps the player's choice.
  sys.AddSet(off);
  CHECK(sys.IsSetEnabled("online"));
  CHECK(sys.IsUnlocked("online:o"));
}

TEST(set_choices_and_settings_persist) {
  const fs::path dir = test::TempDir("set_choices_and_settings_persist");
  {
    System sys;
    sys.SetStorage(dir, [] { return std::string("p1"); });
    TwoSets(sys);
    sys.Unlock("extra:x");
    sys.SetSetEnabled("extra", false);
    sys.SetSetting("popup_sound", {{"enabled", false}, {"volume", 0.4}});
    sys.SetSetting("other", 3);
    sys.SetSetting("other", nullptr);  // Removed.
    CHECK_EQ(sys.settings_path(), dir / "achievement_settings.json");
  }
  const json doc = test::ReadJson(dir / "achievement_settings.json");
  CHECK_EQ(doc["schema"].get<std::string>(), std::string("recomp-achievements-settings/1"));
  CHECK_EQ(doc["sets"]["extra"]["enabled"], json(false));
  CHECK_EQ(doc["settings"]["popup_sound"]["volume"], json(0.4));
  CHECK(!doc["settings"].contains("other"));
  CHECK(!fs::exists(dir / "achievement_settings.json.tmp"));
  // The progress of the set that's off is still in the save.
  CHECK(test::ReadJson(dir / "p1_achievements.json")["achievements"].contains("extra:x"));

  // Not per profile: another profile's System sees the same choice.
  System again;
  again.SetStorage(dir, [] { return std::string("p2"); });
  TwoSets(again);
  CHECK(!again.IsSetEnabled("extra"));
  CHECK_EQ(again.GetSetting("popup_sound")["enabled"], json(false));
  CHECK(again.GetSetting("missing").is_null());
  // Unknown fields and other sets' choices survive a write.
  {
    json edited = doc;
    edited["sets"]["not_loaded"] = {{"enabled", false}};
    edited["future"] = 1;
    test::WriteFile(dir / "achievement_settings.json", edited.dump());
  }
  System third;
  third.SetStorage(dir);
  TwoSets(third);
  third.SetSetEnabled("extra", true);
  const json after = test::ReadJson(dir / "achievement_settings.json");
  CHECK_EQ(after["sets"]["not_loaded"]["enabled"], json(false));
  CHECK_EQ(after["future"], json(1));
  CHECK_EQ(after["sets"]["extra"]["enabled"], json(true));
}

TEST(set_settings_path_and_memory) {
  const fs::path dir = test::TempDir("set_settings_path_and_memory");
  System sys;
  TwoSets(sys);
  // No storage: in memory only.
  sys.SetSetEnabled("extra", false);
  sys.SetSetting("k", "v");
  CHECK(sys.settings_path().empty());
  CHECK(!sys.IsSetEnabled("extra"));
  CHECK_EQ(sys.GetSetting("k"), json("v"));
  // A path of its own; the choices made so far are kept (no file yet).
  const fs::path custom = dir / "nested" / "my_settings.json";
  sys.SetSettingsPath(custom);
  CHECK_EQ(sys.settings_path(), custom);
  CHECK(!sys.IsSetEnabled("extra"));
  sys.SetSetting("k", "w");
  CHECK(fs::exists(custom));
  // SetStorage doesn't move an explicit path.
  sys.SetStorage(dir / "saves");
  CHECK_EQ(sys.settings_path(), custom);
  CHECK(!fs::exists(dir / "saves" / "achievement_settings.json"));
  System other;
  other.SetSettingsPath(custom);
  TwoSets(other);
  CHECK(!other.IsSetEnabled("extra"));
  CHECK_EQ(other.GetSetting("k"), json("w"));
  // An unreadable file is kept aside.
  test::WriteFile(custom, "{ nope");
  System broken;
  broken.SetSettingsPath(custom);
  TwoSets(broken);
  CHECK(broken.IsSetEnabled("extra"));
  CHECK(fs::exists(fs::path(custom) += ".corrupt"));
}

TEST(set_reset_one_set) {
  System sys;
  TwoSets(sys);
  sys.Unlock("base:a");
  sys.Unlock("extra:x");
  sys.ReportEvent("flip", "Kickflip");
  const uint64_t gen = sys.generation();
  sys.ResetSet("extra");
  CHECK(sys.generation() > gen);
  CHECK(sys.IsUnlocked("base:a"));
  CHECK(!sys.IsUnlocked("extra:x"));
  CHECK(sys.GetProgress("extra:flips")->completed.empty());
  // Works on a set that's off, too.
  sys.Unlock("extra:x");
  sys.SetSetEnabled("extra", false);
  sys.ResetSet("extra");
  sys.SetSetEnabled("extra", true);
  CHECK(!sys.IsUnlocked("extra:x"));
}

TEST(split_set_moves_achievements_with_legacy_keys) {
  AchievementSet title;
  title.id = "my_game";
  title.title = "Proving Ground";
  title.platform = Platform::kXbox360;
  title.category = "Story";
  for (uint32_t id : {1u, 29u, 30u, 31u, 48u}) {
    Achievement a;
    a.platform_id = id;
    a.name = "A" + std::to_string(id);
    a.points = 10;
    title.achievements.push_back(a);
  }
  const fs::path dir = test::TempDir("split_set_moves_achievements_with_legacy_keys");
  {
    // An old save, from before the split.
    System sys;
    sys.SetStorage(dir);
    sys.AddSet(title);
    sys.Unlock("my_game:29");
    sys.Unlock("my_game:31");
  }
  const std::vector<uint32_t> ids = {29, 30, 48, 777};
  AchievementSet online = SplitSet(title, ids, "my_game_online", "Online", false);
  CHECK_EQ(title.achievements.size(), 2u);
  CHECK_EQ(online.achievements.size(), 3u);
  CHECK_EQ(online.id, std::string("my_game_online"));
  CHECK_EQ(online.title, std::string("Online"));
  CHECK(online.platform == Platform::kXbox360);
  CHECK_EQ(online.category, std::string("Story"));
  CHECK(!online.enabled_by_default);
  CHECK_EQ(online.achievements[0].local_id, std::string("29"));
  CHECK_EQ(online.achievements[0].legacy_keys, (std::vector<std::string>{"my_game:29"}));
  CHECK_EQ(online.achievements[2].platform_id, 48u);

  System sys;
  sys.SetStorage(dir);
  sys.AddSet(title);
  sys.AddSet(online);
  CHECK(sys.Find("my_game_online:29") != nullptr);
  CHECK(sys.Find("my_game:29") == sys.Find("my_game_online:29"));  // The old key still resolves.
  CHECK(sys.FindByPlatformId("my_game", 29) == nullptr);
  CHECK(!sys.IsSetEnabled("my_game_online"));
  // The old progress counts under the new key, and stays while the set is off.
  CHECK(sys.IsUnlocked("my_game_online:29"));
  CHECK(sys.IsUnlocked("my_game:31"));
  CHECK_EQ(sys.GetTotals().count, 2u);
  CHECK_EQ(sys.GetTotals().unlocked, 1u);
  sys.SetSetEnabled("my_game_online", true);
  CHECK_EQ(sys.GetTotals().unlocked, 2u);
  CHECK(sys.Unlock("my_game_online", 30));
  const json save = test::ReadJson(dir / "default_achievements.json")["achievements"];
  CHECK(save.contains("my_game_online:29"));
  CHECK(save.contains("my_game_online:30"));
  CHECK(!save.contains("my_game:29"));
}

TEST(split_set_pins_position_ids) {
  // Achievements whose id comes from their position keep it after a split.
  AchievementSet s;
  s.id = "pos";
  for (int i = 0; i < 4; ++i) {
    Achievement a;
    a.name = "N" + std::to_string(i);
    a.platform_id = i == 1 ? 5u : 0u;
    s.achievements.push_back(a);
  }
  const std::vector<uint32_t> ids = {5};
  AchievementSet moved = SplitSet(s, ids, "moved", "", true);
  CHECK_EQ(moved.title, std::string("moved"));
  CHECK(moved.enabled_by_default);
  CHECK_EQ(moved.achievements[0].local_id, std::string("5"));
  CHECK_EQ(s.achievements[0].local_id, std::string("0"));
  CHECK_EQ(s.achievements[1].local_id, std::string("2"));
  CHECK_EQ(s.achievements[2].local_id, std::string("3"));
}

TEST(set_enabled_json) {
  AchievementSet set;
  std::string error;
  const json doc = json::parse(R"({"set": {"id": "o", "enabled": false}, "achievements": [{"id": "a", "name": "A"}]})");
  CHECK(detail::ParseNativeSet(doc, "o.achievements.json", set, &error));
  CHECK(!set.enabled_by_default);
  CHECK_EQ(detail::NativeSetToJson(set)["set"]["enabled"], json(false));
  set.enabled_by_default = true;
  CHECK(!detail::NativeSetToJson(set)["set"].contains("enabled"));
  const json bad = json::parse(R"({"set": {"enabled": "no"}, "achievements": []})");
  CHECK(!detail::ParseNativeSet(bad, "b.achievements.json", set, &error));
  CHECK_CONTAINS(error, "set.enabled");
  // Old packs keep their meaning: "enabled": false skips the pack.
  const json pack = json::parse(R"({"name": "P", "enabled": false, "achievements": [{"id": "a", "name": "A"}]})");
  error.clear();
  CHECK(!detail::ParsePackDocument(pack, "P", {}, set, &error));
  CHECK(error.empty());
}
