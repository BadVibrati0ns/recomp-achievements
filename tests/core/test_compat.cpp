// Compatibility with older mod packs and save files.

#include "json_format.h"
#include "recomp/achievements/system.h"
#include "test.h"

using namespace recomp::achievements;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const Achievement& Get(const System& sys, std::string_view key) {
  const Achievement* a = sys.Find(key);
  if (!a) ::test::Fail(__FILE__, __LINE__, "missing achievement " + std::string(key));
  return *a;
}

// The base game's list as a recomp would declare it: new keys, old numeric
// save keys as legacy keys.
AchievementSet BaseGame() {
  AchievementSet set;
  set.id = "my_game";
  set.platform = Platform::kXbox360;
  for (const uint32_t id : {10u, 15u, 112u}) {
    Achievement a;
    a.local_id = "ach" + std::to_string(id);
    a.platform_id = id;
    a.name = "Achievement " + std::to_string(id);
    a.points = 10;
    a.legacy_keys = {std::to_string(id)};
    set.achievements.push_back(a);
  }
  return set;
}

}  // namespace

TEST(pack_sample_mod_loads_like_the_old_loader) {
  System sys;
  CHECK_EQ(sys.LoadDirectory(::test::DataDir() / "packs"), 1u);
  const AchievementSet* set = sys.FindSet("Sample Achievement Mod");
  CHECK(set != nullptr);
  CHECK_EQ(set->title, "Sample Achievement Mod");
  CHECK_EQ(set->points_cap, 1000u);
  CHECK_EQ(set->achievements.size(), 19u);
  CHECK_EQ(set->base_dir, ::test::DataDir() / "packs" / "sample_achievement_mod");
  CHECK_EQ(sys.GetTotals().points, 1000u);

  const auto& own = Get(sys, "Sample Achievement Mod:sample_own_spot");
  CHECK_EQ(own.trigger.type, "own_spot");
  CHECK_EQ(own.trigger.target, "any");
  CHECK_EQ(own.points, 60u);
  CHECK_EQ(own.locked_description, own.description);
  CHECK_EQ(own.category, "Sample Achievement Mod");

  const auto& location = Get(sys, "Sample Achievement Mod:sample_reach_location");
  CHECK_EQ(location.trigger.params["radius"], 8.0);
  CHECK_EQ(location.trigger.params["position"].size(), 3u);

  const auto& gap = Get(sys, "Sample Achievement Mod:sample_gap");
  CHECK_EQ(gap.trigger.params["takeoff"][1], 69.0);
  CHECK_EQ(gap.trigger.params["landing"][2], -181.0);

  CHECK_EQ(Get(sys, "Sample Achievement Mod:sample_speed").trigger.threshold, 30.0);
  CHECK_EQ(Get(sys, "Sample Achievement Mod:sample_trick_name").trigger.target, "360 Flip");

  const auto& flips = Get(sys, "Sample Achievement Mod:sample_flip_master");
  CHECK_EQ(flips.trigger.type, "trick_group");
  CHECK_EQ(flips.trigger.target, "flips");
  CHECK_EQ(flips.trigger.params["group"], "flips");
  CHECK(flips.checklist.empty());
  CHECK(sys.HasPending("trick_group"));
}

TEST(pack_field_mapping) {
  const auto dir = ::test::TempDir("pack_field_mapping");
  ::test::WriteFile(dir / "mine" / "pack.json", R"({
    "name": "My Pack",
    "achievements": [
      {"id": 7, "name": "Seven", "gamerscore": 900, "retail_id": 7, "is_secret": true,
       "unachieved_desc": "???", "description": "Do seven", "trigger": "cheat_code", "code": "mcfly",
       "icon": "icons/seven.png", "category": "Codes"},
      {"name": "Named Only", "trigger": "trick_checklist", "tricks": ["Kickflip", "", "Heelflip"],
       "score": 3.5, "custom_flag": true},
      {"name": "Counted", "trigger": "spots", "count": 4, "spot": "plaza", "target": "ignored"},
      "not an object"
    ]})");
  System sys;
  CHECK_EQ(sys.LoadDirectory(dir), 1u);
  // No "category": the name is the key prefix.
  const AchievementSet* set = sys.FindSet("My Pack");
  CHECK(set != nullptr);
  CHECK_EQ(set->achievements.size(), 3u);

  const auto& seven = Get(sys, "My Pack:7");
  CHECK_EQ(seven.points, 500u);  // Per-achievement cap.
  CHECK_EQ(seven.platform_id, 7u);
  CHECK(seven.secret);
  CHECK_EQ(seven.locked_description, "???");
  CHECK_EQ(seven.trigger.target, "mcfly");
  CHECK_EQ(seven.icon, "icons/seven.png");
  CHECK_EQ(set->base_dir, dir / "mine");
  CHECK_EQ(seven.category, "Codes");
  CHECK(sys.FindByPlatformId("My Pack", 7) == &seven);

  const auto& named = Get(sys, "My Pack:Named Only");
  CHECK_EQ(named.checklist, (std::vector<std::string>{"Kickflip", "Heelflip"}));
  CHECK_EQ(named.trigger.threshold, 3.5);
  CHECK_EQ(named.trigger.params["custom_flag"], true);
  CHECK_EQ(named.category, "My Pack");

  const auto& counted = Get(sys, "My Pack:Counted");
  CHECK_EQ(counted.trigger.target, "plaza");  // "spot" wins over "target".
  CHECK_EQ(counted.trigger.threshold, 4.0);
  CHECK_EQ(counted.trigger.params["target"], "ignored");
}

TEST(pack_ids_and_files) {
  const auto dir = ::test::TempDir("pack_ids_and_files");
  // achievements.json wins over pack.json; a bare array uses the folder name.
  ::test::WriteFile(dir / "folder_pack" / "achievements.json", R"([{"id": "a", "name": "A"}])");
  ::test::WriteFile(dir / "folder_pack" / "pack.json", R"({"category": "Wrong", "achievements": [{"id": "b"}]})");
  ::test::WriteFile(dir / "cat_pack" / "pack.json",
                    R"({"category": "Cat", "name": "Named", "achievements": [{"id": "c", "name": "C"}]})");
  ::test::WriteFile(dir / "off_pack" / "pack.json", R"({"enabled": false, "achievements": [{"id": "d"}]})");
  ::test::WriteFile(dir / "skipped.disabled" / "pack.json", R"({"achievements": [{"id": "e"}]})");
  ::test::WriteFile(dir / "skipped.bak" / "achievements.json", R"([{"id": "e"}])");
  ::test::WriteFile(dir / "no_achievements" / "pack.json", R"({"name": "Model swap", "spots": []})");
  ::test::WriteFile(dir / "broken" / "pack.json", R"({"achievements": [)");
  ::test::WriteFile(dir / "native_pack" / "extra.achievements.json",
                    R"({"set": {"id": "extra"}, "achievements": [{"id": "n", "name": "N"}]})");
  ::test::WriteFile(dir / "top.achievements.json", R"({"achievements": [{"id": "t", "name": "T"}]})");
  ::test::WriteFile(dir / "ignored.json", R"({"achievements": [{"id": "x", "name": "X"}]})");

  System sys;
  CHECK_EQ(sys.LoadDirectory(dir), 4u);
  CHECK(sys.Find("folder_pack:a") != nullptr);
  CHECK(sys.FindSet("Wrong") == nullptr);
  CHECK(sys.Find("Cat:c") != nullptr);
  CHECK_EQ(sys.FindSet("Cat")->title, "Named");
  CHECK(sys.FindSet("off_pack") == nullptr);
  CHECK(sys.Find("extra:n") != nullptr);
  CHECK(sys.Find("top:t") != nullptr);
  CHECK_EQ(sys.sets().size(), 4u);
  // Native files in the top folder load first, then folders in name order.
  CHECK_EQ(sys.sets()[0]->id, "top");
  CHECK_EQ(sys.sets()[1]->id, "Cat");
}

TEST(pack_points_scaled_to_1000) {
  const auto dir = ::test::TempDir("pack_points_scaled");
  ::test::WriteFile(dir / "big" / "pack.json", R"({"category": "Big", "achievements": [
      {"id": "a", "gamerscore": 500}, {"id": "b", "gamerscore": 800}, {"id": "c", "gamerscore": 500},
      {"id": "d", "gamerscore": 0}]})");
  System sys;
  sys.LoadDirectory(dir);
  // 500 + 500 (capped) + 500 = 1500, scaled by 2/3 to 333 each, then padded.
  CHECK_EQ(Get(sys, "Big:a").points, 334u);
  CHECK_EQ(Get(sys, "Big:b").points, 333u);
  CHECK_EQ(Get(sys, "Big:c").points, 333u);
  CHECK_EQ(Get(sys, "Big:d").points, 0u);
  CHECK_EQ(sys.GetTotals("Big").points, 1000u);
  CHECK_EQ(Get(sys, "Big:a").name, "Mod Achievement (a)");
}

TEST(legacy_save_is_read_and_rewritten) {
  const auto dir = ::test::TempDir("legacy_save");
  fs::copy_file(::test::DataDir() / "legacy_save" / "player_achievements.json", dir / "player_achievements.json");

  System sys;
  sys.AddSet(BaseGame());
  sys.LoadDirectory(::test::DataDir() / "packs");
  sys.SetStorage(dir, [] { return std::string("player"); });
  CHECK_EQ(sys.active_profile(), "player");

  CHECK(sys.IsUnlocked("my_game:ach10"));  // From legacy key "10".
  CHECK(sys.IsUnlocked("my_game", 10));
  CHECK_EQ(sys.GetProgress("my_game:ach10")->unlock_time, 1791923180ull);
  CHECK(!sys.IsUnlocked("my_game:ach15"));
  CHECK_EQ(sys.GetProgress("my_game:ach112")->completed.size(), 4u);  // completed_tricks.

  const std::string air = "Sample Achievement Mod:sample_air_time";
  CHECK(sys.IsUnlocked(air));
  CHECK_EQ(sys.GetProgress(air)->best_value, 1.7333323955535889);
  const std::string bones = "Sample Achievement Mod:sample_bones_broken";
  CHECK(!sys.IsUnlocked(bones));
  CHECK_EQ(sys.GetProgress(bones)->best_value, 1.0);
  const std::string other = "Sample Achievement Mod:sample_other_master";
  CHECK_EQ(sys.GetProgress(other)->completed,
           (std::vector<std::string>{"Nose Manual", "Manual", "Boneless"}));

  // An unlock rewrites the file under the new keys.
  CHECK(sys.Unlock("my_game:ach15"));
  const json saved = ::test::ReadJson(dir / "player_achievements.json");
  CHECK_EQ(saved["profile"], "player");
  const json& achs = saved["achievements"];
  CHECK(achs.contains("my_game:ach15"));
  CHECK(achs.contains("my_game:ach10"));
  CHECK(!achs.contains("10"));
  CHECK(!achs.contains("112"));
  CHECK_EQ(achs["my_game:ach10"]["unlock_time"], 1791923180);
  CHECK_EQ(achs["my_game:ach112"]["completed"].size(), 4u);
  CHECK(!achs["my_game:ach112"].contains("completed_tricks"));
  CHECK_EQ(achs[bones]["best_value"], 1.0);
  CHECK(!achs[bones].contains("best_stat"));
  CHECK_EQ(achs[other]["completed"].size(), 3u);
  // Sets that aren't loaded keep their progress.
  CHECK(achs.contains("Online:1"));
  CHECK_EQ(achs["Online:1"]["unlock_time"], 1792000000);
  // No progress, no entry.
  CHECK(!achs.contains("Sample Achievement Mod:sample_gap"));

  // Loading the new file gives the same state.
  System again;
  again.AddSet(BaseGame());
  again.LoadDirectory(::test::DataDir() / "packs");
  again.SetStorage(dir, [] { return std::string("player"); });
  CHECK(again.IsUnlocked("my_game:ach10"));
  CHECK(again.IsUnlocked("my_game:ach15"));
  CHECK_EQ(again.GetProgress(bones)->best_value, 1.0);
  CHECK_EQ(again.GetTotals().unlocked, sys.GetTotals().unlocked);
}

TEST(legacy_online_pack_loaded_later_finds_its_progress) {
  const auto dir = ::test::TempDir("legacy_online");
  fs::copy_file(::test::DataDir() / "legacy_save" / "player_achievements.json", dir / "player_achievements.json");
  System sys;
  sys.SetStorage(dir, [] { return std::string("player"); });
  CHECK(!sys.IsUnlocked("Online:1"));
  // Re-enabled pack: keys "Online:<id>" match the old save.
  json pack = json::parse(R"({"category": "Online", "achievements": [{"id": 1, "name": "Village", "gamerscore": 55}]})");
  AchievementSet set;
  CHECK(detail::ParsePackDocument(pack, "online_achievements", dir, set, nullptr));
  sys.AddSet(set);
  CHECK(sys.IsUnlocked("Online:1"));
}
