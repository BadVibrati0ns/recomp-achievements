#include "recomp/achievements/app/library_view.h"

#include <algorithm>
#include <chrono>

namespace recomp::achievements::app {

namespace fs = std::filesystem;

const LibraryProfile* PickProfile(const LibraryGame& game, const std::string& id) {
  if (!id.empty())
    for (const auto& p : game.profiles)
      if (p.id == id) return &p;
  return game.BestProfile();
}

GameSummary Summarize(const LibraryGame& game, const std::string& profile_id) {
  GameSummary g;
  g.id = game.id;
  g.title = game.title.empty() ? game.id : game.title;
  g.root = game.dir.parent_path();
  g.icon = game.icon;
  g.last_played = game.last_played;
  g.profiles = game.profiles.size();
  if (!game.sets.empty()) g.platform = game.sets.front().set.platform;
  else if (!game.platforms.empty()) g.platform = game.platforms.front();
  const LibraryProfile* profile = PickProfile(game, profile_id);
  if (profile) g.profile = profile->id;
  for (const LibrarySet& s : game.sets) {
    if (!s.enabled) {
      ++g.sets_off;
      continue;
    }
    ++g.sets_on;
    for (const Achievement& a : s.set.achievements) {
      bool unlocked = false;
      if (profile)
        if (auto it = profile->progress.find(a.key); it != profile->progress.end()) unlocked = it->second.progress.unlocked;
      ++g.count;
      g.points += a.points;
      if (unlocked) ++g.unlocked, g.unlocked_points += a.points;
      if (a.tier != Tier::kNone) {
        g.trophies = true;
        TierCount& t = g.tiers[static_cast<int>(a.tier)];
        ++t.total;
        if (unlocked) ++t.unlocked;
      }
    }
  }
  return g;
}

const char* SortName(SortMode mode) {
  switch (mode) {
    case SortMode::kLastPlayed: return "Last played";
    case SortMode::kPercent: return "% complete";
    case SortMode::kClosest: return "Closest to done";
    case SortMode::kTitle: return "A\xE2\x80\x93Z";
  }
  return "";
}

namespace {

std::string Folded(const std::string& s) {
  std::string o = s;
  for (char& c : o)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return o;
}

bool ByTitle(const GameSummary& a, const GameSummary& b) {
  const std::string fa = Folded(a.title), fb = Folded(b.title);
  return fa != fb ? fa < fb : a.id < b.id;
}

// a's fraction done against b's, exactly: -1, 0 or 1. Empty games count as 0%.
int CompareDone(const GameSummary& a, const GameSummary& b) {
  const uint64_t l = uint64_t(a.count ? a.unlocked : 0) * (b.count ? b.count : 1);
  const uint64_t r = uint64_t(b.count ? b.unlocked : 0) * (a.count ? a.count : 1);
  return l < r ? -1 : l > r ? 1 : 0;
}

}  // namespace

void SortGames(std::vector<GameSummary>& games, SortMode mode) {
  std::sort(games.begin(), games.end(), [mode](const GameSummary& a, const GameSummary& b) {
    switch (mode) {
      case SortMode::kLastPlayed:
        if (a.last_played != b.last_played) return a.last_played > b.last_played;
        break;
      case SortMode::kPercent:
        if (const int c = CompareDone(a, b)) return c > 0;
        break;
      case SortMode::kClosest: {
        // Games with nothing left to do (finished or empty) go last.
        const bool da = a.complete() || !a.count, db = b.complete() || !b.count;
        if (da != db) return db;
        const uint32_t la = a.count - a.unlocked, lb = b.count - b.unlocked;
        if (!da && la != lb) return la < lb;
        if (const int c = CompareDone(a, b)) return c > 0;
        break;
      }
      case SortMode::kTitle: break;
    }
    return ByTitle(a, b);
  });
}

namespace {

uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

uint64_t HashText(uint64_t h, const std::string& s) {
  for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
  return Mix(h, s.size());
}

uint64_t StampFile(uint64_t h, const fs::path& p) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  if (ec) return Mix(h, 1);
  const auto time = fs::last_write_time(p, ec);
  h = HashText(h, p.filename().string());
  h = Mix(h, size);
  if (!ec) h = Mix(h, static_cast<uint64_t>(time.time_since_epoch().count()));
  return h;
}

uint64_t StampDir(uint64_t h, const fs::path& dir) {
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    const std::string name = e.path().filename().string();
    if (name.find(".tmp-") != std::string::npos) continue;  // Half-written.
    files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) h = StampFile(h, f);
  return Mix(h, files.size());
}

}  // namespace

std::map<std::string, EntryStamp> StampLibrary(const std::vector<fs::path>& roots) {
  std::map<std::string, EntryStamp> out;
  for (const auto& root : roots) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root, ec)) {
      const std::string id = e.path().filename().string();
      if (id.empty() || id[0] == '.' || !e.is_directory(ec)) continue;
      EntryStamp s;
      s.sets = StampFile(s.sets, e.path() / "game.json");
      s.sets = StampDir(s.sets, e.path() / "sets");
      s.sets = StampFile(s.sets, e.path() / "icons" / "index.json");
      s.progress = StampDir(s.progress, e.path() / "progress");
      out[root.string() + "|" + id] = s;
    }
  }
  return out;
}

void MirrorGame(System& system, const LibraryGame& game, const LibraryProfile* profile, bool sets) {
  if (sets) {
    system.ClearSets();
    for (const LibrarySet& s : game.sets) {
      system.AddSet(s.set);
      system.SetSetEnabled(s.set.id, s.enabled);
    }
  }
  std::map<std::string, Progress> progress;
  if (profile)
    for (const auto& [key, p] : profile->progress) progress[key] = p.progress;
  system.LoadProgress(progress, profile ? profile->id : std::string("default"));
  if (!profile) return;
  // Bars the System can't work out from the trigger alone (game-defined
  // types fed by RecordValue) take the total the game showed.
  for (const AchievementView& v : system.Snapshot(true)) {
    const auto it = profile->progress.find(v.achievement->key);
    if (it == profile->progress.end() || !it->second.total || v.progress.unlocked) continue;
    if (v.progress_total != it->second.total || v.progress_current != it->second.current)
      system.RecordValue(v.achievement->key, it->second.current, it->second.total);
  }
}

std::string RelativeTime(uint64_t then, uint64_t now) {
  if (!then) return "never";
  if (then >= now || now - then < 60) return "just now";
  const uint64_t d = now - then;
  auto n = [](uint64_t v, const char* unit) {
    return std::to_string(v) + " " + unit + (v == 1 ? "" : "s") + " ago";
  };
  if (d < 3600) return n(d / 60, "minute");
  if (d < 86400) return n(d / 3600, "hour");
  if (d < 2 * 86400) return "yesterday";
  if (d < 14 * 86400) return n(d / 86400, "day");
  if (d < 60 * 86400) return n(d / (7 * 86400), "week");
  if (d < 365 * 86400) return n(d / (30 * 86400), "month");
  return n(d / (365 * 86400), "year");
}

}  // namespace recomp::achievements::app
