#pragma once

// Plain data types shared by every part of the SDK. Nothing here depends on a
// recomp framework, a renderer or an audio backend.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace recomp::achievements {

// Where a set's definitions came from. Decides the default popup theme and
// sound, and how platform ids are interpreted.
enum class Platform {
  kCustom,             // A game's own list, written in JSON or code.
  kXbox360,            // XDBF/SPA achievements (platform_id = XDBF id).
  kPlayStation3,       // TROPHY.TRP trophies (platform_id = trophy id).
  kRetroAchievements,  // A copy of a public RetroAchievements list.
};

// PlayStation trophy grade. kNone for every other platform.
enum class Tier { kNone, kBronze, kSilver, kGold, kPlatinum };

// One person or group to credit. RetroAchievements imports carry the set's
// achievement authors here; the UI shows them on the achievement and set.
struct Credit {
  std::string name;
  std::string role;  // "author", "artwork", "port", ...
  std::string url;
};

// How an achievement unlocks. type selects the rule:
//   "manual"    nothing automatic; the game calls Unlock().
//   "stat"      ReportStat(stat, v) with v >= threshold.
//   "event"     ReportEvent(event, t) where target is empty/"any" or matches t.
//   "checklist" ReportEvent(event, t) for every entry of Achievement::checklist.
//   "count"     ReportEvent(event, t) for `threshold` distinct targets (or
//               occurrences when params.distinct is false).
//   "memory"    a RetroAchievements-syntax condition in params.condition,
//               evaluated against guest memory (see memory_triggers.h).
// Any other type is game-defined: the game evaluates it itself, usually with
// System::ForEachPending(type, ...), and the raw JSON is kept in params.
struct Trigger {
  std::string type = "manual";
  std::string stat;    // "stat"
  std::string event;   // "event", "checklist", "count"
  std::string target;  // "event", "count" (optional filter)
  double threshold = 0.0;
  nlohmann::json params = nlohmann::json::object();
};

struct Achievement {
  // Stable key, unique across all loaded sets: "<set id>:<local id>". Saves
  // are stored by key so ids can change between versions without losing
  // progress.
  std::string key;
  std::string local_id;
  // Older save keys for this achievement, read when `key` isn't in a save
  // (e.g. "105" from before the set had an id). Saving always uses `key`.
  std::vector<std::string> legacy_keys;
  // XDBF / trophy / RetroAchievements id. 0 means "none" except in PS3 sets,
  // where trophy 0 (usually the platinum) is a real id.
  uint32_t platform_id = 0;
  std::string set_id;

  std::string name;
  std::string description;
  std::string locked_description;  // Shown while locked (secret text).
  std::string category;            // Defaults to the set's category.
  uint32_t points = 0;             // Gamerscore or RetroAchievements points.
  Tier tier = Tier::kNone;
  bool secret = false;
  uint32_t display_order = 0;

  // Icon reference, resolved by an IconSource: a file path relative to the
  // set file, "xdbf:<image id>", "trophy:<file>" or a RetroAchievements
  // badge name ("ra:<badge>").
  std::string icon;
  std::string locked_icon;

  Trigger trigger;
  std::vector<std::string> checklist;  // "checklist" items, in display order.
  std::vector<Credit> credits;
};

struct AchievementSet {
  std::string id;  // Unique; prefixes every achievement key.
  std::string title;
  Platform platform = Platform::kCustom;
  std::string category;     // Default category for its achievements.
  uint32_t points_cap = 0;  // 0 = no cap; otherwise points are scaled to fit.
  // Whether the set counts until the player turns it on or off
  // (System::SetSetEnabled). Off suits optional packs, e.g. achievements
  // that need servers that are gone.
  bool enabled_by_default = true;
  std::vector<Credit> credits;
  nlohmann::json source = nlohmann::json::object();  // Provenance (game id, URL, date).
  std::filesystem::path base_dir;  // Folder relative icon paths resolve from.
  std::vector<Achievement> achievements;
};

// Per-profile progress for one achievement.
struct Progress {
  bool unlocked = false;
  uint64_t unlock_time = 0;            // Unix seconds.
  std::vector<std::string> completed;  // Checklist items / distinct targets done.
  uint32_t count = 0;                  // Occurrences for non-distinct "count".
  double best_value = 0.0;             // Best "stat" value seen while locked.
};

// What the UI needs to draw one row or popup.
struct AchievementView {
  const Achievement* achievement = nullptr;
  const AchievementSet* set = nullptr;
  Progress progress;
  uint32_t progress_current = 0;
  uint32_t progress_total = 0;  // 0 = no progress bar.
};

}  // namespace recomp::achievements
