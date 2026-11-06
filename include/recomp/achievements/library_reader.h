#pragma once

// Reading the shared achievement library (docs/library.md) for an app that
// shows every game's achievements. Only reads the library folder, never a
// game's own files. Missing or partial entries and older schemas load as far
// as they go; nothing here throws on bad data.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "recomp/achievements/config_dir.h"
#include "recomp/achievements/types.h"

namespace recomp::achievements {

// One line of events.log.
struct LibraryEvent {
  int v = 1;
  uint64_t t = 0;  // Unix milliseconds.
  std::string install_id;
  std::string game;
  std::string profile;
  std::string key;   // Achievement key; the set id for "set_enabled".
  std::string kind;  // "unlock", "progress", "reset", "set_enabled".
  nlohmann::json value;  // Progress count, the set's new state, or null.
  std::string item;      // The checklist item or target a progress step completed.
  std::string game_version;

  // The identity a merge dedupes by: (install_id, t, key, kind).
  bool SameAs(const LibraryEvent& other) const {
    return t == other.t && install_id == other.install_id && key == other.key && kind == other.kind;
  }
};
nlohmann::json EventToJson(const LibraryEvent& event);
// False for lines that aren't events (bad JSON, no kind or time).
bool EventFromJson(const nlohmann::json& j, LibraryEvent& out);
// Union of both lists, deduplicated by SameAs and sorted by time.
std::vector<LibraryEvent> MergeEvents(std::vector<LibraryEvent> a, const std::vector<LibraryEvent>& b);

struct LibraryTotals {
  uint32_t count = 0, unlocked = 0, points = 0, unlocked_points = 0;
};

struct LibraryProgress {
  Progress progress;
  uint32_t current = 0;  // Progress bar, as the game showed it.
  uint32_t total = 0;    // 0 = no bar.
};

struct LibraryProfile {
  std::string id;  // As stored: the game's profile name or a hash.
  uint64_t updated = 0;  // Unix seconds.
  LibraryTotals totals;  // Sets that are on.
  std::map<std::string, LibraryTotals> set_totals;
  std::map<std::string, LibraryProgress> progress;  // By achievement key.
};

struct LibrarySet {
  // Icons are absolute paths into the entry's icons/ folder (empty: none;
  // show the game icon).
  AchievementSet set;
  bool enabled = true;  // The player's choice at the last write.
};

struct LibraryGame {
  std::string id;
  std::filesystem::path dir;  // <root>/<id>
  int schema = 0;             // 0: no readable game.json.
  std::string title;
  std::vector<Platform> platforms;
  std::string game_version;
  std::string sdk_version;
  std::string install_id;
  std::filesystem::path icon;  // Absolute; empty when none.
  uint64_t last_played = 0;    // Unix seconds.
  bool wine = false;           // Written by a Windows build under Wine/Proton.
  std::vector<std::string> set_ids;  // The sets the game had loaded.
  std::vector<LibraryProfile> profiles;
  std::vector<LibrarySet> sets;  // LoadLibraryGame only.
  std::vector<std::string> warnings;

  // The profile with the most unlocked points (ties: the latest), or null.
  const LibraryProfile* BestProfile() const;
};

// Every game in a root, by id, with totals and progress but not the sets.
std::vector<LibraryGame> ListLibrary(const std::filesystem::path& root);
// The same over several roots. A game in more than one keeps the entry
// played last.
std::vector<LibraryGame> ListLibrary(const std::vector<std::filesystem::path>& roots);
// One game with its sets. Nullopt when the folder doesn't exist.
std::optional<LibraryGame> LoadLibraryGame(const std::filesystem::path& root, const std::string& game_id);

// Events at or after since_ms (unix ms) from events.log and rotated
// events-*.log files, sorted by time, deduplicated. Bad lines are skipped.
std::vector<LibraryEvent> ReadEvents(const LibraryGame& game, uint64_t since_ms = 0);

// Totals across games, each counted by its BestProfile().
LibraryTotals SumLibrary(const std::vector<LibraryGame>& games);

struct LibraryUnlock {
  std::string game;
  std::string profile;
  std::string key;
  uint64_t time = 0;  // Unix seconds.
};
// The latest unlocks across games, newest first.
std::vector<LibraryUnlock> RecentUnlocks(const std::vector<LibraryGame>& games, size_t limit = 20);

// Library folders inside Wine and Proton prefixes under a Linux home, for
// Windows builds that wrote there (no host found, or wine_host off): Steam's
// compatdata/*/pfx (native, ~/.steam and Flatpak Steam, plus extra Steam
// library folders from libraryfolders.vdf) and ~/.wine. Only existing folders.
std::vector<std::filesystem::path> FindWinePrefixLibraries(const std::filesystem::path& home);
// Where an app should look: DefaultLibraryRoot(false), then, off Windows, the
// prefixes under $HOME.
std::vector<std::filesystem::path> DefaultLibraryRoots();

}  // namespace recomp::achievements
