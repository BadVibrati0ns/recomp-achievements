#pragma once

// The achievement registry, trigger engine and save store. One System per
// game. Thread-safe: every public call takes the internal lock, and listeners
// are called after it is released.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/types.h"

namespace recomp::achievements {

// Sent to listeners when an achievement unlocks or moves forward. The view's
// pointers stay valid until its set is removed or replaced.
struct UnlockEvent {
  AchievementView view;
  bool replay = false;  // True for TestPopup(): nothing was saved.
};
struct ProgressEvent {
  AchievementView view;
  std::string item;  // Checklist item or target that was just completed.
};

class System {
 public:
  using UnlockListener = std::function<void(const UnlockEvent&)>;
  using ProgressListener = std::function<void(const ProgressEvent&)>;
  // Compares a reported name with a checklist item / target. The default
  // compares NormalizeName() of both.
  using NameMatcher = std::function<bool(std::string_view reported, std::string_view expected)>;
  using ProfileProvider = std::function<std::string()>;

  System();
  ~System();
  System(const System&) = delete;
  System& operator=(const System&) = delete;

  // ---- Definitions -------------------------------------------------------
  // Adding a set whose id is already loaded replaces it. Fills in keys,
  // categories and the points cap, then loads saved progress for it.
  void AddSet(AchievementSet set);
  // Loads one JSON set file (schema/achievement-set.schema.json).
  bool LoadSetFile(const std::filesystem::path& path, std::string* error = nullptr);
  // The same from JSON text, e.g. a set embedded in the executable. origin
  // stands in for the file: it names it in errors, supplies the default set
  // id (from "<id>.achievements.json") and the folder relative icons resolve
  // from.
  bool LoadSetJson(std::string_view json, const std::filesystem::path& origin,
                   std::string* error = nullptr);
  // Loads every *.achievements.json file in dir and, in each subfolder,
  // achievements.json or the "achievements" array of pack.json. Folders
  // ending in .disabled/.off/.bak/.tmp are skipped. Returns sets loaded.
  // Files that fail to load are skipped; pass `errors` to get one message per
  // file (disabled packs aren't errors).
  size_t LoadDirectory(const std::filesystem::path& dir, std::vector<std::string>* errors = nullptr);
  void RemoveSet(std::string_view set_id);
  void ClearSets();

  // Pointers stay valid until the set is removed or replaced.
  const AchievementSet* FindSet(std::string_view set_id) const;
  const Achievement* Find(std::string_view key) const;
  const Achievement* FindByPlatformId(std::string_view set_id, uint32_t platform_id) const;
  std::vector<const AchievementSet*> sets() const;

  // ---- Saves -------------------------------------------------------------
  // Progress is saved as <dir>/<profile>_achievements.json. The provider is
  // asked for the profile before each unlock, so switching profiles in game
  // reloads that profile's progress. With no storage, progress is in memory.
  void SetStorage(std::filesystem::path dir, ProfileProvider profile = nullptr);
  void ReloadProgress();
  std::string active_profile() const;
  // Increases whenever sets are added, replaced, removed or cleared, progress
  // is reset or reloaded, or the active profile changes. Compare with a
  // stored value to know when to re-read definitions and progress.
  uint64_t generation() const;

  // ---- Unlocking ---------------------------------------------------------
  // Returns true only when the achievement was locked and is now unlocked.
  bool Unlock(std::string_view key);
  bool Unlock(std::string_view set_id, uint32_t platform_id);
  bool IsUnlocked(std::string_view key) const;
  bool IsUnlocked(std::string_view set_id, uint32_t platform_id) const;
  std::optional<Progress> GetProgress(std::string_view key) const;
  void Reset();
  void Reset(const std::vector<std::string>& keys);

  // ---- Built-in triggers -------------------------------------------------
  void ReportStat(std::string_view stat, double value);
  void ReportEvent(std::string_view event, std::string_view target = {});
  // After AddStatAlias("combo", "tricks_chained"), ReportStat("combo", v)
  // also feeds "stat" triggers on "tricks_chained" (and the reverse).
  void AddStatAlias(std::string alias, std::string stat);
  void SetNameMatcher(NameMatcher matcher);
  // Moves a checklist/count achievement forward by hand; unlocks it when
  // complete. Returns true when the item was new.
  bool CompleteItem(std::string_view key, std::string_view item);
  // Records the best value for a game-defined trigger's progress bar.
  void RecordValue(std::string_view key, double value);
  // Same, and sets the progress bar's total for that achievement in
  // Snapshot(), overriding the trigger's (in memory only; total <= 0 clears).
  void RecordValue(std::string_view key, double value, double total);

  // Calls fn for every locked achievement whose trigger type is `type`, for
  // game-defined triggers. Runs on a copy, so fn may call Unlock().
  void ForEachPending(std::string_view type,
                      const std::function<void(const Achievement&, const Progress&)>& fn) const;
  bool HasPending(std::string_view type) const;

  // ---- UI ----------------------------------------------------------------
  // Every achievement, ordered by set (load order) then display_order.
  std::vector<AchievementView> Snapshot() const;
  struct Totals {
    uint32_t count = 0, unlocked = 0, points = 0, unlocked_points = 0;
  };
  Totals GetTotals(std::string_view set_id = {}) const;
  // Sends an unlock event with replay = true without saving anything. Empty
  // key: the first locked achievement (or the first one if all are unlocked).
  void TestPopup(std::string_view key = {});

  int AddUnlockListener(UnlockListener listener);
  int AddProgressListener(ProgressListener listener);
  void RemoveListener(int id);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// The default NameMatcher's normalisation: lower-case letters and digits only.
std::string NormalizeName(std::string_view name);

}  // namespace recomp::achievements
