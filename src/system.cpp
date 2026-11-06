#include "recomp/achievements/system.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <variant>

#include "json_format.h"

namespace recomp::achievements {

namespace fs = std::filesystem;
using nlohmann::json;

std::string NormalizeName(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (const char c : name) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc >= 'A' && uc <= 'Z') {
      out.push_back(static_cast<char>(uc - 'A' + 'a'));
    } else if ((uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9')) {
      out.push_back(c);
    } else if (uc >= 0x80) {
      // Keep UTF-8 so non-Latin names don't all normalise to "".
      out.push_back(c);
    }
  }
  return out;
}

namespace {

struct SetEntry {
  // Shared so events queued for listeners keep their set alive even if it is
  // replaced before they are delivered.
  std::shared_ptr<const AchievementSet> set;
  std::vector<Progress> progress;
  bool enabled = true;  // The player's choice, else the set's default.
};

struct Loc {
  SetEntry* entry = nullptr;
  size_t index = 0;

  const Achievement& achievement() const { return entry->set->achievements[index]; }
  Progress& progress() const { return entry->progress[index]; }
};

struct Outbox {
  std::vector<std::variant<UnlockEvent, ProgressEvent>> events;
  std::vector<std::shared_ptr<const AchievementSet>> keep_alive;
};

bool IsAny(std::string_view target) {
  return target.empty() || NormalizeName(target) == "any";
}

bool IsDistinct(const Trigger& t) {
  const auto it = t.params.find("distinct");
  return it == t.params.end() || !it->is_boolean() || it->get<bool>();
}

uint32_t ThresholdCount(double threshold) {
  if (!(threshold > 0.0)) return 0;
  return static_cast<uint32_t>(std::min(std::ceil(threshold), 4294967295.0));
}

bool IsBuiltinEventType(std::string_view type) {
  return type == "event" || type == "checklist" || type == "count";
}

uint64_t Now() {
  return static_cast<uint64_t>(std::time(nullptr));
}

// Profile ids come from the game (gamertags, XUIDs, names); keep them usable
// as file names without changing ordinary ones.
std::string FileSafe(std::string_view profile) {
  std::string out(profile);
  for (char& c : out) {
    const auto uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) c = '_';
  }
  if (out.empty() || out == "." || out == "..") out = "default";
  return out;
}

// Write-then-rename so a crash mid-save can't leave a truncated file.
void WriteJsonAtomic(const fs::path& path, const json& doc) {
  fs::path tmp = path;
  tmp += ".tmp";
  std::error_code ec;
  if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << doc.dump(2) << '\n';
    if (!out.good()) return;
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << doc.dump(2) << '\n';
  }
}

// Reads a JSON object; an unreadable file is kept as <file>.corrupt before
// the next save replaces it. Null when missing or unreadable.
json ReadJsonObject(const fs::path& path) {
  std::error_code ec;
  if (path.empty() || !fs::exists(path, ec)) return nullptr;
  std::ifstream in(path, std::ios::binary);
  json doc = json::parse(in, nullptr, /*allow_exceptions=*/false);
  in.close();
  if (doc.is_object()) return doc;
  fs::copy_file(path, fs::path(path) += ".corrupt", fs::copy_options::overwrite_existing, ec);
  return nullptr;
}

constexpr std::string_view kSettingsFile = "achievement_settings.json";
constexpr std::string_view kSettingsSchema = "recomp-achievements-settings/1";

// The id AddSet() will give a set.
std::string EffectiveSetId(const AchievementSet& set) {
  if (!set.id.empty()) return set.id;
  std::string id = NormalizeName(set.title);
  return id.empty() ? std::string("default") : id;
}

// The local id AddSet() will give the achievement at `index`.
std::string EffectiveLocalId(const AchievementSet& set, const std::string& set_id, size_t index) {
  const Achievement& a = set.achievements[index];
  if (!a.local_id.empty()) return a.local_id;
  const std::string prefix = set_id + ":";
  if (!a.key.empty()) return a.key.rfind(prefix, 0) == 0 ? a.key.substr(prefix.size()) : a.key;
  const bool has_id = a.platform_id != 0 || set.platform == Platform::kPlayStation3;
  return std::to_string(has_id ? a.platform_id : static_cast<uint32_t>(index));
}

}  // namespace

struct System::Impl {
  mutable std::mutex mutex;
  std::vector<std::unique_ptr<SetEntry>> sets;  // Load order.
  std::unordered_map<std::string, Loc> by_key;  // Keys, then legacy keys.

  fs::path dir;
  ProfileProvider provider;
  std::string profile = "default";
  // The active profile's "achievements" object. It also holds entries for
  // sets that aren't loaded, so saving never drops them.
  json stored = json::object();
  std::map<std::string, json> memory_saves;  // Per profile, when there's no dir.
  bool dirty = false;  // Unsaved best values.
  uint64_t generation = 0;
  // Progress bar totals from RecordValue(key, value, total); never saved.
  std::unordered_map<std::string, uint32_t> bar_totals;
  std::function<uint64_t()> clock;  // SetClock(); null = the system clock.
  // achievement_settings.json: {"sets": {id: {"enabled": b}}, "settings": {...}}.
  // Entries for sets that aren't loaded and unknown fields are kept.
  fs::path settings_override;
  json settings = json::object();

  std::unordered_map<std::string, std::set<std::string>> aliases;  // Normalised.
  NameMatcher matcher;

  std::vector<std::pair<int, UnlockListener>> unlock_listeners;
  std::vector<std::pair<int, ProgressListener>> progress_listeners;
  int next_listener_id = 1;

  // ---- Lookup (lock held) ----------------------------------------------

  void RebuildIndex() {
    by_key.clear();
    for (const auto& entry : sets) {
      for (size_t i = 0; i < entry->set->achievements.size(); ++i) {
        by_key[entry->set->achievements[i].key] = {entry.get(), i};
      }
    }
    for (const auto& entry : sets) {
      for (size_t i = 0; i < entry->set->achievements.size(); ++i) {
        for (const auto& legacy : entry->set->achievements[i].legacy_keys) {
          by_key.emplace(legacy, Loc{entry.get(), i});
        }
      }
    }
  }

  std::optional<Loc> Lookup(std::string_view key) const {
    const auto it = by_key.find(std::string(key));
    if (it == by_key.end()) return std::nullopt;
    return it->second;
  }

  SetEntry* FindEntry(std::string_view set_id) const {
    for (const auto& entry : sets) {
      if (entry->set->id == set_id) return entry.get();
    }
    return nullptr;
  }

  std::optional<Loc> LookupPlatformId(std::string_view set_id, uint32_t platform_id) const {
    SetEntry* entry = FindEntry(set_id);
    if (!entry) return std::nullopt;
    // Trophy 0 is real (usually the platinum); elsewhere 0 means "no id".
    if (platform_id == 0 && entry->set->platform != Platform::kPlayStation3) return std::nullopt;
    const auto& list = entry->set->achievements;
    for (size_t i = 0; i < list.size(); ++i) {
      if (list[i].platform_id == platform_id) return Loc{entry, i};
    }
    return std::nullopt;
  }

  bool IsLoadedKey(const std::string& key) const {
    const auto it = by_key.find(key);
    return it != by_key.end() && it->second.achievement().key == key;
  }

  // ---- Matching --------------------------------------------------------

  bool Matches(std::string_view reported, std::string_view expected) const {
    if (matcher) {
      try {
        return matcher(reported, expected);
      } catch (...) {
        return false;
      }
    }
    const std::string r = NormalizeName(reported);
    const std::string e = NormalizeName(expected);
    if (r.empty() || e.empty()) return reported == expected;
    return r == e;
  }

  bool TargetMatches(std::string_view reported, std::string_view expected) const {
    return IsAny(expected) || Matches(reported, expected);
  }

  static bool SameName(std::string_view a, std::string_view b) {
    const std::string na = NormalizeName(a);
    const std::string nb = NormalizeName(b);
    return na.empty() || nb.empty() ? a == b : na == nb;
  }

  std::set<std::string> StatNames(std::string_view stat) const {
    std::set<std::string> names = {NormalizeName(stat)};
    std::vector<std::string> queue(names.begin(), names.end());
    while (!queue.empty()) {
      const std::string name = std::move(queue.back());
      queue.pop_back();
      const auto it = aliases.find(name);
      if (it == aliases.end()) continue;
      for (const auto& other : it->second) {
        if (names.insert(other).second) queue.push_back(other);
      }
    }
    return names;
  }

  bool IsDone(const Progress& p, std::string_view item) const {
    return std::any_of(p.completed.begin(), p.completed.end(),
                       [&](const std::string& done) { return Matches(done, item); });
  }

  // Checklist entries done; with no checklist, distinct items or occurrences.
  uint32_t CurrentCount(const Achievement& a, const Progress& p) const {
    if (!a.checklist.empty()) {
      return static_cast<uint32_t>(std::count_if(a.checklist.begin(), a.checklist.end(),
                                                 [&](const std::string& item) { return IsDone(p, item); }));
    }
    if (a.trigger.type == "count" && !IsDistinct(a.trigger)) return p.count;
    return static_cast<uint32_t>(p.completed.size()) + p.count;
  }

  uint32_t NeededCount(const Achievement& a) const {
    if (!a.checklist.empty()) return static_cast<uint32_t>(a.checklist.size());
    const uint32_t n = ThresholdCount(a.trigger.threshold);
    return a.trigger.type == "count" ? std::max(n, 1u) : n;
  }

  // ---- Views -----------------------------------------------------------

  AchievementView View(const SetEntry& entry, size_t index) const {
    AchievementView view;
    view.set = entry.set.get();
    view.achievement = &entry.set->achievements[index];
    view.progress = entry.progress[index];
    const Achievement& a = *view.achievement;
    const Progress& p = view.progress;
    const Trigger& t = a.trigger;

    const auto bar = t.params.find("progress_bar");
    const auto total = bar_totals.find(a.key);
    if (total != bar_totals.end()) {
      view.progress_total = total->second;
      view.progress_current = static_cast<uint32_t>(std::clamp(std::floor(p.best_value), 0.0, 4294967295.0));
    } else if (bar != t.params.end() && bar->is_boolean() && !bar->get<bool>()) {
      return view;
    } else if (!a.checklist.empty() || t.type == "count") {
      view.progress_total = NeededCount(a);
      view.progress_current = CurrentCount(a, p);
    } else if (t.type != "manual" && t.type != "event" && t.type != "checklist" &&
               t.threshold >= 2.0) {
      // "stat", plus game-defined types fed through RecordValue/CompleteItem.
      view.progress_total = static_cast<uint32_t>(std::round(t.threshold));
      const double best = std::max(0.0, std::floor(p.best_value));
      view.progress_current = std::max(static_cast<uint32_t>(std::min(best, 4294967295.0)),
                                       static_cast<uint32_t>(p.completed.size()) + p.count);
    }
    if (p.unlocked) view.progress_current = view.progress_total;
    view.progress_current = std::min(view.progress_current, view.progress_total);
    return view;
  }

  AchievementView View(Loc loc) const { return View(*loc.entry, loc.index); }

  static std::vector<size_t> DisplayOrder(const AchievementSet& set) {
    const auto& list = set.achievements;
    std::vector<size_t> order(list.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return list[a].display_order < list[b].display_order; });
    return order;
  }

  void QueueUnlock(Loc loc, bool replay, Outbox& out) const {
    out.events.emplace_back(UnlockEvent{View(loc), replay});
    out.keep_alive.push_back(loc.entry->set);
  }

  void QueueProgress(Loc loc, std::string item, Outbox& out) const {
    out.events.emplace_back(ProgressEvent{View(loc), std::move(item)});
    out.keep_alive.push_back(loc.entry->set);
  }

  // ---- Settings (lock held) --------------------------------------------

  fs::path SettingsPath() const {
    if (!settings_override.empty()) return settings_override;
    return dir.empty() ? fs::path() : dir / kSettingsFile;
  }

  std::optional<bool> Choice(std::string_view set_id) const {
    const auto sets_it = settings.find("sets");
    if (sets_it == settings.end() || !sets_it->is_object()) return std::nullopt;
    const auto it = sets_it->find(std::string(set_id));
    if (it == sets_it->end() || !it->is_object()) return std::nullopt;
    const auto e = it->find("enabled");
    if (e == it->end() || !e->is_boolean()) return std::nullopt;
    return e->get<bool>();
  }

  bool EnabledFor(const AchievementSet& set) const { return Choice(set.id).value_or(set.enabled_by_default); }

  void ApplyChoices() {
    for (auto& entry : sets) entry->enabled = EnabledFor(*entry->set);
  }

  void LoadSettings() {
    json doc = ReadJsonObject(SettingsPath());
    if (doc.is_object()) settings = std::move(doc);
    ApplyChoices();
  }

  void SaveSettings() {
    if (!settings.contains("schema")) settings["schema"] = kSettingsSchema;
    const fs::path path = SettingsPath();
    if (!path.empty()) WriteJsonAtomic(path, settings);
  }

  // ---- Saves (lock held) -----------------------------------------------

  fs::path SavePath() const {
    return dir / (FileSafe(profile) + "_achievements.json");
  }

  // The provider runs under the lock, so it must not call back into System.
  std::string QueryProfile() const {
    if (!provider) return profile;
    try {
      std::string id = provider();
      return id.empty() ? std::string("default") : id;
    } catch (...) {
      return profile;
    }
  }

  void LoadStored() {
    stored = json::object();
    if (dir.empty()) {
      if (const auto it = memory_saves.find(profile); it != memory_saves.end()) stored = it->second;
      return;
    }
    json doc = ReadJsonObject(SavePath());
    if (!doc.is_object()) return;
    if (const auto it = doc.find("achievements"); it != doc.end() && it->is_object()) stored = std::move(*it);
  }

  const json* StoredEntry(const Achievement& a) const {
    if (const auto it = stored.find(a.key); it != stored.end()) return &*it;
    for (const auto& legacy : a.legacy_keys) {
      if (const auto it = stored.find(legacy); it != stored.end()) return &*it;
    }
    return nullptr;
  }

  void ApplyStored(SetEntry& entry) {
    const auto& list = entry.set->achievements;
    entry.progress.assign(list.size(), Progress{});
    for (size_t i = 0; i < list.size(); ++i) {
      if (const json* e = StoredEntry(list[i])) entry.progress[i] = detail::ProgressFromJson(*e);
    }
  }

  void ApplyStoredAll() {
    for (auto& entry : sets) ApplyStored(*entry);
  }

  void MergeIntoStored(const SetEntry& entry) {
    const auto& list = entry.set->achievements;
    for (size_t i = 0; i < list.size(); ++i) {
      const Achievement& a = list[i];
      const Progress& p = entry.progress[i];
      if (detail::HasProgress(p)) {
        const json* existing = StoredEntry(a);
        stored[a.key] = detail::ProgressToJson(p, existing ? *existing : json::object());
      } else {
        stored.erase(a.key);
      }
      // Progress now lives under the key; a stale legacy entry would bring
      // back reset progress on the next load.
      for (const auto& legacy : a.legacy_keys) {
        if (!IsLoadedKey(legacy)) stored.erase(legacy);
      }
    }
  }

  void Save() {
    for (const auto& entry : sets) MergeIntoStored(*entry);
    dirty = false;
    if (dir.empty()) {
      memory_saves[profile] = stored;
      return;
    }
    WriteJsonAtomic(SavePath(), json{{"profile", profile}, {"achievements", stored}});
  }

  void SwitchProfile(std::string next) {
    if (dirty) Save();
    ++generation;
    profile = std::move(next);
    LoadStored();
    ApplyStoredAll();
    dirty = false;
  }

  // Called before anything that saves, so progress goes to the right profile.
  void SyncProfile() {
    if (!provider) return;
    std::string current = QueryProfile();
    if (current != profile) SwitchProfile(std::move(current));
  }

  bool MarkUnlocked(Loc loc, Outbox& out) {
    Progress& p = loc.progress();
    if (p.unlocked) return false;
    p.unlocked = true;
    p.unlock_time = clock ? clock() : Now();
    QueueUnlock(loc, false, out);
    return true;
  }

  // ---- Trigger evaluation (lock held) ------------------------------------
  // With out == nullptr nothing changes; the return value says whether
  // anything would, so callers can sync the profile only when it matters.

  // trigger.params.progress_popups: true (default) pops one per step, false
  // none, a number N every Nth step.
  static bool WantsProgressPopup(const Trigger& trigger, uint32_t current) {
    const auto it = trigger.params.find("progress_popups");
    if (it == trigger.params.end()) return true;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number_unsigned() || it->is_number_integer()) {
      const int64_t every = it->get<int64_t>();
      return every > 0 && current % static_cast<uint64_t>(every) == 0;
    }
    return true;
  }

  bool AddItems(Loc loc, std::vector<std::string> items, Outbox* out) {
    if (items.empty()) return false;
    if (!out) return true;
    const Achievement& a = loc.achievement();
    Progress& p = loc.progress();
    const uint32_t needed = NeededCount(a);
    const bool occurrences = a.checklist.empty() && a.trigger.type == "count" && !IsDistinct(a.trigger);
    for (auto& item : items) {
      if (occurrences) {
        ++p.count;
      } else if (IsDone(p, item)) {
        continue;  // Two spellings of one item matched the same report.
      } else {
        p.completed.push_back(item);
      }
      const uint32_t current = CurrentCount(a, p);
      if ((needed == 0 || current < needed) && WantsProgressPopup(a.trigger, current)) {
        QueueProgress(loc, std::move(item), *out);
      }
    }
    if (needed > 0 && CurrentCount(a, p) >= needed) MarkUnlocked(loc, *out);
    return true;
  }

  // Items a report or CompleteItem() would add (for a non-distinct count, the
  // one occurrence being reported).
  std::vector<std::string> NewItems(Loc loc, std::string_view reported) const {
    const Achievement& a = loc.achievement();
    const Progress& p = loc.progress();
    std::vector<std::string> items;
    if (!a.checklist.empty()) {
      for (const auto& entry : a.checklist) {
        if (Matches(reported, entry) && !IsDone(p, entry)) items.push_back(entry);
      }
    } else if (a.trigger.type == "count" && !IsDistinct(a.trigger)) {
      items.emplace_back(reported);
    } else if (!reported.empty() && !IsDone(p, reported)) {
      items.emplace_back(reported);
    }
    return items;
  }

  bool ApplyEvent(Loc loc, std::string_view target, Outbox* out) {
    const Achievement& a = loc.achievement();
    const Trigger& t = a.trigger;
    if (t.type == "event") {
      if (!TargetMatches(target, t.target)) return false;
      if (out) MarkUnlocked(loc, *out);
      return true;
    }
    if (t.type == "checklist") return AddItems(loc, NewItems(loc, target), out);
    if (t.type == "count") {
      if (!TargetMatches(target, t.target)) return false;
      return AddItems(loc, NewItems(loc, target), out);
    }
    return false;
  }

  // Locked achievements of the sets that are on.
  template <typename Fn>
  void ForEachLocked(Fn&& fn) const {
    for (const auto& entry : sets) {
      if (!entry->enabled) continue;
      for (size_t i = 0; i < entry->set->achievements.size(); ++i) {
        if (!entry->progress[i].unlocked) fn(Loc{entry.get(), i});
      }
    }
  }

  // ---- Listeners (lock NOT held) ------------------------------------------

  void Dispatch(Outbox& out) {
    if (out.events.empty()) return;
    std::vector<UnlockListener> on_unlock;
    std::vector<ProgressListener> on_progress;
    {
      std::lock_guard lock(mutex);
      for (const auto& [id, fn] : unlock_listeners) on_unlock.push_back(fn);
      for (const auto& [id, fn] : progress_listeners) on_progress.push_back(fn);
    }
    for (const auto& event : out.events) {
      if (const auto* unlock = std::get_if<UnlockEvent>(&event)) {
        for (const auto& fn : on_unlock) {
          try {
            fn(*unlock);
          } catch (...) {
          }
        }
      } else {
        const auto& progress = std::get<ProgressEvent>(event);
        for (const auto& fn : on_progress) {
          try {
            fn(progress);
          } catch (...) {
          }
        }
      }
    }
  }
};

System::System() : impl_(std::make_unique<Impl>()) {}

System::~System() {
  std::lock_guard lock(impl_->mutex);
  if (impl_->dirty) impl_->Save();
}

// ---- Definitions -----------------------------------------------------------

void System::AddSet(AchievementSet set) {
  if (set.id.empty()) set.id = NormalizeName(set.title);
  if (set.id.empty()) set.id = "default";
  if (set.title.empty()) set.title = set.id;

  // A repeated id replaces the earlier entry in its place, as old packs did.
  std::vector<Achievement> unique;
  std::unordered_map<std::string, size_t> position;
  const std::string prefix = set.id + ":";
  for (size_t i = 0; i < set.achievements.size(); ++i) {
    Achievement& a = set.achievements[i];
    if (a.local_id.empty() && !a.key.empty()) {
      a.local_id = a.key.rfind(prefix, 0) == 0 ? a.key.substr(prefix.size()) : a.key;
    }
    if (a.local_id.empty()) {
      const bool has_id = a.platform_id != 0 || set.platform == Platform::kPlayStation3;
      a.local_id = std::to_string(has_id ? a.platform_id : static_cast<uint32_t>(i));
    }
    a.key = prefix + a.local_id;
    a.set_id = set.id;
    if (a.category.empty()) a.category = set.category;
    if (const auto it = position.find(a.local_id); it != position.end()) {
      unique[it->second] = std::move(a);
    } else {
      position.emplace(a.local_id, unique.size());
      unique.push_back(std::move(a));
    }
  }
  set.achievements = std::move(unique);
  detail::ApplyPointsCap(set.achievements, set.points_cap);

  std::lock_guard lock(impl_->mutex);
  auto shared = std::make_shared<const AchievementSet>(std::move(set));
  SetEntry* entry = impl_->FindEntry(shared->id);
  if (entry) {
    impl_->MergeIntoStored(*entry);
    entry->set = std::move(shared);
  } else {
    impl_->sets.push_back(std::make_unique<SetEntry>());
    entry = impl_->sets.back().get();
    entry->set = std::move(shared);
  }
  entry->enabled = impl_->EnabledFor(*entry->set);
  impl_->RebuildIndex();
  impl_->ApplyStored(*entry);
  ++impl_->generation;
}

bool System::LoadSetFile(const fs::path& path, std::string* error) {
  AchievementSet set;
  if (!detail::LoadSetFile(path, set, error)) return false;
  AddSet(std::move(set));
  return true;
}

bool System::LoadSetJson(std::string_view json_text, const fs::path& origin, std::string* error) {
  AchievementSet set;
  if (!detail::LoadSetText(json_text, origin, set, error)) return false;
  AddSet(std::move(set));
  return true;
}

size_t System::LoadDirectory(const fs::path& dir, std::vector<std::string>* errors) {
  std::error_code ec;
  std::vector<fs::path> files, folders;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const auto u8 = entry.path().filename().u8string();
    const std::string name(u8.begin(), u8.end());
    if (entry.is_directory(ec)) {
      if (name.ends_with(".disabled") || name.ends_with(".off") || name.ends_with(".bak") ||
          name.ends_with(".tmp")) {
        continue;
      }
      folders.push_back(entry.path());
    } else if (name.ends_with(".achievements.json")) {
      files.push_back(entry.path());
    }
  }
  // Directory order differs between file systems; keep loading deterministic.
  std::sort(files.begin(), files.end());
  std::sort(folders.begin(), folders.end());

  size_t loaded = 0;
  for (const auto& file : files) {
    std::string error;
    if (LoadSetFile(file, &error)) {
      ++loaded;
    } else if (errors && !error.empty()) {
      errors->push_back(std::move(error));
    }
  }
  for (const auto& folder : folders) {
    for (auto& set : detail::LoadPackFolder(folder, errors)) {
      AddSet(std::move(set));
      ++loaded;
    }
  }
  return loaded;
}

void System::RemoveSet(std::string_view set_id) {
  std::lock_guard lock(impl_->mutex);
  auto& sets = impl_->sets;
  const auto it = std::find_if(sets.begin(), sets.end(),
                               [&](const auto& entry) { return entry->set->id == set_id; });
  if (it == sets.end()) return;
  impl_->MergeIntoStored(**it);
  sets.erase(it);
  impl_->RebuildIndex();
  ++impl_->generation;
}

void System::ClearSets() {
  std::lock_guard lock(impl_->mutex);
  for (const auto& entry : impl_->sets) impl_->MergeIntoStored(*entry);
  impl_->sets.clear();
  impl_->by_key.clear();
  ++impl_->generation;
}

const AchievementSet* System::FindSet(std::string_view set_id) const {
  std::lock_guard lock(impl_->mutex);
  const SetEntry* entry = impl_->FindEntry(set_id);
  return entry ? entry->set.get() : nullptr;
}

const Achievement* System::Find(std::string_view key) const {
  std::lock_guard lock(impl_->mutex);
  const auto loc = impl_->Lookup(key);
  return loc ? &loc->achievement() : nullptr;
}

const Achievement* System::FindByPlatformId(std::string_view set_id, uint32_t platform_id) const {
  std::lock_guard lock(impl_->mutex);
  const auto loc = impl_->LookupPlatformId(set_id, platform_id);
  return loc ? &loc->achievement() : nullptr;
}

std::vector<const AchievementSet*> System::sets() const {
  std::lock_guard lock(impl_->mutex);
  std::vector<const AchievementSet*> out;
  for (const auto& entry : impl_->sets) out.push_back(entry->set.get());
  return out;
}

std::vector<const AchievementSet*> System::enabled_sets() const {
  std::lock_guard lock(impl_->mutex);
  std::vector<const AchievementSet*> out;
  for (const auto& entry : impl_->sets)
    if (entry->enabled) out.push_back(entry->set.get());
  return out;
}

// ---- Turning sets on and off and settings -------------------------------------

void System::SetSetEnabled(std::string_view set_id, bool enabled) {
  std::lock_guard lock(impl_->mutex);
  SetEntry* entry = impl_->FindEntry(set_id);
  const bool same = impl_->Choice(set_id) == enabled && (!entry || entry->enabled == enabled);
  if (same) return;
  json& sets = impl_->settings["sets"];
  if (!sets.is_object()) sets = json::object();
  json& choice = sets[std::string(set_id)];
  if (!choice.is_object()) choice = json::object();
  choice["enabled"] = enabled;
  if (entry) entry->enabled = enabled;
  impl_->SaveSettings();
  ++impl_->generation;
}

bool System::IsSetEnabled(std::string_view set_id) const {
  std::lock_guard lock(impl_->mutex);
  if (const SetEntry* entry = impl_->FindEntry(set_id)) return entry->enabled;
  return impl_->Choice(set_id).value_or(true);
}

json System::GetSetting(std::string_view key) const {
  std::lock_guard lock(impl_->mutex);
  const auto it = impl_->settings.find("settings");
  if (it == impl_->settings.end() || !it->is_object()) return nullptr;
  const auto value = it->find(std::string(key));
  return value == it->end() ? json(nullptr) : *value;
}

void System::SetSetting(std::string_view key, json value) {
  std::lock_guard lock(impl_->mutex);
  json& all = impl_->settings["settings"];
  if (!all.is_object()) all = json::object();
  const std::string k(key);
  const auto it = all.find(k);
  if (value.is_null()) {
    if (it == all.end()) return;
    all.erase(it);
  } else {
    if (it != all.end() && *it == value) return;
    all[k] = std::move(value);
  }
  impl_->SaveSettings();
  ++impl_->generation;
}

void System::SetSettingsPath(fs::path path) {
  std::lock_guard lock(impl_->mutex);
  impl_->settings_override = std::move(path);
  impl_->LoadSettings();
  ++impl_->generation;
}

fs::path System::settings_path() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->SettingsPath();
}

// ---- Saves -------------------------------------------------------------------

void System::SetStorage(fs::path dir, ProfileProvider profile) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->dirty) impl_->Save();
  impl_->dir = std::move(dir);
  impl_->provider = std::move(profile);
  impl_->profile = impl_->provider ? impl_->QueryProfile() : std::string("default");
  impl_->LoadStored();
  impl_->ApplyStoredAll();
  if (impl_->settings_override.empty()) impl_->LoadSettings();
  impl_->dirty = false;
  ++impl_->generation;
}

void System::ReloadProgress() {
  std::lock_guard lock(impl_->mutex);
  std::string current = impl_->QueryProfile();
  // Unsaved best values belong to the profile being left; on a plain reload
  // the file wins.
  if (current != impl_->profile && impl_->dirty) impl_->Save();
  impl_->profile = std::move(current);
  impl_->LoadStored();
  impl_->ApplyStoredAll();
  impl_->dirty = false;
  ++impl_->generation;
}

void System::LoadProgress(const std::map<std::string, Progress>& progress, std::string profile) {
  std::lock_guard lock(impl_->mutex);
  impl_->profile = profile.empty() ? std::string("default") : std::move(profile);
  impl_->stored = json::object();
  for (const auto& [key, p] : progress)
    if (detail::HasProgress(p)) impl_->stored[key] = detail::ProgressToJson(p);
  impl_->ApplyStoredAll();
  impl_->dirty = false;
  ++impl_->generation;
}

void System::SetClock(std::function<uint64_t()> clock) {
  std::lock_guard lock(impl_->mutex);
  impl_->clock = std::move(clock);
}

std::string System::active_profile() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->profile;
}

uint64_t System::generation() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->generation;
}

// ---- Unlocking -----------------------------------------------------------------

bool System::Unlock(std::string_view key) {
  Outbox out;
  bool unlocked = false;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->SyncProfile();
    if (const auto loc = impl_->Lookup(key); loc && loc->entry->enabled) {
      unlocked = impl_->MarkUnlocked(*loc, out);
      if (unlocked) impl_->Save();
    }
  }
  impl_->Dispatch(out);
  return unlocked;
}

bool System::Unlock(std::string_view set_id, uint32_t platform_id) {
  std::string key;
  {
    std::lock_guard lock(impl_->mutex);
    const auto loc = impl_->LookupPlatformId(set_id, platform_id);
    if (!loc) return false;
    key = loc->achievement().key;
  }
  return Unlock(key);
}

bool System::IsUnlocked(std::string_view key) const {
  std::lock_guard lock(impl_->mutex);
  const auto loc = impl_->Lookup(key);
  return loc && loc->progress().unlocked;
}

bool System::IsUnlocked(std::string_view set_id, uint32_t platform_id) const {
  std::lock_guard lock(impl_->mutex);
  const auto loc = impl_->LookupPlatformId(set_id, platform_id);
  return loc && loc->progress().unlocked;
}

std::optional<Progress> System::GetProgress(std::string_view key) const {
  std::lock_guard lock(impl_->mutex);
  const auto loc = impl_->Lookup(key);
  if (!loc) return std::nullopt;
  return loc->progress();
}

void System::Reset() {
  std::lock_guard lock(impl_->mutex);
  for (auto& entry : impl_->sets) {
    std::fill(entry->progress.begin(), entry->progress.end(), Progress{});
  }
  // Like the old reset, this clears the whole profile, unloaded sets included.
  impl_->stored = json::object();
  impl_->bar_totals.clear();
  impl_->Save();
  ++impl_->generation;
}

void System::Reset(const std::vector<std::string>& keys) {
  if (keys.empty()) return;
  std::lock_guard lock(impl_->mutex);
  for (const auto& key : keys) {
    if (const auto loc = impl_->Lookup(key)) {
      loc->progress() = Progress{};
      impl_->bar_totals.erase(loc->achievement().key);
    } else {
      impl_->stored.erase(key);
    }
  }
  impl_->Save();
  ++impl_->generation;
}

void System::ResetSet(std::string_view set_id) {
  std::lock_guard lock(impl_->mutex);
  if (SetEntry* entry = impl_->FindEntry(set_id)) {
    std::fill(entry->progress.begin(), entry->progress.end(), Progress{});
    for (const auto& a : entry->set->achievements) impl_->bar_totals.erase(a.key);
  } else {
    // Not loaded: drop its saved entries by their key prefix.
    const std::string prefix = std::string(set_id) + ":";
    for (auto it = impl_->stored.begin(); it != impl_->stored.end();) {
      if (it.key().rfind(prefix, 0) == 0) it = impl_->stored.erase(it);
      else ++it;
    }
  }
  impl_->Save();
  ++impl_->generation;
}

// ---- Built-in triggers -----------------------------------------------------------

void System::ReportStat(std::string_view stat, double value) {
  if (std::isnan(value)) return;
  Outbox out;
  {
    std::lock_guard lock(impl_->mutex);
    const auto names = impl_->StatNames(stat);
    const auto matches = [&](Loc loc) {
      const Trigger& t = loc.achievement().trigger;
      return t.type == "stat" && names.contains(NormalizeName(t.stat));
    };
    bool will_unlock = false;
    impl_->ForEachLocked([&](Loc loc) {
      if (matches(loc) && value >= loc.achievement().trigger.threshold) will_unlock = true;
    });
    if (will_unlock) impl_->SyncProfile();

    bool unlocked = false;
    impl_->ForEachLocked([&](Loc loc) {
      if (!matches(loc)) return;
      Progress& p = loc.progress();
      if (value > p.best_value) {
        p.best_value = value;
        impl_->dirty = true;
      }
      if (value >= loc.achievement().trigger.threshold) unlocked |= impl_->MarkUnlocked(loc, out);
    });
    if (unlocked) impl_->Save();
  }
  impl_->Dispatch(out);
}

void System::ReportEvent(std::string_view event, std::string_view target) {
  Outbox out;
  {
    std::lock_guard lock(impl_->mutex);
    const auto candidates = [&] {
      std::vector<Loc> found;
      impl_->ForEachLocked([&](Loc loc) {
        const Trigger& t = loc.achievement().trigger;
        if (IsBuiltinEventType(t.type) && Impl::SameName(t.event, event)) found.push_back(loc);
      });
      return found;
    };
    const auto before = candidates();
    const bool will_change = std::any_of(before.begin(), before.end(),
                                         [&](Loc loc) { return impl_->ApplyEvent(loc, target, nullptr); });
    if (will_change) {
      // Another profile may have different achievements locked.
      impl_->SyncProfile();
      bool changed = false;
      for (const Loc loc : candidates()) changed |= impl_->ApplyEvent(loc, target, &out);
      if (changed) impl_->Save();
    }
  }
  impl_->Dispatch(out);
}

void System::AddStatAlias(std::string alias, std::string stat) {
  std::lock_guard lock(impl_->mutex);
  const std::string a = NormalizeName(alias);
  const std::string b = NormalizeName(stat);
  if (a == b) return;
  impl_->aliases[a].insert(b);
  impl_->aliases[b].insert(a);
}

void System::SetNameMatcher(NameMatcher matcher) {
  std::lock_guard lock(impl_->mutex);
  impl_->matcher = std::move(matcher);
}

bool System::CompleteItem(std::string_view key, std::string_view item) {
  Outbox out;
  bool added = false;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->SyncProfile();
    if (const auto loc = impl_->Lookup(key); loc && loc->entry->enabled && !loc->progress().unlocked) {
      added = impl_->AddItems(*loc, impl_->NewItems(*loc, item), &out);
      if (added) impl_->Save();
    }
  }
  impl_->Dispatch(out);
  return added;
}

void System::RecordValue(std::string_view key, double value) {
  std::lock_guard lock(impl_->mutex);
  const auto loc = impl_->Lookup(key);
  if (!loc || !loc->entry->enabled || loc->progress().unlocked || !(value > loc->progress().best_value)) return;
  loc->progress().best_value = value;
  impl_->dirty = true;
}

void System::RecordValue(std::string_view key, double value, double total) {
  {
    std::lock_guard lock(impl_->mutex);
    const auto loc = impl_->Lookup(key);
    if (!loc) return;
    const std::string& real_key = loc->achievement().key;
    if (total > 0.0) {
      impl_->bar_totals[real_key] = static_cast<uint32_t>(std::min(std::round(total), 4294967295.0));
    } else {
      impl_->bar_totals.erase(real_key);
    }
  }
  RecordValue(key, value);
}

void System::ForEachPending(std::string_view type,
                            const std::function<void(const Achievement&, const Progress&)>& fn) const {
  std::vector<std::pair<Achievement, Progress>> pending;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->ForEachLocked([&](Loc loc) {
      if (loc.achievement().trigger.type == type) pending.emplace_back(loc.achievement(), loc.progress());
    });
  }
  for (const auto& [achievement, progress] : pending) fn(achievement, progress);
}

bool System::HasPending(std::string_view type) const {
  std::lock_guard lock(impl_->mutex);
  bool found = false;
  impl_->ForEachLocked([&](Loc loc) { found |= loc.achievement().trigger.type == type; });
  return found;
}

// ---- UI ----------------------------------------------------------------------------

std::vector<AchievementView> System::Snapshot(bool include_disabled) const {
  std::lock_guard lock(impl_->mutex);
  std::vector<AchievementView> out;
  for (const auto& entry : impl_->sets) {
    if (!entry->enabled && !include_disabled) continue;
    for (const size_t i : Impl::DisplayOrder(*entry->set)) out.push_back(impl_->View(*entry, i));
  }
  return out;
}

System::Totals System::GetTotals(std::string_view set_id) const {
  std::lock_guard lock(impl_->mutex);
  Totals totals;
  for (const auto& entry : impl_->sets) {
    if (set_id.empty() ? !entry->enabled : entry->set->id != set_id) continue;
    for (size_t i = 0; i < entry->set->achievements.size(); ++i) {
      const uint32_t points = entry->set->achievements[i].points;
      ++totals.count;
      totals.points += points;
      if (entry->progress[i].unlocked) {
        ++totals.unlocked;
        totals.unlocked_points += points;
      }
    }
  }
  return totals;
}

System::State System::CopyState() const {
  std::lock_guard lock(impl_->mutex);
  State state;
  state.profile = impl_->profile;
  state.generation = impl_->generation;
  state.sets.reserve(impl_->sets.size());
  for (const auto& entry : impl_->sets) {
    SetState s;
    s.set = entry->set;
    s.enabled = entry->enabled;
    s.views.reserve(entry->set->achievements.size());
    for (size_t i = 0; i < entry->set->achievements.size(); ++i) s.views.push_back(impl_->View(*entry, i));
    state.sets.push_back(std::move(s));
  }
  return state;
}

void System::TestPopup(std::string_view key) {
  Outbox out;
  {
    std::lock_guard lock(impl_->mutex);
    std::optional<Loc> loc;
    if (!key.empty()) {
      loc = impl_->Lookup(key);
      // A set that's off shows no popups, not even test ones.
      if (loc && !loc->entry->enabled) loc.reset();
    } else {
      // Same order as Snapshot(): first locked, else the very first.
      std::optional<Loc> first;
      for (const auto& entry : impl_->sets) {
        if (!entry->enabled) continue;
        for (const size_t i : Impl::DisplayOrder(*entry->set)) {
          if (!first) first = Loc{entry.get(), i};
          if (!entry->progress[i].unlocked) {
            loc = Loc{entry.get(), i};
            break;
          }
        }
        if (loc) break;
      }
      if (!loc) loc = first;
    }
    if (loc) impl_->QueueUnlock(*loc, true, out);
  }
  impl_->Dispatch(out);
}

int System::AddUnlockListener(UnlockListener listener) {
  std::lock_guard lock(impl_->mutex);
  const int id = impl_->next_listener_id++;
  impl_->unlock_listeners.emplace_back(id, std::move(listener));
  return id;
}

int System::AddProgressListener(ProgressListener listener) {
  std::lock_guard lock(impl_->mutex);
  const int id = impl_->next_listener_id++;
  impl_->progress_listeners.emplace_back(id, std::move(listener));
  return id;
}

void System::RemoveListener(int id) {
  std::lock_guard lock(impl_->mutex);
  std::erase_if(impl_->unlock_listeners, [id](const auto& l) { return l.first == id; });
  std::erase_if(impl_->progress_listeners, [id](const auto& l) { return l.first == id; });
}

AchievementSet SplitSet(AchievementSet& from, std::span<const uint32_t> platform_ids, std::string new_id,
                        std::string title, bool enabled_by_default) {
  const std::string from_id = EffectiveSetId(from);
  AchievementSet out;
  out.id = std::move(new_id);
  out.title = title.empty() ? out.id : std::move(title);
  out.platform = from.platform;
  out.category = from.category;
  out.credits = from.credits;
  out.source = from.source;
  out.base_dir = from.base_dir;
  out.enabled_by_default = enabled_by_default;
  // Pin every local id first: ids taken from the position would shift once
  // achievements are moved out.
  for (size_t i = 0; i < from.achievements.size(); ++i) {
    from.achievements[i].local_id = EffectiveLocalId(from, from_id, i);
    from.achievements[i].key.clear();
  }
  std::vector<Achievement> kept;
  for (auto& a : from.achievements) {
    const bool move = std::find(platform_ids.begin(), platform_ids.end(), a.platform_id) != platform_ids.end();
    if (!move) {
      kept.push_back(std::move(a));
      continue;
    }
    const std::string old_key = from_id + ":" + a.local_id;
    if (std::find(a.legacy_keys.begin(), a.legacy_keys.end(), old_key) == a.legacy_keys.end())
      a.legacy_keys.insert(a.legacy_keys.begin(), old_key);
    a.set_id.clear();
    out.achievements.push_back(std::move(a));
  }
  from.achievements = std::move(kept);
  return out;
}

}  // namespace recomp::achievements
