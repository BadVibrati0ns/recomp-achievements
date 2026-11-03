// The native set format and the save entry format.

#include "json_format.h"
#include "test.h"

using namespace recomp::achievements;
using nlohmann::json;

namespace {

AchievementSet ParseOk(const json& doc, const char* origin = "test.achievements.json") {
  AchievementSet set;
  std::string error;
  if (!detail::ParseNativeSet(doc, origin, set, &error)) ::test::Fail(__FILE__, __LINE__, "parse failed: " + error);
  return set;
}

std::string ParseError(const json& doc, const char* origin = "bad.achievements.json") {
  AchievementSet set;
  std::string error;
  if (detail::ParseNativeSet(doc, origin, set, &error)) ::test::Fail(__FILE__, __LINE__, "parse unexpectedly succeeded");
  return error;
}

json WithAchievement(json achievement) {
  return json{{"set", {{"id", "g"}}}, {"achievements", json::array({std::move(achievement)})}};
}

}  // namespace

TEST(native_minimal_defaults) {
  const auto set = ParseOk(json::parse(R"({"set": {"id": "g"}, "achievements": [{"id": "a", "name": "A"}]})"));
  CHECK_EQ(set.id, "g");
  CHECK_EQ(set.title, "g");
  CHECK(set.platform == Platform::kCustom);
  CHECK_EQ(set.points_cap, 0u);
  CHECK_EQ(set.achievements.size(), 1u);
  const Achievement& a = set.achievements[0];
  CHECK_EQ(a.local_id, "a");
  CHECK_EQ(a.trigger.type, "manual");
  CHECK_EQ(a.display_order, 0u);
  CHECK_EQ(a.points, 0u);
  CHECK(!a.secret);
  CHECK(a.tier == Tier::kNone);
  CHECK(a.trigger.params.is_object() && a.trigger.params.empty());
}

TEST(native_set_id_defaults_to_file_name) {
  const auto set = ParseOk(json::parse(R"({"achievements": [{"id": 1, "name": "A"}]})"),
                           "/games/sly.achievements.json");
  CHECK_EQ(set.id, "sly");
  CHECK_EQ(set.base_dir, std::filesystem::path("/games"));
  CHECK_EQ(set.achievements[0].local_id, "1");
}

TEST(native_full_example_file) {
  AchievementSet set;
  std::string error;
  const bool ok = detail::LoadSetFile(::test::DataDir() / "native" / "example.achievements.json", set, &error);
  CHECK_EQ(error, "");
  CHECK(ok);
  CHECK_EQ(set.id, "my_game");
  CHECK_EQ(set.title, "My Game");
  CHECK(set.platform == Platform::kXbox360);
  CHECK_EQ(set.category, "Career");
  CHECK_EQ(set.credits.size(), 1u);
  CHECK_EQ(set.credits[0].role, "original");
  CHECK_EQ(set.source["title_id"], "454108E6");
  CHECK_EQ(set.achievements.size(), 7u);

  const Achievement& board = set.achievements[0];
  CHECK_EQ(board.platform_id, 15u);
  CHECK_EQ(board.legacy_keys, std::vector<std::string>{"15"});
  CHECK_EQ(board.trigger.type, "manual");

  const Achievement& combo = set.achievements[1];
  CHECK_EQ(combo.trigger.type, "stat");
  CHECK_EQ(combo.trigger.stat, "tricks_chained");
  CHECK_EQ(combo.trigger.threshold, 10.0);

  const Achievement& miracle = set.achievements[2];
  CHECK_EQ(miracle.trigger.type, "event");  // Inferred from "event".
  CHECK_EQ(miracle.trigger.target, "Miracle Whip");
  CHECK(miracle.secret);
  CHECK_EQ(miracle.locked_description, "Land something miraculous.");

  CHECK_EQ(set.achievements[3].checklist.size(), 3u);
  CHECK_EQ(set.achievements[4].trigger.threshold, 16.0);

  const Achievement& gap = set.achievements[5];
  CHECK_EQ(gap.trigger.type, "gap");
  CHECK_EQ(gap.trigger.params["radius"], 8.5);
  CHECK_EQ(gap.trigger.params["takeoff"].size(), 3u);
  CHECK_EQ(gap.icon, "icons/gap.png");
  CHECK_EQ(set.achievements[6].display_order, 6u);
}

TEST(native_every_field) {
  const auto set = ParseOk(json::parse(R"({
    "schema": "recomp-achievements/1",
    "set": {"id": "rc", "title": "Ratchet", "platform": "PS3", "points_cap": 100,
            "credits": ["Someone", {"name": "Port Team", "role": "port", "url": "https://example.com"}]},
    "achievements": [{
      "id": "plat", "platform_id": 0, "name": "All", "description": "d", "locked_description": "?",
      "category": "Trophies", "points": 90, "tier": "platinum", "secret": true, "display_order": 7,
      "icon": "a.png", "locked_icon": "b.png", "legacy_keys": ["0", 12],
      "credits": [{"name": "Author", "role": "author"}],
      "trigger": {"type": "custom_thing", "params": {"x": 1}, "y": [1, 2]},
      "_note": "ignored"
    }]})"));
  CHECK(set.platform == Platform::kPlayStation3);
  CHECK_EQ(set.points_cap, 100u);
  CHECK_EQ(set.credits.size(), 2u);
  CHECK_EQ(set.credits[0].name, "Someone");
  CHECK_EQ(set.credits[1].url, "https://example.com");
  const Achievement& a = set.achievements[0];
  CHECK(a.tier == Tier::kPlatinum);
  CHECK_EQ(a.display_order, 7u);
  CHECK_EQ(a.locked_icon, "b.png");
  CHECK_EQ(a.legacy_keys.size(), 2u);
  CHECK_EQ(a.legacy_keys[1], "12");
  CHECK_EQ(a.credits[0].role, "author");
  CHECK_EQ(a.category, "Trophies");
  CHECK_EQ(a.trigger.params["x"], 1);
  CHECK_EQ(a.trigger.params["y"].size(), 2u);
}

TEST(native_trigger_short_forms) {
  auto set = ParseOk(WithAchievement({{"id", "a"}, {"name", "A"}, {"trigger", "manual"}}));
  CHECK_EQ(set.achievements[0].trigger.type, "manual");
  set = ParseOk(WithAchievement({{"id", "a"}, {"name", "A"}, {"trigger", "reach_location"}}));
  CHECK_EQ(set.achievements[0].trigger.type, "reach_location");
  set = ParseOk(WithAchievement({{"id", "a"}, {"name", "A"}, {"trigger", {{"stat", "speed"}, {"threshold", 60}}}}));
  CHECK_EQ(set.achievements[0].trigger.type, "stat");
  set = ParseOk(WithAchievement(
      {{"id", "a"}, {"name", "A"}, {"checklist", {"x", "y"}}, {"trigger", {{"event", "trick"}}}}));
  CHECK_EQ(set.achievements[0].trigger.type, "checklist");
  set = ParseOk(WithAchievement({{"id", "a"}, {"name", "A"}, {"trigger", {{"condition", "0xH1234=1"}}}}));
  CHECK_EQ(set.achievements[0].trigger.type, "memory");
  CHECK_EQ(set.achievements[0].trigger.params["condition"], "0xH1234=1");
  set = ParseOk(WithAchievement({{"id", "a"}, {"name", "A"}, {"trigger", {{"target", 7}}}}));
  CHECK_EQ(set.achievements[0].trigger.type, "manual");
  CHECK_EQ(set.achievements[0].trigger.target, "7");
}

TEST(native_trophy_zero_as_id) {
  const auto set = ParseOk(json::parse(R"({"set": {"id": "np", "platform": "ps3"},
      "achievements": [{"platform_id": 0, "name": "Platinum", "tier": "platinum"}]})"));
  CHECK_EQ(set.achievements[0].local_id, "0");
  CHECK_EQ(set.achievements[0].platform_id, 0u);
}

TEST(native_id_from_platform_id) {
  const auto set = ParseOk(WithAchievement({{"platform_id", 42}, {"name", "A"}}));
  CHECK_EQ(set.achievements[0].local_id, "42");
  CHECK_EQ(set.achievements[0].platform_id, 42u);
}

TEST(native_errors_name_file_and_index) {
  const json two = json::parse(R"({"set": {"id": "g"}, "achievements": [
      {"id": "ok", "name": "Fine"}, {"id": "b", "name": "Broken", "descripton": "typo"}]})");
  const std::string e = ParseError(two);
  CHECK_CONTAINS(e, "bad.achievements.json");
  CHECK_CONTAINS(e, "achievements[1]");
  CHECK_CONTAINS(e, "\"Broken\"");
  CHECK_CONTAINS(e, "unknown field \"descripton\"");
}

TEST(native_errors) {
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}})), "needs a \"name\"");
  CHECK_CONTAINS(ParseError(WithAchievement({{"name", "A"}})), "needs an \"id\"");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", ""}, {"name", "A"}})), "must not be empty");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", true}, {"name", "A"}})), "expected a string or number");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}, {"name", "A"}, {"points", -5}})),
                 "achievements[0] (\"A\").points: expected a whole number");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}, {"name", "A"}, {"points", 2.5}})), "whole number");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}, {"name", "A"}, {"tier", "diamond"}})),
                 "\"diamond\" is not one of");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}, {"name", "A"}, {"secret", "yes"}})),
                 "secret: expected true or false");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}, {"name", "A"}, {"trigger", {{"type", "stat"}}}})),
                 "needs a \"stat\" name");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}, {"name", "A"}, {"trigger", {{"type", "event"}}}})),
                 "needs an \"event\" name");
  CHECK_CONTAINS(ParseError(WithAchievement(
                     {{"id", "a"}, {"name", "A"}, {"trigger", {{"type", "checklist"}, {"event", "e"}}}})),
                 "non-empty \"checklist\"");
  CHECK_CONTAINS(ParseError(WithAchievement(
                     {{"id", "a"}, {"name", "A"}, {"trigger", {{"type", "count"}, {"event", "e"}}}})),
                 "\"threshold\" of at least 1");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}, {"name", "A"}, {"trigger", {{"type", "memory"}}}})),
                 "\"condition\" string");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}, {"name", "A"}, {"trigger", 5}})),
                 "trigger: expected a type name or an object");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}, {"name", "A"}, {"checklist", {"x", 3}}})),
                 "checklist[1]: expected a string");
  CHECK_CONTAINS(ParseError(WithAchievement({{"id", "a"}, {"name", "A"}, {"credits", {{{"role", "x"}}}}})),
                 "needs a \"name\"");

  CHECK_CONTAINS(ParseError(json::parse(R"({"set": {"id": "g"}, "achievements": [
      {"id": "a", "name": "A"}, {"id": "a", "name": "B"}]})")),
                 "achievements[1]: duplicate id \"a\"");
  CHECK_CONTAINS(ParseError(json::parse(R"({"set": {"id": "g"}})")), "needs an \"achievements\" array");
  CHECK_CONTAINS(ParseError(json::parse(R"({"set": {"id": "g", "platform": "n64"}, "achievements": []})")),
                 "set.platform: \"n64\" is not one of");
  CHECK_CONTAINS(ParseError(json::parse(R"({"set": {"id": "g", "colour": 1}, "achievements": []})")),
                 "set: unknown field \"colour\"");
  CHECK_CONTAINS(ParseError(json::parse(R"({"schema": "recomp-achievements/2", "achievements": []})")),
                 "newer than this SDK supports");
  CHECK_CONTAINS(ParseError(json::parse(R"({"schema": "other/1", "achievements": []})")),
                 "not a recomp-achievements schema");
  CHECK_CONTAINS(ParseError(json::parse(R"([1, 2])")), "expected an object");
}

TEST(native_file_errors) {
  const auto dir = ::test::TempDir("native_file_errors");
  AchievementSet set;
  std::string error;
  ::test::WriteFile(dir / "broken.achievements.json", "{\n  \"set\": {\"id\": \"g\"},\n  \"achievements\": [,]\n}");
  CHECK(!detail::LoadSetFile(dir / "broken.achievements.json", set, &error));
  CHECK_CONTAINS(error, "broken.achievements.json: invalid JSON");
  CHECK_CONTAINS(error, "line 3");

  CHECK(!detail::LoadSetFile(dir / "missing.achievements.json", set, &error));
  CHECK_CONTAINS(error, "cannot open file");

  // Hand-written files may carry comments.
  ::test::WriteFile(dir / "commented.achievements.json",
                    "// my set\n{\"achievements\": [ /* one */ {\"id\": \"a\", \"name\": \"A\"}]}");
  CHECK(detail::LoadSetFile(dir / "commented.achievements.json", set, &error));
  CHECK_EQ(set.id, "commented");
}

TEST(native_round_trip) {
  AchievementSet set;
  CHECK(detail::LoadSetFile(::test::DataDir() / "native" / "example.achievements.json", set, nullptr));
  const json written = detail::NativeSetToJson(set);
  CHECK_EQ(written["schema"], "recomp-achievements/1");
  AchievementSet again = ParseOk(written);
  CHECK_EQ(again.id, set.id);
  CHECK(again.platform == set.platform);
  CHECK_EQ(again.achievements.size(), set.achievements.size());
  for (size_t i = 0; i < set.achievements.size(); ++i) {
    const auto& a = set.achievements[i];
    const auto& b = again.achievements[i];
    CHECK_EQ(a.local_id, b.local_id);
    CHECK_EQ(a.name, b.name);
    CHECK_EQ(a.points, b.points);
    CHECK_EQ(a.secret, b.secret);
    CHECK_EQ(a.display_order, b.display_order);
    CHECK_EQ(a.trigger.type, b.trigger.type);
    CHECK_EQ(a.trigger.stat, b.trigger.stat);
    CHECK_EQ(a.trigger.event, b.trigger.event);
    CHECK_EQ(a.trigger.target, b.trigger.target);
    CHECK_EQ(a.trigger.threshold, b.trigger.threshold);
    CHECK_EQ(a.trigger.params, b.trigger.params);
    CHECK_EQ(a.checklist, b.checklist);
    CHECK_EQ(a.legacy_keys, b.legacy_keys);
  }
  // Trigger params that shadow a known field survive under "params".
  AchievementSet tricky;
  tricky.id = "t";
  Achievement a;
  a.local_id = "x";
  a.name = "X";
  a.trigger.type = "custom";
  a.trigger.params = {{"stat", "inner"}, {"radius", 2}};
  tricky.achievements.push_back(a);
  const auto back = ParseOk(detail::NativeSetToJson(tricky));
  CHECK_EQ(back.achievements[0].trigger.params, a.trigger.params);
  CHECK_EQ(back.achievements[0].trigger.stat, "");
}

TEST(schema_file_is_draft_2020_12) {
  const json schema = ::test::ReadJson(RECOMP_ACH_SCHEMA);
  CHECK(schema.is_object());
  CHECK_EQ(schema["$schema"], "https://json-schema.org/draft/2020-12/schema");
  CHECK(schema["properties"].contains("achievements"));
  CHECK(schema["$defs"].contains("trigger"));
}

TEST(progress_entry_reads_old_names) {
  const auto p = detail::ProgressFromJson(json::parse(
      R"({"unlocked": false, "unlock_time": 0, "completed_tricks": ["Manual"], "best_stat": 1.75})"));
  CHECK(!p.unlocked);
  CHECK_EQ(p.completed, std::vector<std::string>{"Manual"});
  CHECK_EQ(p.best_value, 1.75);

  const auto q = detail::ProgressFromJson(json::parse(
      R"({"unlocked": true, "unlock_time": 1792116599, "completed": ["a"], "completed_tricks": ["b"],
          "count": 3, "best_value": 2, "best_stat": 9})"));
  CHECK(q.unlocked);
  CHECK_EQ(q.unlock_time, 1792116599ull);
  CHECK_EQ(q.completed, std::vector<std::string>{"a"});
  CHECK_EQ(q.count, 3u);
  CHECK_EQ(q.best_value, 2.0);

  CHECK(!detail::HasProgress(detail::ProgressFromJson(json::parse("{}"))));
  CHECK(!detail::HasProgress(detail::ProgressFromJson(json::parse("[1]"))));
}

TEST(progress_entry_writes_new_names_and_keeps_foreign_fields) {
  Progress p;
  p.unlocked = true;
  p.unlock_time = 5;
  p.best_value = 3.5;
  const json out = detail::ProgressToJson(
      p, json::parse(R"({"best_stat": 1, "completed_tricks": ["x"], "game_note": "kept", "count": 4})"));
  CHECK_EQ(out["unlocked"], true);
  CHECK_EQ(out["unlock_time"], 5);
  CHECK_EQ(out["best_value"], 3.5);
  CHECK_EQ(out["game_note"], "kept");
  CHECK(!out.contains("best_stat"));
  CHECK(!out.contains("completed_tricks"));
  CHECK(!out.contains("completed"));
  CHECK(!out.contains("count"));
}

namespace {
std::vector<uint32_t> Capped(std::vector<uint32_t> points, uint32_t cap) {
  std::vector<Achievement> list(points.size());
  for (size_t i = 0; i < points.size(); ++i) list[i].points = points[i];
  detail::ApplyPointsCap(list, cap);
  for (size_t i = 0; i < points.size(); ++i) points[i] = list[i].points;
  return points;
}
}  // namespace

TEST(points_cap_matches_old_scaling) {
  // Under the cap: untouched, even if not exactly the cap.
  CHECK_EQ(Capped({100, 200}, 1000), (std::vector<uint32_t>{100, 200}));
  // 333.3 each rounds to 999; the first largest gets the missing point.
  CHECK_EQ(Capped({500, 500, 500}, 1000), (std::vector<uint32_t>{334, 333, 333}));
  CHECK_EQ(Capped({500, 500, 100, 10}, 1000), (std::vector<uint32_t>{451, 450, 90, 9}));
  // Rounding up overshoots: trim the first largest until it fits.
  CHECK_EQ(Capped({3, 3, 3, 3, 3, 3}, 10), (std::vector<uint32_t>{1, 1, 2, 2, 2, 2}));
  // Zero stays zero; everything else keeps at least one point.
  CHECK_EQ(Capped({0, 1000, 1}, 100), (std::vector<uint32_t>{0, 99, 1}));
  // All ones over the cap: the old loop gives up rather than zeroing.
  CHECK_EQ(Capped(std::vector<uint32_t>(12, 1), 10), std::vector<uint32_t>(12, 1));
  CHECK_EQ(Capped({600}, 0), std::vector<uint32_t>{600});
}
