#pragma once

// Private: reading and writing achievement sets and saves as JSON. The format
// is documented in docs/json-format.md and schema/achievement-set.schema.json.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "recomp/achievements/types.h"

namespace recomp::achievements::detail {

inline constexpr std::string_view kSchemaId = "recomp-achievements/1";

std::string_view PlatformName(Platform platform);
std::optional<Platform> ParsePlatform(std::string_view name);
std::string_view TierName(Tier tier);
std::optional<Tier> ParseTier(std::string_view name);

// True when a document looks like the native format rather than an old pack.
bool IsNativeDocument(const nlohmann::json& doc);

// Native format. `origin` names the file in error messages and supplies the
// default set id; base_dir is set to its folder.
bool ParseNativeSet(const nlohmann::json& doc, const std::filesystem::path& origin,
                    AchievementSet& out, std::string* error);
nlohmann::json NativeSetToJson(const AchievementSet& set);

// Old mod pack format: achievements.json (object or bare array) or the
// "achievements" array of pack.json. folder_name is the fallback set id.
// Returns false with an empty error for packs that are disabled or have no
// achievements, which are skipped rather than reported.
bool ParsePackDocument(const nlohmann::json& doc, std::string_view folder_name,
                       const std::filesystem::path& base_dir, AchievementSet& out,
                       std::string* error);

// Reads any set file: native, or an old pack's achievements.json/pack.json.
bool LoadSetFile(const std::filesystem::path& path, AchievementSet& out, std::string* error);
// The same from text already in memory; origin stands in for the file path.
bool LoadSetText(std::string_view text, const std::filesystem::path& origin, AchievementSet& out,
                 std::string* error);
// Loads a mod folder: its *.achievements.json files, else achievements.json,
// else pack.json. Problems are appended to errors.
std::vector<AchievementSet> LoadPackFolder(const std::filesystem::path& folder,
                                           std::vector<std::string>* errors);

// One save entry. Accepts the old field names (completed_tricks, best_stat).
Progress ProgressFromJson(const nlohmann::json& entry);
// Writes progress over an existing entry, keeping fields this SDK doesn't own.
nlohmann::json ProgressToJson(const Progress& progress, nlohmann::json entry = nlohmann::json::object());
bool HasProgress(const Progress& progress);

// Scales points to fit the cap the way older mod packs did: round each
// to the scaled value (at least 1), then trim or pad the largest until the
// total is exactly the cap. Leaves sets at or under the cap untouched.
void ApplyPointsCap(std::vector<Achievement>& achievements, uint32_t cap);

}  // namespace recomp::achievements::detail
