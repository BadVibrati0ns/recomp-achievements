#pragma once

// The library app's view of the shared library: one summary per game (counted
// over the sets that are on), sort orders, cheap change detection, and a
// read-only System mirroring one game so the viewer can show it.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "recomp/achievements/library_reader.h"
#include "recomp/achievements/system.h"

namespace recomp::achievements::app {

struct TierCount {
  uint32_t unlocked = 0, total = 0;
};

struct GameSummary {
  std::string id;
  std::string title;
  std::filesystem::path root;  // The library folder the entry is in.
  std::filesystem::path icon;  // Empty: draw the trophy.
  Platform platform = Platform::kCustom;  // The first set's.
  uint64_t last_played = 0;  // Unix seconds.
  std::string profile;       // The profile counted.
  size_t profiles = 0;
  // Over the sets that are on; sets that are off never count.
  uint32_t count = 0, unlocked = 0, points = 0, unlocked_points = 0;
  TierCount tiers[5];  // By Tier; [0] unused.
  bool trophies = false;  // Any achievement has a tier.
  size_t sets_on = 0, sets_off = 0;
  bool missing = false;  // Its folder isn't there (LibraryGame::missing).

  float fraction() const { return count ? static_cast<float>(unlocked) / count : 0.0f; }
  bool complete() const { return count > 0 && unlocked == count; }
};

// The profile with this id, else the game's BestProfile(). Null when none.
const LibraryProfile* PickProfile(const LibraryGame& game, const std::string& id);
// game must have its sets (LoadLibraryGame).
GameSummary Summarize(const LibraryGame& game, const std::string& profile_id = {});

enum class SortMode { kLastPlayed, kPercent, kClosest, kTitle };
inline constexpr int kSortModes = 4;
const char* SortName(SortMode mode);
// Last played: newest first. % complete: highest first. Closest to done:
// unfinished games by achievements left (fewest first), finished ones last.
// A-Z: by title. Ties go by title, then id.
void SortGames(std::vector<GameSummary>& games, SortMode mode);

// File sizes and times under each entry, hashed: what changed since the last
// look. sets covers game.json, sets/ and icons/index.json; progress covers
// progress/. Keyed by "<root>|<game id>".
struct EntryStamp {
  uint64_t sets = 0, progress = 0;
  bool operator==(const EntryStamp&) const = default;
};
std::map<std::string, EntryStamp> StampLibrary(const std::vector<std::filesystem::path>& roots);

// Fills a System (with no storage, so it never writes) with a game's sets,
// their on/off state and one profile's progress. With sets = false only the
// progress is replaced, so the viewer keeps its place.
void MirrorGame(System& system, const LibraryGame& game, const LibraryProfile* profile, bool sets = true);

// "just now", "5 minutes ago", "yesterday", "3 weeks ago", ...
std::string RelativeTime(uint64_t then, uint64_t now);

}  // namespace recomp::achievements::app
