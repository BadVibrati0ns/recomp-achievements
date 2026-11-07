#pragma once

// Starting a game again from the library (docs/launching.md). A running game
// records how it was started (its executable, arguments, folder and Steam id)
// in its library entry, plus a running.json marker while it runs. An app
// reads them back to offer "Launch through Steam" and "Launch directly", to
// find a Steam shortcut added after the game first ran, and to avoid starting
// a second copy. Nothing here uses the network.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace recomp::achievements {

struct LibraryGame;

// How the game was started. Paths are the host's: a Windows build under Wine
// records Linux paths (and the prefix), so an app on the host can use them.
struct LaunchInfo {
  std::string os;   // "linux", "windows", "macos": what the executable is built for.
  std::string exe;  // Absolute.
  std::vector<std::string> args;  // Without the executable.
  std::string cwd;
  // Steam's game id when Steam started it (SteamGameId): an app id, or a
  // shortcut's 64-bit id. Empty otherwise.
  std::string steam_game_id;
  bool wine = false;
  std::string wine_prefix;   // WINEPREFIX, as a host path; empty: ~/.wine or unknown.
  bool proton = false;       // Started by Proton (STEAM_COMPAT_DATA_PATH was set).

  bool empty() const { return exe.empty(); }
};
nlohmann::json LaunchToJson(const LaunchInfo& info);
LaunchInfo LaunchFromJson(const nlohmann::json& j);

// This process: /proc/self (Linux), GetModuleFileNameW and the command line
// (Windows, translated to host paths under Wine), _NSGetExecutablePath
// (macOS). RECOMP_ACHIEVEMENTS_LAUNCH_EXE (and _ARGS, one per line) replace
// the executable and arguments, for games started by a wrapper script that
// should be started again instead. A Steam id equal to
// RECOMP_ACHIEVEMENTS_PARENT_STEAM_ID (set by an app that started the game
// directly from inside its own Steam session) isn't recorded.
LaunchInfo CurrentLaunchInfo();

// The marker a game keeps in its entry while it runs (running.json), removed
// when it exits cleanly. A marker left by a crash is told apart by the
// process id and start time no longer matching.
struct RunningMarker {
  uint64_t pid = 0;    // The host's process id (under Wine, the Unix one).
  uint64_t start = 0;  // The process's start time: clock ticks since boot (Linux), FILETIME (Windows).
  std::string boot_id;  // Linux: /proc/sys/kernel/random/boot_id.
  std::string os;       // The host the pid belongs to: "linux", "windows", "macos".
  uint64_t started = 0;  // Unix seconds.
};
nlohmann::json MarkerToJson(const RunningMarker& marker);
std::optional<RunningMarker> MarkerFromJson(const nlohmann::json& j);
// <game dir>/running.json; nullopt when there's none.
std::optional<RunningMarker> ReadRunningMarker(const std::filesystem::path& game_dir);
// This process's marker; nullopt when its id can't be found.
std::optional<RunningMarker> CurrentRunningMarker();
// True when that process still runs on this machine (a stopped or suspended
// one counts). False for another OS or boot.
bool MarkerAlive(const RunningMarker& marker);

// ---- Steam --------------------------------------------------------------------

struct SteamShortcut {
  uint32_t appid = 0;
  std::string name, exe, start_dir, launch_options;
  std::filesystem::path file;  // The shortcuts.vdf it came from.
  // The id steam://rungameid/ takes: (appid << 32) | 0x02000000.
  uint64_t game_id() const { return (static_cast<uint64_t>(appid) << 32) | 0x02000000u; }
};
// A binary shortcuts.vdf. Empty when unreadable.
std::vector<SteamShortcut> ReadSteamShortcuts(const std::filesystem::path& vdf);
std::vector<SteamShortcut> ParseSteamShortcuts(const std::vector<uint8_t>& bytes);
// Every userdata/<user>/config/shortcuts.vdf under Steam's folders: native and
// Flatpak Steam under `home` on Linux, Steam's install folder on Windows.
std::vector<std::filesystem::path> FindSteamShortcutFiles(const std::filesystem::path& home);

// A shortcut's "Exe" without its quotes.
std::string UnquoteExe(std::string exe);
// The shortcut that starts this game: its Exe is the recorded executable
// (or names it, for a wrapper), or its folder is the game's. Null if none.
const SteamShortcut* MatchShortcut(const LaunchInfo& info, const std::vector<SteamShortcut>& shortcuts);

// ---- Launching ------------------------------------------------------------------

enum class LaunchMethod { kSteam, kDirect };
const char* LaunchMethodName(LaunchMethod method);  // "steam", "direct"
std::optional<LaunchMethod> ParseLaunchMethod(const std::string& name);

// What an app can offer for one game.
struct LaunchPlan {
  std::string steam_game_id;  // Empty: no Steam entry found.
  std::string steam_source;   // "recorded" (Steam started it) or "shortcut" (found in shortcuts.vdf).
  bool direct = false;        // Can be started without Steam.
  std::string direct_note;    // Why not, when it can't; else what will run.
  bool running = false;
  LaunchInfo info;
  bool any() const { return direct || !steam_game_id.empty(); }
};
// shortcuts: what FindSteamShortcutFiles found, read now, so a shortcut
// added since the game last ran is seen.
LaunchPlan PlanLaunch(const LibraryGame& game, const std::vector<SteamShortcut>& shortcuts);

// What to run: a program and its arguments, the folder, extra environment.
struct LaunchCommand {
  std::vector<std::string> argv;
  std::string cwd;
  std::map<std::string, std::string> env;
  std::string url;  // Set instead of argv on Windows: opened with the shell.
};
// Nullopt with *error set when the plan can't do that.
std::optional<LaunchCommand> BuildLaunch(const LaunchPlan& plan, LaunchMethod method, std::string* error = nullptr);
// Starts it detached (its own session; it outlives the caller). False with
// *error when it couldn't start.
bool StartDetached(const LaunchCommand& command, std::string* error = nullptr);

// The player's launch choices for every game: <GlobalConfigDir()>/launcher.json.
struct LauncherPrefs {
  std::map<std::string, LaunchMethod> methods;  // Last choice, by game id.
  bool quit_current = true;  // In-game launching: quit the game it was started from.
};
LauncherPrefs LoadLauncherPrefs(const std::filesystem::path& file);
bool SaveLauncherPrefs(const std::filesystem::path& file, const LauncherPrefs& prefs);
// <GlobalConfigDir()>/launcher.json; empty when there's no such folder.
std::filesystem::path DefaultLauncherPrefsFile();

}  // namespace recomp::achievements
