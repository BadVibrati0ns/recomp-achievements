#include "recomp/achievements/library_reader.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

#include "json_format.h"
#include "library_util.h"
#include "recomp/achievements/library.h"

namespace recomp::achievements {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string PathText(const fs::path& p) {
  const auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
}

std::string Str(const json& obj, const char* key) {
  const auto it = obj.find(key);
  return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

uint64_t U64(const json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_number()) return 0;
  if (it->is_number_unsigned()) return it->get<uint64_t>();
  const double d = it->get<double>();
  return d > 0 ? static_cast<uint64_t>(d) : 0;
}

uint32_t U32(const json& obj, const char* key) {
  return static_cast<uint32_t>(std::min<uint64_t>(U64(obj, key), 0xffffffffu));
}

LibraryTotals TotalsFrom(const json& j) {
  LibraryTotals t;
  if (!j.is_object()) return t;
  t.count = U32(j, "count");
  t.unlocked = U32(j, "unlocked");
  t.points = U32(j, "points");
  t.unlocked_points = U32(j, "unlocked_points");
  return t;
}

// The version number of "<prefix>N" schema ids; 0 when unreadable.
int SchemaVersion(const std::string& schema) {
  const size_t slash = schema.rfind('/');
  if (slash == std::string::npos) return 0;
  try {
    return std::max(0, std::stoi(schema.substr(slash + 1)));
  } catch (...) {
    return 0;
  }
}

// A path inside the entry folder, or empty: entries are data from other
// programs and must not point the app elsewhere.
fs::path InsideEntry(const fs::path& entry, const fs::path& path) {
  const fs::path full = (path.is_absolute() ? path : entry / path).lexically_normal();
  const fs::path rel = full.lexically_relative(entry.lexically_normal());
  if (rel.empty() || *rel.begin() == "..") return {};
  return full;
}

bool LoadProfile(const fs::path& path, LibraryProfile& out) {
  const json doc = detail::ReadJsonFile(path);
  if (!doc.is_object()) return false;
  out.id = Str(doc, "profile");
  if (out.id.empty()) out.id = PathText(path.stem());
  out.updated = U64(doc, "updated");
  if (const auto it = doc.find("totals"); it != doc.end()) out.totals = TotalsFrom(*it);
  if (const auto it = doc.find("sets"); it != doc.end() && it->is_object()) {
    for (const auto& [id, t] : it->items()) out.set_totals[id] = TotalsFrom(t);
  }
  if (const auto it = doc.find("achievements"); it != doc.end() && it->is_object()) {
    for (const auto& [key, e] : it->items()) {
      if (!e.is_object()) continue;
      LibraryProgress p;
      p.progress = detail::ProgressFromJson(e);
      p.current = U32(e, "current");
      p.total = U32(e, "total");
      out.progress[key] = std::move(p);
    }
    // Older or partial files without totals still count their unlocks.
    if (doc.find("totals") == doc.end()) {
      for (const auto& [key, p] : out.progress) out.totals.unlocked += p.progress.unlocked ? 1 : 0;
    }
  }
  return true;
}

bool LoadSet(const LibraryGame& game, const fs::path& path, LibrarySet& out, std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = PathText(path) + ": cannot open file";
    return false;
  }
  json doc = json::parse(in, nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
  if (doc.is_discarded()) {
    if (error) *error = PathText(path) + ": invalid JSON";
    return false;
  }
  AchievementSet set;
  if (!detail::ParseNativeSet(doc, path, set, error)) return false;
  // What System::AddSet would fill in; points are already capped.
  for (auto& a : set.achievements) {
    a.set_id = set.id;
    a.key = set.id + ":" + a.local_id;
    if (a.category.empty()) a.category = set.category;
    for (std::string* ref : {&a.icon, &a.locked_icon}) {
      if (ref->empty()) continue;
      const fs::path full = InsideEntry(game.dir, set.base_dir / fs::path(*ref));
      *ref = full.empty() ? std::string() : PathText(full);
    }
  }
  out.enabled = set.enabled_by_default;
  if (const auto lib = doc.find("_library"); lib != doc.end() && lib->is_object()) {
    if (const auto e = lib->find("enabled"); e != lib->end() && e->is_boolean()) out.enabled = e->get<bool>();
  }
  out.set = std::move(set);
  return true;
}

LibraryGame ReadGame(const fs::path& dir, const std::string& id, bool with_sets) {
  LibraryGame g;
  g.id = id;
  g.dir = dir;
  std::error_code ec;
  const json doc = detail::ReadJsonFile(dir / "game.json");
  if (doc.is_object()) {
    g.schema = std::max(1, SchemaVersion(Str(doc, "schema")));
    if (g.schema > 1) g.warnings.push_back("game.json: schema " + std::to_string(g.schema) + " is newer than this reader");
    g.title = Str(doc, "title");
    if (const auto it = doc.find("platforms"); it != doc.end() && it->is_array()) {
      for (const auto& p : *it)
        if (p.is_string())
          if (const auto parsed = detail::ParsePlatform(p.get<std::string>())) g.platforms.push_back(*parsed);
    }
    g.game_version = Str(doc, "game_version");
    g.sdk_version = Str(doc, "sdk_version");
    g.install_id = Str(doc, "install_id");
    if (const std::string icon = Str(doc, "icon"); !icon.empty()) {
      const fs::path full = InsideEntry(dir, fs::path(icon));
      if (!full.empty() && fs::is_regular_file(full, ec)) g.icon = full;
    }
    g.last_played = U64(doc, "last_played");
    if (const auto it = doc.find("wine"); it != doc.end() && it->is_boolean()) g.wine = it->get<bool>();
    if (const auto it = doc.find("sets"); it != doc.end() && it->is_array()) {
      for (const auto& s : *it)
        if (s.is_string()) g.set_ids.push_back(s.get<std::string>());
    }
    if (const auto it = doc.find("launch"); it != doc.end()) g.launch = LaunchFromJson(*it);
  } else {
    g.warnings.push_back("no readable game.json");
  }
  g.running = ReadRunningMarker(dir);
  if (g.title.empty()) g.title = id;

  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir / "progress", ec)) {
    if (e.path().extension() == ".json" && e.is_regular_file(ec)) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    LibraryProfile p;
    if (LoadProfile(f, p)) g.profiles.push_back(std::move(p));
    else g.warnings.push_back(PathText(f.filename()) + ": unreadable");
  }

  if (!with_sets) return g;
  files.clear();
  for (const auto& e : fs::directory_iterator(dir / "sets", ec)) {
    if (PathText(e.path().filename()).ends_with(".achievements.json") && e.is_regular_file(ec)) files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    LibrarySet s;
    std::string error;
    if (LoadSet(g, f, s, &error)) g.sets.push_back(std::move(s));
    else g.warnings.push_back(error);
  }
  if (!g.set_ids.empty()) {
    // Only the sets the game had loaded last, in its order; files of sets it
    // no longer loads stay on disk but aren't shown.
    std::vector<LibrarySet> ordered;
    for (const auto& id_wanted : g.set_ids) {
      for (auto& s : g.sets) {
        if (s.set.id == id_wanted && !s.set.id.empty()) {
          ordered.push_back(std::move(s));
          s.set.id.clear();
          break;
        }
      }
    }
    g.sets = std::move(ordered);
  }
  return g;
}

bool LooksLikeEntry(const fs::path& dir) {
  std::error_code ec;
  return fs::is_regular_file(dir / "game.json", ec) || fs::is_directory(dir / "sets", ec) ||
         fs::is_directory(dir / "progress", ec);
}

bool SafeId(const std::string& id) {
  return !id.empty() && id != "." && id != ".." && id.find_first_of("/\\:") == std::string::npos;
}

void ReadEventFile(const fs::path& path, uint64_t since, std::vector<LibraryEvent>& out) {
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const json j = json::parse(line, nullptr, /*allow_exceptions=*/false);
    LibraryEvent e;
    if (EventFromJson(j, e) && e.t >= since) out.push_back(std::move(e));
  }
}

}  // namespace

// ---- Events ------------------------------------------------------------------------

namespace {

template <typename Json>
Json EventJson(const LibraryEvent& e) {
  Json j = {{"v", e.v},          {"t", e.t},       {"install_id", e.install_id}, {"game", e.game},
            {"profile", e.profile}, {"key", e.key}, {"kind", e.kind}};
  if (!e.value.is_null()) j["value"] = Json::parse(e.value.dump());
  if (!e.item.empty()) j["item"] = e.item;
  j["game_version"] = e.game_version;
  return j;
}

}  // namespace

json EventToJson(const LibraryEvent& e) { return EventJson<json>(e); }

std::string detail::EventLine(const LibraryEvent& e) {
  return EventJson<nlohmann::ordered_json>(e).dump(-1, ' ', false, json::error_handler_t::replace);
}

bool EventFromJson(const json& j, LibraryEvent& out) {
  if (!j.is_object()) return false;
  LibraryEvent e;
  e.v = static_cast<int>(U32(j, "v"));
  e.t = U64(j, "t");
  e.kind = Str(j, "kind");
  if (e.t == 0 || e.kind.empty()) return false;
  e.install_id = Str(j, "install_id");
  e.game = Str(j, "game");
  e.profile = Str(j, "profile");
  e.key = Str(j, "key");
  if (const auto it = j.find("value"); it != j.end()) e.value = *it;
  e.item = Str(j, "item");
  e.game_version = Str(j, "game_version");
  out = std::move(e);
  return true;
}

std::vector<LibraryEvent> MergeEvents(std::vector<LibraryEvent> a, const std::vector<LibraryEvent>& b) {
  a.insert(a.end(), b.begin(), b.end());
  std::stable_sort(a.begin(), a.end(), [](const LibraryEvent& x, const LibraryEvent& y) {
    return std::tie(x.t, x.install_id, x.key, x.kind) < std::tie(y.t, y.install_id, y.key, y.kind);
  });
  a.erase(std::unique(a.begin(), a.end(), [](const LibraryEvent& x, const LibraryEvent& y) { return x.SameAs(y); }),
          a.end());
  return a;
}

std::vector<LibraryEvent> ReadEvents(const LibraryGame& game, uint64_t since_ms) {
  std::vector<LibraryEvent> out;
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(game.dir, ec)) {
    const std::string name = PathText(e.path().filename());
    if (name == "events.log" || (name.starts_with("events-") && name.ends_with(".log"))) files.push_back(e.path());
  }
  for (const auto& f : files) ReadEventFile(f, since_ms, out);
  return MergeEvents(std::move(out), {});
}

// ---- Games ---------------------------------------------------------------------------

const LibraryProfile* LibraryGame::BestProfile() const {
  const LibraryProfile* best = nullptr;
  for (const auto& p : profiles) {
    if (!best || p.totals.unlocked_points > best->totals.unlocked_points ||
        (p.totals.unlocked_points == best->totals.unlocked_points && p.updated > best->updated)) {
      best = &p;
    }
  }
  return best;
}

std::vector<LibraryGame> ListLibrary(const fs::path& root) {
  std::vector<LibraryGame> out;
  std::error_code ec;
  try {
    for (const auto& e : fs::directory_iterator(root, ec)) {
      const std::string id = PathText(e.path().filename());
      if (id.empty() || id[0] == '.' || !e.is_directory(ec) || !LooksLikeEntry(e.path())) continue;
      out.push_back(ReadGame(e.path(), id, false));
    }
  } catch (...) {
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  return out;
}

std::vector<LibraryGame> ListLibrary(const std::vector<fs::path>& roots) {
  std::vector<LibraryGame> out;
  std::set<fs::path> seen_roots;
  for (const auto& root : roots) {
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(root, ec);
    if (ec) canonical = root;
    if (!seen_roots.insert(canonical).second) continue;
    for (auto& g : ListLibrary(root)) {
      const auto it = std::find_if(out.begin(), out.end(), [&](const auto& x) { return x.id == g.id; });
      if (it == out.end()) out.push_back(std::move(g));
      else if (g.last_played > it->last_played) *it = std::move(g);
    }
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
  return out;
}

std::optional<LibraryGame> LoadLibraryGame(const fs::path& root, const std::string& game_id) {
  if (!SafeId(game_id)) return std::nullopt;
  const fs::path dir = root / game_id;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return std::nullopt;
  try {
    return ReadGame(dir, game_id, true);
  } catch (...) {
    return std::nullopt;
  }
}

LibraryTotals SumLibrary(const std::vector<LibraryGame>& games) {
  LibraryTotals sum;
  for (const auto& g : games) {
    const LibraryProfile* p = g.BestProfile();
    if (!p) continue;
    sum.count += p->totals.count;
    sum.unlocked += p->totals.unlocked;
    sum.points += p->totals.points;
    sum.unlocked_points += p->totals.unlocked_points;
  }
  return sum;
}

std::vector<LibraryUnlock> RecentUnlocks(const std::vector<LibraryGame>& games, size_t limit) {
  std::vector<LibraryUnlock> out;
  for (const auto& g : games) {
    for (const auto& p : g.profiles) {
      for (const auto& [key, progress] : p.progress) {
        if (progress.progress.unlocked && progress.progress.unlock_time > 0)
          out.push_back({g.id, p.id, key, progress.progress.unlock_time});
      }
    }
  }
  std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.time > b.time; });
  if (out.size() > limit) out.resize(limit);
  return out;
}

// ---- Wine and Proton prefixes ------------------------------------------------------

namespace {

// "path" entries of Steam's libraryfolders.vdf: extra library folders.
std::vector<fs::path> SteamLibraryFolders(const fs::path& steam_root) {
  std::vector<fs::path> out;
  for (const fs::path& vdf : {steam_root / "steamapps" / "libraryfolders.vdf", steam_root / "config" / "libraryfolders.vdf"}) {
    std::ifstream in(vdf, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      const size_t key = line.find("\"path\"");
      if (key == std::string::npos) continue;
      const size_t open = line.find('"', key + 6);
      const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
      if (close == std::string::npos) continue;
      std::string path;
      for (size_t i = open + 1; i < close; ++i) {
        if (line[i] == '\\' && i + 1 < close) ++i;  // VDF escapes backslashes.
        path.push_back(line[i]);
      }
      if (!path.empty()) out.emplace_back(path);
    }
  }
  return out;
}

}  // namespace

std::vector<fs::path> FindWinePrefixLibraries(const fs::path& home) {
  std::vector<fs::path> out;
  if (home.empty()) return out;
  std::set<fs::path> seen;
  std::error_code ec;
  const auto add = [&](const fs::path& p) {
    if (!fs::is_directory(p, ec)) return;
    fs::path canonical = fs::weakly_canonical(p, ec);
    if (ec) canonical = p;
    if (seen.insert(canonical).second) out.push_back(p);
  };
  const fs::path roaming = fs::path("AppData") / "Roaming" / "recomp-achievements" / "library";

  std::vector<fs::path> libraries;
  for (const fs::path& steam : {home / ".local" / "share" / "Steam", home / ".steam" / "steam", home / ".steam" / "root",
                                home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
                                home / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam"}) {
    if (!fs::is_directory(steam, ec)) continue;
    libraries.push_back(steam);
    for (auto& extra : SteamLibraryFolders(steam)) libraries.push_back(std::move(extra));
  }
  std::set<fs::path> seen_libraries;
  for (const auto& lib : libraries) {
    fs::path canonical = fs::weakly_canonical(lib, ec);
    if (ec) canonical = lib;
    if (!seen_libraries.insert(canonical).second) continue;
    std::vector<fs::path> prefixes;
    for (const auto& e : fs::directory_iterator(lib / "steamapps" / "compatdata", ec)) prefixes.push_back(e.path());
    std::sort(prefixes.begin(), prefixes.end());
    for (const auto& prefix : prefixes) add(prefix / "pfx" / "drive_c" / "users" / "steamuser" / roaming);
  }

  std::vector<fs::path> users;
  for (const auto& e : fs::directory_iterator(home / ".wine" / "drive_c" / "users", ec)) users.push_back(e.path());
  std::sort(users.begin(), users.end());
  for (const auto& user : users) {
    add(user / roaming);
    add(user / "Application Data" / "recomp-achievements" / "library");
  }
  return out;
}

std::vector<fs::path> DefaultLibraryRoots() {
  std::vector<fs::path> roots;
  if (fs::path root = DefaultLibraryRoot(false); !root.empty()) roots.push_back(std::move(root));
#if !defined(_WIN32)
  for (auto& p : FindWinePrefixLibraries(GetEnv("HOME"))) roots.push_back(std::move(p));
#endif
  return roots;
}

}  // namespace recomp::achievements
