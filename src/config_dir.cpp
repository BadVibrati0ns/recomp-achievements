#include "recomp/achievements/config_dir.h"

#include <cstdlib>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace recomp::achievements {

namespace {

std::filesystem::path EnvPath(const char* name) {
  const std::string value = GetEnv(name);
  return value.empty() ? std::filesystem::path() : std::filesystem::path(value);
}

}  // namespace

std::string GetEnv(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
}

std::filesystem::path GlobalConfigDir() {
  if (auto dir = EnvPath("RECOMP_ACHIEVEMENTS_CONFIG_DIR"); !dir.empty()) return dir;
#if defined(_WIN32)
  if (auto appdata = EnvPath("APPDATA"); !appdata.empty()) return appdata / "recomp-achievements";
  if (auto profile = EnvPath("USERPROFILE"); !profile.empty())
    return profile / "AppData" / "Roaming" / "recomp-achievements";
#else
  if (auto xdg = EnvPath("XDG_CONFIG_HOME"); !xdg.empty() && xdg.is_absolute()) return xdg / "recomp-achievements";
  if (auto home = EnvPath("HOME"); !home.empty()) return home / ".config" / "recomp-achievements";
#endif
  return {};
}

bool RunningUnderWine() {
#if defined(_WIN32)
  // Wine's ntdll exports this; Windows' never does.
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
  return false;
#endif
}

std::string UnixToWinePath(const std::string& unix_path) {
  if (unix_path.empty() || unix_path[0] != '/') return unix_path;
  std::string out = "Z:";
  for (const char c : unix_path) out.push_back(c == '/' ? '\\' : c);
  while (out.size() > 3 && out.back() == '\\') out.pop_back();
  return out;
}

std::vector<WineHostCandidate> WineHostConfigCandidates(const EnvLookup& env) {
  std::vector<WineHostCandidate> out;
  const auto add = [&](std::string home_dir, std::string config_dir) {
    for (const auto& c : out)
      if (c.config_dir == config_dir) return;
    out.push_back({std::move(config_dir), std::move(home_dir)});
  };
  const auto config_under = [](const std::string& home) { return home + "\\.config\\recomp-achievements"; };

  if (const std::string xdg = env("XDG_CONFIG_HOME"); !xdg.empty() && xdg[0] == '/') {
    const std::string dir = UnixToWinePath(xdg);
    add(dir, dir + "\\recomp-achievements");
  }
  if (std::string wine_home = env("WINEHOMEDIR"); !wine_home.empty()) {
    // NT form: "\??\Z:\home\user".
    if (wine_home.rfind("\\??\\", 0) == 0) wine_home.erase(0, 4);
    if (wine_home.size() > 2 && wine_home[1] == ':') add(wine_home, config_under(wine_home));
  }
  if (const std::string home = env("HOME"); !home.empty() && home[0] == '/') {
    const std::string dir = UnixToWinePath(home);
    add(dir, config_under(dir));
  }
  for (const char* name : {"USER", "LOGNAME"}) {
    const std::string user = env(name);
    // Proton's Windows user is always steamuser; that isn't the host's.
    if (user.empty() || user == "root" || user == "steamuser" ||
        user.find_first_of("/\\:") != std::string::npos) {
      continue;
    }
    const std::string dir = "Z:\\home\\" + user;
    add(dir, config_under(dir));
  }
  return out;
}

std::filesystem::path WineHostConfigDir() {
  if (!RunningUnderWine()) return {};
  for (const auto& c : WineHostConfigCandidates(GetEnv)) {
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::path(c.home), ec)) return std::filesystem::path(c.config_dir);
  }
  return {};
}

}  // namespace recomp::achievements
