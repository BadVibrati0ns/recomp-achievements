#pragma once

// The achievement registry, trigger engine and save store. One System per
// game. Thread-safe: every public call takes the internal lock, and listeners
// are called after it is released.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
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
  // Every loaded set, turned on or not, in load order.
  std::vector<const AchievementSet*> sets() const;
  // Only the sets that are turned on.
  std::vector<const AchievementSet*> enabled_sets() const;

  // ---- Turning sets on and off -----------------------------------------
  // A set that's off can't unlock or move forward (Unlock() returns false and
  // triggers skip it), and is left out of Snapshot() and GetTotals(). Its
  // saved progress stays and counts again once it's back on. The choice is
  // the player's, for the game rather than per profile, and is saved in the
  // settings file. Sets without a choice use enabled_by_default. Works for
  // ids that aren't loaded yet.
  void SetSetEnabled(std::string_view set_id, bool enabled);
  bool IsSetEnabled(std::string_view set_id) const;

  // ---- Settings --------------------------------------------------------
  // Small player settings the UI keeps for this game (popup sound, ...),
  // saved next to the per-set choices. Values are any JSON; a null value
  // removes the key. GetSetting returns null for a missing key.
  nlohmann::json GetSetting(std::string_view key) const;
  void SetSetting(std::string_view key, nlohmann::json value);
  // The settings file. Default: <storage dir>/achievement_settings.json, or
  // in memory with no storage. Setting a path reads it (choices made before
  // are kept if the file doesn't exist yet); an empty path goes back to the
  // default.
  void SetSettingsPath(std::filesystem::path path);
  std::filesystem::path settings_path() const;

  // ---- Saves -------------------------------------------------------------
  // Progress is saved as <dir>/<profile>_achievements.json. The provider is
  // asked for the profile before each unlock, so switching profiles in game
  // reloads that profile's progress. With no storage, progress is in memory.
  void SetStorage(std::filesystem::path dir, ProfileProvider profile = nullptr);
  std::filesystem::path storage_dir() const;  // Empty: no storage.
  void ReloadProgress();
  // Replaces the active profile's progress in memory (by achievement key) and
  // makes `profile` active, without saving or calling listeners. For
  // read-only mirrors of progress kept elsewhere, such as the library app;
  // games use SetStorage(). Without storage nothing is ever written.
  void LoadProgress(const std::map<std::string, Progress>& progress, std::string profile = "default");
  // Unix seconds for unlock times; null goes back to the system clock. For
  // tests and tools that make demo data.
  void SetClock(std::function<uint64_t()> clock);
  std::string active_profile() const;
  // Increases whenever sets are added, replaced, removed, cleared or turned
  // on or off, progress is reset or reloaded, a setting changes, or the
  // active profile changes. Compare with a
  // stored value to know when to re-read definitions and progress.
  uint64_t generation() const;

  // ---- Unlocking ---------------------------------------------------------
  // Returns true only when the achievement was locked and is now unlocked.
  bool Unlock(std::string_view key);
  bool Unlock(std::string_view set_id, uint32_t platform_id);
  bool IsUnlocked(std::string_view key) const;
  bool IsUnlocked(std::string_view set_id, uint32_t platform_id) const;
  std::optional<Progress> GetProgress(std::string_view key) const;
  // Clears the active profile's progress: everything (sets that are off and
  // sets that aren't loaded included), the given keys, or one set.
  void Reset();
  void Reset(const std::vector<std::string>& keys);
  void ResetSet(std::string_view set_id);

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
  // Every achievement of the sets that are on, ordered by set (load order)
  // then display_order; include_disabled adds the sets that are off.
  std::vector<AchievementView> Snapshot(bool include_disabled = false) const;
  struct Totals {
    uint32_t count = 0, unlocked = 0, points = 0, unlocked_points = 0;
  };
  // Empty set_id: the sets that are on. A set id: that set, even when off
  // (so a settings page can show what turning it on would bring back).
  Totals GetTotals(std::string_view set_id = {}) const;
  // A consistent copy of every set for another thread (the library writer):
  // the shared pointers keep the sets alive, so the views stay valid for as
  // long as the State does. Views are in each set's own order, not
  // display_order.
  struct SetState {
    std::shared_ptr<const AchievementSet> set;
    bool enabled = true;
    std::vector<AchievementView> views;
  };
  struct State {
    std::vector<SetState> sets;  // Load order, on or off.
    std::string profile;
    uint64_t generation = 0;
  };
  State CopyState() const;
  // Sends an unlock event with replay = true without saving anything. Empty
  // key: the first locked achievement of a set that is on (or the first one
  // if all are unlocked).
  void TestPopup(std::string_view key = {});

  int AddUnlockListener(UnlockListener listener);
  int AddProgressListener(ProgressListener listener);
  void RemoveListener(int id);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Moves the achievements of `from` with these platform ids into a new set,
// before either is added, e.g. a title's online-only achievements into an
// optional pack that starts off. The new set keeps from's platform, category,
// credits and folder. Moved achievements keep their old key
// ("<from id>:<local id>") as a legacy key, so saved progress still counts.
// Ids from doesn't have are ignored.
AchievementSet SplitSet(AchievementSet& from, std::span<const uint32_t> platform_ids, std::string new_id,
                        std::string title, bool enabled_by_default = false);

// The default NameMatcher's normalisation: lower-case letters and digits only.
std::string NormalizeName(std::string_view name);

}  // namespace recomp::achievements
