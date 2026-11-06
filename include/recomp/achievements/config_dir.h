#pragma once

// Where the player's files for every game live, and how a Windows build
// running under Wine or Proton finds the Linux host's copy of that folder.

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace recomp::achievements {

// The player's folder for every game using this SDK:
//   Windows        %APPDATA%\recomp-achievements
//   Linux, macOS   $XDG_CONFIG_HOME/recomp-achievements (default ~/.config/...)
// RECOMP_ACHIEVEMENTS_CONFIG_DIR overrides it. Holds the global popup style,
// sounds/ and the achievement library (docs/library.md). Empty when no home
// folder can be found.
std::filesystem::path GlobalConfigDir();

// Reads an environment variable; empty when unset. Tests pass fake ones.
using EnvLookup = std::function<std::string(const char* name)>;
std::string GetEnv(const char* name);

// True when this is a Windows build running under Wine or Proton (ntdll
// exports wine_get_version). Always false on other builds.
bool RunningUnderWine();

// Under Wine, where the Linux host's GlobalConfigDir() may be, as Windows
// paths through the Z: drive (Wine maps / to Z:), most likely first: from
// XDG_CONFIG_HOME, WINEHOMEDIR ("\??\Z:\home\user"), HOME, then
// Z:\home\<USER or LOGNAME>. `home` is the folder that must exist for the
// candidate to be used. Pure string work, so it can be tested anywhere.
struct WineHostCandidate {
  std::string config_dir;  // ...\.config\recomp-achievements
  std::string home;
};
std::vector<WineHostCandidate> WineHostConfigCandidates(const EnvLookup& env);
// The first candidate whose home exists, or empty (also when not under Wine).
std::filesystem::path WineHostConfigDir();

// "/home/user/x" -> "Z:\home\user\x"; anything not starting with '/' is
// returned unchanged.
std::string UnixToWinePath(const std::string& unix_path);

}  // namespace recomp::achievements
