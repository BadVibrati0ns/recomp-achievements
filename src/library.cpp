#include "recomp/achievements/library.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include "json_format.h"
#include "library_util.h"
#include "recomp/achievements/config_dir.h"
#include "recomp/achievements/library_reader.h"

namespace recomp::achievements {

namespace fs = std::filesystem;
using nlohmann::json;
using ojson = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;

namespace {

// How long an icon that couldn't be fetched is left alone (a badge may still
// be downloading).
constexpr auto kIconRetry = std::chrono::seconds(30);

std::span<const uint8_t> Bytes(const std::string& s) {
  return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

json TotalsJson(const LibraryTotals& t) {
  return {{"count", t.count}, {"unlocked", t.unlocked}, {"points", t.points}, {"unlocked_points", t.unlocked_points}};
}

// State shared with the System listeners. Held by shared_ptr: a listener
// copy can still be running inside System's dispatch after the Library has
// removed it.
struct Shared {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<LibraryEvent> queue;  // Profile is the game's raw name here.
  bool dirty = false;               // Progress changed; a snapshot is due.
  bool wake = false;
  bool stop = false;
};

}  // namespace

fs::path DefaultLibraryRoot(bool wine_host) {
  if (std::string dir = GetEnv("RECOMP_ACHIEVEMENTS_CONFIG_DIR"); !dir.empty()) {
    if (RunningUnderWine()) dir = UnixToWinePath(dir);
    return fs::path(dir) / "library";
  }
  if (wine_host) {
    if (const fs::path host = WineHostConfigDir(); !host.empty()) return host / "library";
  }
  const fs::path global = GlobalConfigDir();
  return global.empty() ? fs::path() : global / "library";
}

struct Library::Impl {
  System& system;
  LibraryOptions options;
  bool active = false;
  bool wine = false;
  bool hash_profiles = false;
  fs::path root, dir;
  fs::path pointer_dir;  // <root>/<game id>: the entry (kCopy) or its location.json (kIndex).
  LibraryMode mode = LibraryMode::kCopy;
  std::string game_id, install_id;

  uint64_t NowMs() const { return options.clock ? options.clock() : detail::UnixMillis(); }
  uint64_t UnixSeconds() const { return NowMs() / 1000; }

  std::shared_ptr<Shared> shared = std::make_shared<Shared>();
  std::thread worker;
  int unlock_listener = 0, progress_listener = 0;

  mutable std::mutex stats_mutex;
  Stats stats;

  // ---- Worker state (io_mutex held) -----------------------------------------
  std::mutex io_mutex;
  bool observed = false;
  uint64_t last_generation = 0;
  std::map<std::string, bool> set_enabled;                       // Last seen, by set id.
  std::map<std::string, std::map<std::string, bool>> had_progress;  // Raw profile -> key.
  struct Exported {
    std::shared_ptr<const AchievementSet> set;
    bool enabled = true;
    bool missing_icons = false;
    std::string hash;
  };
  std::map<std::string, Exported> exported;
  struct IconEntry {
    std::string stamp, file;
  };
  std::map<std::string, IconEntry> icon_index;
  bool icon_index_dirty = false;
  std::set<std::string> icon_files_seen;
  std::map<std::string, Clock::time_point> icon_missing;
  bool icons_missing = false;  // Some icon couldn't be fetched last time.
  Clock::time_point icon_retry_at{};
  bool forcing = false;        // Flush(): retry missing icons now.
  std::string game_icon;  // File in icons/, or empty.
  std::vector<std::string> set_ids;
  std::vector<std::string> platforms;
  std::string first_title;
  std::string last_game_json;
  bool snapshot_dirty = false;
  Clock::time_point last_snapshot{};
  bool launch_recorded = false;
  json launch;  // game.json's "launch", null when not recorded.
  std::optional<RunningMarker> marker;  // Ours, written to running.json.

  Impl(System& sys, LibraryOptions opt) : system(sys), options(std::move(opt)) {}

  void Count(uint64_t Stats::*field, uint64_t n = 1) {
    std::lock_guard lock(stats_mutex);
    stats.*field += n;
  }

  template <typename Json>
  bool Write(const fs::path& path, const Json& doc) {
    const bool ok = detail::WriteJsonAtomic(path, doc);
    if (!ok) Count(&Stats::failures);
    return ok;
  }

  // ---- Setup (constructor thread) ----------------------------------------

  void Init() {
    game_id = detail::SafeName(options.game_id);
    wine = RunningUnderWine();
    root = options.root.empty() ? DefaultLibraryRoot(options.wine_host) : options.root;
    if (root.empty()) return;
    pointer_dir = root / game_id;
    dir = pointer_dir;
    if (options.mode == LibraryMode::kIndex) {
      fs::path data = options.data_dir;
      if (data.empty())
        if (const fs::path storage = system.storage_dir(); !storage.empty()) data = storage / "recomp-achievements";
      if (!data.empty()) {
        mode = LibraryMode::kIndex;
        std::error_code ec;
        dir = fs::absolute(data, ec);
        if (ec) dir = data;
      }
    }
    hash_profiles = options.hash_profiles;
    LoadInstall();
    LoadIconIndex();
    active = true;
  }

  // install.json: one random id per library folder, made once. Two games
  // starting together may both write one; both then use the file's.
  void LoadInstall() {
    const fs::path path = root / "install.json";
    json doc = detail::ReadJsonFile(path);
    const auto id_of = [](const json& d) {
      const auto it = d.is_object() ? d.find("install_id") : d.end();
      return it != d.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    install_id = id_of(doc);
    if (install_id.empty()) {
      ojson fresh = {{"schema", detail::kInstallSchema}, {"install_id", detail::NewUuid()},
                     {"created", UnixSeconds()}};
      if (doc.is_object())
        for (const auto& [k, v] : doc.items())
          if (!fresh.contains(k)) fresh[k] = ojson::parse(v.dump());
      Write(path, fresh);
      doc = detail::ReadJsonFile(path);
      install_id = id_of(doc);
      // Unwritable: keep a per-session id so events still identify their source.
      if (install_id.empty()) install_id = fresh["install_id"].get<std::string>();
    }
    if (doc.is_object()) {
      if (const auto it = doc.find("hash_profiles"); it != doc.end() && it->is_boolean() && it->get<bool>())
        hash_profiles = true;
    }
  }

  void LoadIconIndex() {
    const json doc = detail::ReadJsonFile(dir / "icons" / "index.json");
    if (!doc.is_object()) return;
    const auto entries = doc.find("icons");
    if (entries == doc.end() || !entries->is_object()) return;
    for (const auto& [key, e] : entries->items()) {
      if (!e.is_object()) continue;
      const std::string file = e.value("file", "");
      if (file.empty() || file.find_first_of("/\\") != std::string::npos) continue;
      icon_index[key] = {e.value("stamp", ""), file};
    }
  }

  // ---- Profiles --------------------------------------------------------------

  std::string ProfileId(std::string_view profile) const {
    const std::string raw = profile.empty() ? std::string("default") : std::string(profile);
    // Salted with the install id: the same name hashes differently on
    // another machine, so ids can't be matched against a list of names.
    if (hash_profiles) return "h-" + detail::Sha256Hex(install_id + "\n" + raw).substr(0, 16);
    return raw;
  }

  // A file name that stays unique when the id had characters replaced.
  static std::string ProfileFile(const std::string& id) {
    std::string name = detail::SafeName(id);
    if (name != id) name += "-" + detail::Sha256Hex(id).substr(0, 8);
    return name + ".json";
  }

  // ---- Listeners (game thread) -------------------------------------------------

  void Install() {
    auto queue = [shared = shared, sys = &system, clock = options.clock](LibraryEvent e) {
      e.t = clock ? clock() : detail::UnixMillis();
      e.profile = sys->active_profile();
      {
        std::lock_guard lock(shared->mutex);
        if (shared->stop) return;
        shared->queue.push_back(std::move(e));
        shared->dirty = true;
        shared->wake = true;
      }
      shared->cv.notify_one();
    };
    unlock_listener = system.AddUnlockListener([queue](const UnlockEvent& u) {
      if (u.replay || !u.view.achievement) return;
      LibraryEvent e;
      e.kind = "unlock";
      e.key = u.view.achievement->key;
      queue(std::move(e));
    });
    progress_listener = system.AddProgressListener([queue](const ProgressEvent& p) {
      if (!p.view.achievement) return;
      LibraryEvent e;
      e.kind = "progress";
      e.key = p.view.achievement->key;
      e.value = p.view.progress_current;
      e.item = p.item;
      queue(std::move(e));
    });
  }

  void Start() {
    Install();
    worker = std::thread([this] { Run(); });
  }

  void Stop() {
    if (!active) return;
    system.RemoveListener(unlock_listener);
    system.RemoveListener(progress_listener);
    {
      std::lock_guard lock(shared->mutex);
      shared->stop = true;
    }
    shared->cv.notify_all();
    if (worker.joinable()) worker.join();
  }

  void Run() {
    // The first pass right away, so a short session still records itself.
    Work(false);
    for (;;) {
      {
        std::unique_lock lock(shared->mutex);
        shared->cv.wait_for(lock, options.poll_interval, [&] { return shared->stop || shared->wake; });
        if (shared->stop) return;
        shared->wake = false;
      }
      Work(false);
    }
  }

  // ---- The work (io_mutex held) ------------------------------------------------

  void Work(bool force) {
    std::lock_guard lock(io_mutex);
    try {
      WorkLocked(force);
    } catch (...) {
      Count(&Stats::failures);
    }
  }

  // How this process was started, and the running.json marker, once.
  void RecordLaunch() {
    launch_recorded = true;
    if (options.record_launch) {
      LaunchInfo info = CurrentLaunchInfo();
      if (!options.launch.exe.empty()) {
        info.exe = options.launch.exe;
        info.args = options.launch.args;
        if (!options.launch.cwd.empty()) info.cwd = options.launch.cwd;
      }
      if (!info.empty()) launch = LaunchToJson(info);
    }
    if (options.running_marker) {
      marker = CurrentRunningMarker();
      if (marker && !Write(dir / "running.json", MarkerToJson(*marker))) marker.reset();
    }
  }

  // Removes running.json if it is still ours (a second copy may have
  // written its own since).
  void ClearMarker() {
    if (!marker) return;
    const auto on_disk = MarkerFromJson(detail::ReadJsonFile(dir / "running.json"));
    std::error_code ec;
    if (on_disk && on_disk->pid == marker->pid && on_disk->start == marker->start) fs::remove(dir / "running.json", ec);
    marker.reset();
  }

  // kIndex: moves an entry an older version copied into the library folder
  // over to the game's folder (when that has none yet), then points the
  // library at it. kCopy: drops a pointer a kIndex run left.
  void PlaceEntry() {
    std::error_code ec;
    const fs::path location = pointer_dir / "location.json";
    if (mode == LibraryMode::kCopy) {
      fs::remove(location, ec);
      return;
    }
    if (fs::is_regular_file(pointer_dir / "game.json", ec) && !fs::exists(dir / "game.json", ec)) {
      fs::create_directories(dir, ec);
      for (const auto& e : fs::directory_iterator(pointer_dir, ec)) {
        const std::string name = e.path().filename().string();
        const bool ours = name == "game.json" || name == "sets" || name == "icons" || name == "progress" ||
                          (name.rfind("events", 0) == 0 && e.path().extension() == ".log");
        if (!ours) continue;
        std::error_code move_ec;
        fs::rename(e.path(), dir / name, move_ec);
        if (move_ec) {
          // Another drive: copy, then remove the old one only if that worked.
          std::error_code copy_ec;
          fs::copy(e.path(), dir / name, fs::copy_options::recursive | fs::copy_options::overwrite_existing, copy_ec);
          if (!copy_ec) fs::remove_all(e.path(), copy_ec);
        }
      }
    }
    const ojson doc = {{"schema", "recomp-achievements-location/1"},
                       {"game_id", game_id},
                       {"title", !options.title.empty() ? options.title : game_id},
                       {"dir", HostPath(dir)},
                       {"updated", UnixSeconds()}};
    fs::create_directories(pointer_dir, ec);
    Write(location, doc);
  }

  void WorkLocked(bool force) {
    if (!launch_recorded) {
      PlaceEntry();
      RecordLaunch();
    }
    std::vector<LibraryEvent> events;
    {
      std::lock_guard lock(shared->mutex);
      events.swap(shared->queue);
      if (shared->dirty) snapshot_dirty = true;
      shared->dirty = false;
    }
    // Progress the listeners saw counts for reset detection even if the
    // state below has already been reset again.
    for (const auto& e : events) had_progress[e.profile][e.key] = true;

    const bool due = force || Clock::now() - last_snapshot >= options.write_interval;
    const uint64_t generation = system.generation();
    std::optional<System::State> state;
    bool changed = !observed || generation != last_generation;
    // Providers are often registered after the Library starts, and badges
    // may still be downloading: look again now and then.
    const bool retry_icons = icons_missing && Clock::now() >= icon_retry_at;
    forcing = force;
    if (force || changed || retry_icons || (snapshot_dirty && due)) {
      state = system.CopyState();
      changed = !observed || state->generation != last_generation;
      Observe(*state, events);
      // Forced: also retries icons that were pending.
      if (changed || force || retry_icons) ExportSets(*state);
      if (changed) snapshot_dirty = true;
      last_generation = state->generation;
      observed = true;
    }

    AppendEvents(events);
    if (state && snapshot_dirty && due) {
      WriteSnapshot(*state);
      snapshot_dirty = false;
      last_snapshot = Clock::now();
    }
    if (icon_index_dirty) WriteIconIndex();
    if (state) WriteGameJson(force);
  }

  // Resets and set toggles have no listener; they show up as differences
  // from the last state seen.
  void Observe(const System::State& state, std::vector<LibraryEvent>& events) {
    const uint64_t now = NowMs();
    for (const auto& s : state.sets) {
      const std::string& id = s.set->id;
      const auto known = set_enabled.find(id);
      if (known != set_enabled.end() && known->second != s.enabled) {
        LibraryEvent e;
        e.t = now;
        e.kind = "set_enabled";
        e.key = id;
        e.value = s.enabled;
        events.push_back(std::move(e));
      }
      set_enabled[id] = s.enabled;
    }
    auto& before = had_progress[state.profile];
    for (const auto& s : state.sets) {
      for (const auto& v : s.views) {
        const std::string& key = v.achievement->key;
        const bool has = detail::HasProgress(v.progress);
        const auto it = before.find(key);
        if (it != before.end() && it->second && !has) {
          LibraryEvent e;
          e.t = now;
          e.profile = state.profile;
          e.kind = "reset";
          e.key = key;
          events.push_back(std::move(e));
        }
        before[key] = has;
      }
    }
  }

  void AppendEvents(std::vector<LibraryEvent>& events) {
    if (events.empty()) return;
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.t < b.t; });
    std::string text;
    for (auto& e : events) {
      e.install_id = install_id;
      e.game = game_id;
      // Set toggles are the game's, not a profile's.
      if (e.kind != "set_enabled") e.profile = ProfileId(e.profile);
      e.game_version = options.version;
      text += detail::EventLine(e);
      text += '\n';
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path log = dir / "events.log";
    // Rotation renames; nothing already written is ever rewritten.
    if (options.rotate_bytes > 0) {
      const auto size = fs::file_size(log, ec);
      if (!ec && size >= options.rotate_bytes) {
        const std::string stem = "events-" + std::to_string(NowMs());
        fs::path target = dir / (stem + ".log");
        for (int i = 1; fs::exists(target, ec); ++i) target = dir / (stem + "-" + std::to_string(i) + ".log");
        fs::rename(log, target, ec);
      }
    }
    // One write per batch in append mode, so lines from two processes don't interleave.
    std::FILE* f = nullptr;
#if defined(_WIN32)
    f = _wfopen(log.c_str(), L"ab");
#else
    f = std::fopen(log.c_str(), "ab");
#endif
    bool ok = false;
    if (f) {
      ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
      ok = std::fclose(f) == 0 && ok;
    }
    if (ok) Count(&Stats::events_written, events.size());
    else Count(&Stats::failures);
  }

  // ---- Sets and icons -------------------------------------------------------------

  void ExportSets(const System::State& state) {
    icons_missing = false;
    std::vector<std::string> ids;
    std::vector<std::string> plats;
    for (const auto& s : state.sets) {
      const AchievementSet& set = *s.set;
      ids.push_back(set.id);
      const std::string plat(detail::PlatformName(set.platform));
      if (std::find(plats.begin(), plats.end(), plat) == plats.end()) plats.push_back(plat);
      if (first_title.empty()) first_title = set.title;

      Exported& last = exported[set.id];
      if (last.set == s.set && last.enabled == s.enabled && !last.missing_icons) continue;
      AchievementSet copy = set;
      bool missing = false;
      for (auto& a : copy.achievements) {
        for (std::string* ref : {&a.icon, &a.locked_icon}) {
          if (ref->empty()) continue;
          const std::string file = ExportIcon(*ref, set.id, set.base_dir, missing);
          *ref = file.empty() ? std::string() : "../icons/" + file;
        }
      }
      json doc = detail::NativeSetToJson(copy);
      doc["_library"] = {{"enabled", s.enabled}};
      const std::string text = doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
      const std::string hash = detail::Sha256Hex(text);
      last.set = s.set;
      last.enabled = s.enabled;
      last.missing_icons = missing;
      icons_missing |= missing;
      if (hash == last.hash) continue;
      const fs::path path = dir / "sets" / (detail::SafeName(set.id) + ".achievements.json");
      // Already on disk from an earlier session: no write.
      if (last.hash.empty() && detail::Sha256Hex(ReadText(path)) == hash) {
        last.hash = hash;
        continue;
      }
      if (detail::WriteFileAtomic(path, Bytes(text))) {
        last.hash = hash;
        Count(&Stats::set_writes);
      } else {
        Count(&Stats::failures);
      }
    }
    // Keep the last known list while nothing is loaded yet.
    if (!ids.empty()) {
      set_ids = std::move(ids);
      platforms = std::move(plats);
    }

    std::string icon = options.icon;
    fs::path base = options.icon_base_dir;
    if (icon.empty() && options.icon_source) {
      // The title image the platform data carries.
      for (const auto& s : state.sets) {
        if (s.set->platform == Platform::kXbox360) icon = "xdbf:32768";
        else if (s.set->platform == Platform::kPlayStation3) icon = "trophy:ICON0.PNG";
        else continue;
        base = s.set->base_dir;
        break;
      }
    }
    if (!icon.empty()) {
      bool missing = false;
      const std::string file = ExportIcon(icon, {}, base, missing);
      if (!file.empty()) game_icon = file;
      icons_missing |= missing;
    }
    icon_retry_at = Clock::now() + kIconRetry;
  }

  static std::string ReadText(const fs::path& path) {
    std::FILE* f = nullptr;
#if defined(_WIN32)
    f = _wfopen(path.c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return {};
    std::string out;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
  }

  // Returns the file name in icons/ (content-addressed), or empty. Each ref
  // is fetched once per change: files by size and time, other refs by game
  // version.
  std::string ExportIcon(const std::string& ref, std::string_view set_id, const fs::path& base_dir,
                         bool& missing) {
    std::string scheme;
    std::string body = ref;
    if (const size_t colon = ref.find(':'); colon != std::string::npos && colon >= 2) {
      scheme = ref.substr(0, colon);
      body = ref.substr(colon + 1);
      const bool word = std::all_of(scheme.begin(), scheme.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
      });
      if (!word || scheme == "file") {
        scheme.clear();
        body = word ? ref.substr(colon + 1) : ref;
      }
    }

    std::error_code ec;
    std::string key, stamp;
    fs::path file;
    if (scheme.empty()) {
      file = fs::path(body);
      if (file.is_relative() && !base_dir.empty()) file = base_dir / file;
      file = file.lexically_normal();
      key = "file|" + file.generic_string();
      const auto size = fs::file_size(file, ec);
      const auto time = fs::last_write_time(file, ec);
      if (ec) {
        missing = true;
        return {};
      }
      stamp = std::to_string(size) + ":" + std::to_string(time.time_since_epoch().count());
    } else {
      key = std::string(set_id) + "|" + ref;
      stamp = "version:" + options.version;
    }
    // Hashed: the index must not record where the game's files are.
    key = detail::Sha256Hex(key).substr(0, 32);
    stamp = detail::Sha256Hex(stamp).substr(0, 16);

    if (const auto it = icon_index.find(key); it != icon_index.end() && it->second.stamp == stamp) {
      const std::string& name = it->second.file;
      if (icon_files_seen.contains(name) || fs::exists(dir / "icons" / name, ec)) {
        icon_files_seen.insert(name);
        return name;
      }
    }
    if (const auto it = icon_missing.find(key);
        !forcing && it != icon_missing.end() && Clock::now() - it->second < kIconRetry) {
      missing = true;
      return {};
    }

    LibraryIcon icon;
    bool ok = false;
    if (scheme.empty()) {
      const std::string text = ReadText(file);
      icon.png.assign(text.begin(), text.end());
      ok = detail::IsPng(icon.png);
      if (!ok) icon.png.clear();
    }
    if (!ok && options.icon_source) {
      try {
        ok = options.icon_source(ref, set_id, base_dir, icon);
      } catch (...) {
        ok = false;
      }
    }
    std::vector<uint8_t> png;
    if (ok && detail::IsPng(icon.png)) {
      png = std::move(icon.png);
    } else if (ok && icon.width > 0 && icon.height > 0) {
      png = EncodePng(icon.width, icon.height, icon.rgba);
    }
    if (png.empty()) {
      icon_missing[key] = Clock::now();
      missing = true;
      return {};
    }
    icon_missing.erase(key);

    const std::string name = detail::Sha256Hex(png).substr(0, 24) + ".png";
    const fs::path path = dir / "icons" / name;
    if (!icon_files_seen.contains(name) && !fs::exists(path, ec)) {
      if (!detail::WriteFileAtomic(path, png)) {
        Count(&Stats::failures);
        missing = true;
        return {};
      }
      Count(&Stats::icon_writes);
    }
    icon_files_seen.insert(name);
    icon_index[key] = {stamp, name};
    icon_index_dirty = true;
    return name;
  }

  void WriteIconIndex() {
    json entries = json::object();
    for (const auto& [key, e] : icon_index) entries[key] = {{"file", e.file}, {"stamp", e.stamp}};
    if (Write(dir / "icons" / "index.json", json{{"icons", std::move(entries)}})) icon_index_dirty = false;
  }

  // ---- Snapshots -------------------------------------------------------------------

  void WriteSnapshot(const System::State& state) {
    if (state.sets.empty()) return;  // Nothing loaded yet; don't blank the last one.
    const std::string id = ProfileId(state.profile);
    LibraryTotals totals;
    json sets = json::object();
    json achievements = json::object();
    for (const auto& s : state.sets) {
      LibraryTotals t;
      for (const auto& v : s.views) {
        const uint32_t points = v.achievement->points;
        ++t.count;
        t.points += points;
        if (v.progress.unlocked) {
          ++t.unlocked;
          t.unlocked_points += points;
        }
        if (!detail::HasProgress(v.progress)) continue;
        json entry = detail::ProgressToJson(v.progress);
        if (v.progress_total > 0) {
          entry["current"] = v.progress_current;
          entry["total"] = v.progress_total;
        }
        achievements[v.achievement->key] = std::move(entry);
      }
      if (s.enabled) {
        totals.count += t.count;
        totals.unlocked += t.unlocked;
        totals.points += t.points;
        totals.unlocked_points += t.unlocked_points;
      }
      json st = TotalsJson(t);
      st["enabled"] = s.enabled;
      sets[s.set->id] = std::move(st);
    }
    const ojson doc = {{"schema", detail::kProgressSchema}, {"game", game_id}, {"profile", id},
                       {"updated", UnixSeconds()}, {"totals", ojson::parse(TotalsJson(totals).dump())},
                       {"sets", ojson::parse(sets.dump())}, {"achievements", ojson::parse(achievements.dump())}};
    if (Write(dir / "progress" / ProfileFile(id), doc)) Count(&Stats::snapshot_writes);
  }

  void WriteGameJson(bool force) {
    ojson doc = {{"schema", kLibrarySchema},
                {"game_id", game_id},
                {"title", !options.title.empty() ? options.title : !first_title.empty() ? first_title : game_id},
                {"platforms", platforms},
                {"game_version", options.version},
                {"icon", game_icon.empty() ? ojson(nullptr) : ojson("icons/" + game_icon)},
                {"sdk_version", kSdkVersion},
                {"install_id", install_id},
                {"sets", set_ids}};
    if (wine) doc["wine"] = true;
    if (!launch.is_null()) doc["launch"] = ojson::parse(launch.dump());
    const std::string compare = doc.dump();
    // last_played changes every time; only it changing isn't worth a write
    // until the session ends.
    if (!force && compare == last_game_json) return;
    doc["last_played"] = UnixSeconds();
    if (Write(dir / "game.json", doc)) last_game_json = compare;
  }
};

Library::Library(System& system, LibraryOptions options)
    : impl_(std::make_unique<Impl>(system, std::move(options))) {
  if (!impl_->options.enabled || impl_->options.game_id.empty()) return;
  try {
    impl_->Init();
    if (impl_->active) impl_->Start();
  } catch (...) {
    impl_->active = false;
  }
}

Library::~Library() {
  if (!impl_->active) return;
  impl_->Stop();
  impl_->Work(true);
  std::lock_guard lock(impl_->io_mutex);
  impl_->ClearMarker();
}

void Library::Flush() {
  if (impl_->active) impl_->Work(true);
}

bool Library::enabled() const { return impl_->active; }
fs::path Library::dir() const { return impl_->active ? impl_->dir : fs::path(); }
fs::path Library::root() const { return impl_->active ? impl_->root : fs::path(); }
std::string Library::game_id() const { return impl_->game_id; }
LibraryMode Library::mode() const { return impl_->mode; }
std::string Library::install_id() const { return impl_->install_id; }
std::string Library::ProfileId(std::string_view profile) const { return impl_->ProfileId(profile); }

Library::Stats Library::stats() const {
  std::lock_guard lock(impl_->stats_mutex);
  return impl_->stats;
}

}  // namespace recomp::achievements
