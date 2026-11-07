// Launching games from the library: what a game records, Steam shortcuts,
// the running marker, launch plans and the player's choices.

#include <chrono>
#include <thread>

#include "recomp/achievements/launch.h"
#include "recomp/achievements/library.h"
#include "recomp/achievements/library_reader.h"
#include "test.h"

using namespace recomp::achievements;
namespace fs = std::filesystem;

namespace {

// A binary shortcuts.vdf like Steam writes.
struct Vdf {
  std::vector<uint8_t> b;
  void Key(uint8_t type, const std::string& k) {
    b.push_back(type);
    b.insert(b.end(), k.begin(), k.end());
    b.push_back(0);
  }
  void Str(const std::string& k, const std::string& v) {
    Key(0x01, k);
    b.insert(b.end(), v.begin(), v.end());
    b.push_back(0);
  }
  void Int(const std::string& k, uint32_t v) {
    Key(0x02, k);
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  void Open(const std::string& k) { Key(0x00, k); }
  void Close() { b.push_back(0x08); }
};

struct Entry {
  uint32_t appid;
  std::string name, exe, dir, options;
};

std::vector<uint8_t> ShortcutsVdf(const std::vector<Entry>& entries) {
  Vdf v;
  v.Open("shortcuts");
  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& e = entries[i];
    v.Open(std::to_string(i));
    v.Int("appid", e.appid);
    v.Str("AppName", e.name);
    v.Str("Exe", e.exe);
    v.Str("StartDir", e.dir);
    v.Str("icon", "");
    v.Str("LaunchOptions", e.options);
    v.Int("IsHidden", 0);
    v.Open("tags");
    v.Str("0", "Recomps");
    v.Close();
    v.Close();
  }
  v.Close();
  v.Close();
  return v.b;
}

AchievementSet OneSet() {
  AchievementSet s;
  s.id = "launch_game";
  s.title = "Launch Game";
  s.platform = Platform::kCustom;
  Achievement a;
  a.key = "launch_game:a";
  a.name = "A";
  s.achievements.push_back(a);
  return s;
}

}  // namespace

TEST(shortcuts_vdf_parses) {
  const auto list = ParseSteamShortcuts(ShortcutsVdf({
      {3000000001u, "My Game", "\"/games/my_game/my_game\"", "\"/games/my_game/\"", ""},
      {123u, "My Game", "/games/my_game/launch.sh", "/games/my_game", "--fullscreen"},
  }));
  CHECK_EQ(list.size(), size_t(2));
  CHECK_EQ(list[0].appid, 3000000001u);
  CHECK_EQ(list[0].name, std::string("My Game"));
  CHECK_EQ(UnquoteExe(list[0].exe), std::string("/games/my_game/my_game"));
  CHECK_EQ(list[0].game_id(), (uint64_t(3000000001u) << 32) | 0x02000000u);
  CHECK_EQ(list[1].launch_options, std::string("--fullscreen"));
  // Truncated or foreign files give nothing rather than garbage.
  auto bytes = ShortcutsVdf({{1, "x", "/x", "/", ""}});
  bytes.resize(bytes.size() / 2);
  CHECK(ParseSteamShortcuts(bytes).empty());
  CHECK(ParseSteamShortcuts({'h', 'i'}).empty());
}

TEST(shortcuts_found_under_steam_userdata) {
  const fs::path home = test::TempDir("launch_home");
  const fs::path cfg = home / ".local" / "share" / "Steam" / "userdata" / "1234" / "config";
  fs::create_directories(cfg);
  const auto bytes = ShortcutsVdf({{7, "Game", "/g/game", "/g", ""}});
  test::WriteFile(cfg / "shortcuts.vdf", std::string(bytes.begin(), bytes.end()));
  // ~/.steam/steam is usually a link to the same folder: listed once.
  fs::create_directories(home / ".steam");
  fs::create_directory_symlink(home / ".local" / "share" / "Steam", home / ".steam" / "steam");
  const auto files = FindSteamShortcutFiles(home);
  CHECK_EQ(files.size(), size_t(1));
  const auto list = ReadSteamShortcuts(files[0]);
  CHECK_EQ(list.size(), size_t(1));
  CHECK_EQ(list[0].file, files[0]);
}

TEST(shortcut_matching) {
  LaunchInfo info;
  info.os = "linux";
  info.exe = "/games/my_game/build/my_game";
  info.cwd = "/games/my_game";
  const auto list = ParseSteamShortcuts(ShortcutsVdf({
      {1, "Other", "/games/other/other", "/games/other", ""},
      {2, "Wrapper", "/usr/bin/env", "/", "/games/my_game/build/my_game %command%"},
      {3, "Script", "\"/games/my_game/run.sh\"", "\"/games/my_game\"", ""},
      {4, "Exact", "\"/games/my_game/build/my_game\"", "\"/games/my_game/build\"", ""},
  }));
  const SteamShortcut* m = MatchShortcut(info, list);
  CHECK(m);
  CHECK_EQ(m->appid, 4u);  // The same executable beats the rest.
  const std::vector<SteamShortcut> no_exact(list.begin(), list.begin() + 3);
  m = MatchShortcut(info, no_exact);
  CHECK(m);
  CHECK_EQ(m->appid, 2u);  // Then a wrapper naming it.
  const std::vector<SteamShortcut> script{list[0], list[2]};
  m = MatchShortcut(info, script);
  CHECK(m);
  CHECK_EQ(m->appid, 3u);  // Then a script in the game's folder.
  CHECK(!MatchShortcut(info, {list[0]}));
}

TEST(library_records_launch_and_running) {
  const fs::path root = test::TempDir("launch_library");
  {
    System system;
    system.AddSet(OneSet());
    LibraryOptions o;
    o.game_id = "launch_game";
    o.root = root;
    Library library(system, o);
    library.Flush();
    const nlohmann::json game = test::ReadJson(root / "launch_game" / "game.json");
    CHECK(game.contains("launch"));
    CHECK(!game["launch"]["exe"].get<std::string>().empty());
#if defined(__linux__)
    CHECK_EQ(game["launch"]["os"].get<std::string>(), std::string("linux"));
    CHECK_EQ(fs::path(game["launch"]["exe"].get<std::string>()), fs::read_symlink("/proc/self/exe"));
#endif
    auto g = LoadLibraryGame(root, "launch_game");
    CHECK(g);
    CHECK(!g->launch.empty());
    CHECK(g->running);
    CHECK(MarkerAlive(*g->running));
    CHECK(PlanLaunch(*g, {}).running);
  }
  // Gone after a clean exit; the launch info stays.
  CHECK(!fs::exists(root / "launch_game" / "running.json"));
  auto g = LoadLibraryGame(root, "launch_game");
  CHECK(g);
  CHECK(!g->running);
  CHECK(!g->launch.empty());
  CHECK(!PlanLaunch(*g, {}).running);
}

TEST(library_launch_override_and_off) {
  const fs::path root = test::TempDir("launch_override");
  System system;
  system.AddSet(OneSet());
  LibraryOptions o;
  o.game_id = "g";
  o.root = root;
  o.launch.exe = "/games/g/start.sh";
  o.launch.args = {"--quick"};
  o.running_marker = false;
  {
    Library library(system, o);
    library.Flush();
    CHECK(!fs::exists(root / "g" / "running.json"));
  }
  auto g = LoadLibraryGame(root, "g");
  CHECK(g);
  CHECK_EQ(g->launch.exe, std::string("/games/g/start.sh"));
  CHECK_EQ(g->launch.args.size(), size_t(1));
  o.record_launch = false;
  o.root = test::TempDir("launch_off");
  {
    Library library(system, o);
    library.Flush();
  }
  CHECK(!test::ReadJson(o.root / "g" / "game.json").contains("launch"));
}

TEST(stale_markers_dont_count) {
  auto m = CurrentRunningMarker();
  CHECK(m);
  CHECK(MarkerAlive(*m));
  RunningMarker other = *m;
  other.start += 1;  // The pid was reused by another process.
  CHECK(!MarkerAlive(other));
  other = *m;
  other.os = "plan9";
  CHECK(!MarkerAlive(other));
#if defined(__linux__)
  other = *m;
  other.boot_id = "00000000-0000-0000-0000-000000000000";  // Before a reboot.
  CHECK(!MarkerAlive(other));
#endif
  CHECK(!MarkerFromJson(nlohmann::json::object()));
  auto back = MarkerFromJson(MarkerToJson(*m));
  CHECK(back);
  CHECK_EQ(back->pid, m->pid);
  CHECK_EQ(back->start, m->start);
}

TEST(launch_plans) {
  const fs::path dir = test::TempDir("launch_plans");
  test::WriteFile(dir / "game", "#!/bin/sh\n");
  LibraryGame g;
  g.id = "g";
  g.launch.os =
#if defined(_WIN32)
      "windows";
#elif defined(__APPLE__)
      "macos";
#else
      "linux";
#endif
  g.launch.exe = (dir / "game").string();
  g.launch.cwd = dir.string();
  g.launch.args = {"--x"};

  // No Steam entry: direct only.
  LaunchPlan p = PlanLaunch(g, {});
  CHECK(p.direct);
  CHECK(p.steam_game_id.empty());
  std::string error;
  CHECK(!BuildLaunch(p, LaunchMethod::kSteam, &error));
  CHECK(!error.empty());
  auto direct = BuildLaunch(p, LaunchMethod::kDirect);
  CHECK(direct);
  CHECK_EQ(direct->argv.size(), size_t(2));
  CHECK_EQ(direct->argv[0], g.launch.exe);
  CHECK_EQ(direct->cwd, g.launch.cwd);

  // Added to Steam after it first ran: found in shortcuts.vdf.
  const auto shortcuts = ParseSteamShortcuts(ShortcutsVdf({{42, "G", "\"" + g.launch.exe + "\"", "\"" + dir.string() + "\"", ""}}));
  p = PlanLaunch(g, shortcuts);
  CHECK_EQ(p.steam_source, std::string("shortcut"));
  CHECK_EQ(p.steam_game_id, std::to_string(shortcuts[0].game_id()));
  auto steam = BuildLaunch(p, LaunchMethod::kSteam);
  CHECK(steam);
#if !defined(_WIN32)
  CHECK_EQ(steam->argv.back(), "steam://rungameid/" + p.steam_game_id);
#endif

  // Started by Steam: the recorded id, while its shortcut still exists.
  g.launch.steam_game_id = std::to_string(shortcuts[0].game_id());
  p = PlanLaunch(g, shortcuts);
  CHECK_EQ(p.steam_source, std::string("recorded"));
  const auto others = ParseSteamShortcuts(ShortcutsVdf({{43, "H", "/h/h", "/h", ""}}));
  g.launch.steam_game_id = std::to_string(others[0].game_id() + (uint64_t(1) << 32));
  CHECK(PlanLaunch(g, others).steam_game_id.empty());  // Shortcut removed.
  g.launch.steam_game_id = "1234560";                  // A store app id is kept.
  CHECK_EQ(PlanLaunch(g, others).steam_game_id, std::string("1234560"));

  // Proton builds start through Steam only.
  g.launch.os = "windows";
  g.launch.wine = true;
  g.launch.proton = true;
#if !defined(_WIN32)
  p = PlanLaunch(g, {});
  CHECK(!p.direct);
  CHECK(!p.direct_note.empty());
#endif
  // Nothing recorded yet.
  LibraryGame empty;
  p = PlanLaunch(empty, {});
  CHECK(!p.any());
  CHECK(!p.direct_note.empty());
}

#if defined(__linux__) || defined(__APPLE__)
TEST(start_detached_runs_and_reports_failure) {
  const fs::path dir = test::TempDir("launch_start");
  LaunchCommand c;
  c.argv = {"/bin/sh", "-c", "printf '%s' \"$LAUNCH_TEST\" > out.txt"};
  c.cwd = dir.string();
  c.env["LAUNCH_TEST"] = "started";
  std::string error;
  CHECK(StartDetached(c, &error));
  for (int i = 0; i < 200 && test::ReadFile(dir / "out.txt") != "started"; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  CHECK_EQ(test::ReadFile(dir / "out.txt"), std::string("started"));

  c.argv = {(dir / "missing").string()};
  CHECK(!StartDetached(c, &error));
  CHECK(!error.empty());
  c.argv = {"/bin/sh"};
  c.cwd = (dir / "no such folder").string();
  CHECK(!StartDetached(c, &error));
}
#endif

TEST(launcher_prefs_round_trip) {
  const fs::path file = test::TempDir("launch_prefs") / "sub" / "launcher.json";
  LauncherPrefs p = LoadLauncherPrefs(file);
  CHECK(p.quit_current);
  CHECK(p.methods.empty());
  p.methods["my_game"] = LaunchMethod::kDirect;
  p.methods["my_game"] = LaunchMethod::kSteam;
  p.quit_current = false;
  CHECK(SaveLauncherPrefs(file, p));
  const LauncherPrefs q = LoadLauncherPrefs(file);
  CHECK(!q.quit_current);
  CHECK(q.methods.at("my_game") == LaunchMethod::kDirect);
  CHECK(q.methods.at("my_game") == LaunchMethod::kSteam);
}
