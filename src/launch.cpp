#include "recomp/achievements/launch.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

#include "library_util.h"
#include "recomp/achievements/config_dir.h"
#include "recomp/achievements/library_reader.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

namespace recomp::achievements {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::string_view kPrefsSchema = "recomp-achievements-launcher/1";

const char* ThisOs() {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

std::string Str(const json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}
uint64_t U64(const json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_number_unsigned() ? it->get<uint64_t>() : 0;
}
bool Bool(const json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_boolean() && it->get<bool>();
}

std::string ReadSmallFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::string s;
  char buf[4096];
  while (in.read(buf, sizeof buf) || in.gcount() > 0) {
    s.append(buf, static_cast<size_t>(in.gcount()));
    if (s.size() > (1u << 20)) break;
  }
  return s;
}

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

// /proc/<pid>/stat: the pid (field 1) and start time (field 22). The command
// name in field 2 may hold spaces and parentheses, so fields are counted from
// the last ')'.
bool ParseStat(const std::string& stat, uint64_t& pid, uint64_t& start) {
  const size_t close = stat.rfind(')');
  if (close == std::string::npos) return false;
  pid = std::strtoull(stat.c_str(), nullptr, 10);
  std::istringstream rest(stat.substr(close + 1));
  std::string field;
  for (int i = 3; i <= 22; ++i) {
    if (!(rest >> field)) return false;
  }
  start = std::strtoull(field.c_str(), nullptr, 10);
  return pid != 0;
}

std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

std::string Normal(const std::string& path) {
  if (path.empty()) return {};
  std::error_code ec;
  fs::path p = fs::weakly_canonical(fs::path(path), ec);
  if (ec) p = fs::path(path).lexically_normal();
  std::string s = p.generic_string();
  while (s.size() > 1 && s.back() == '/') s.pop_back();
#if defined(_WIN32)
  s = Lower(s);
#endif
  return s;
}

#if defined(_WIN32)
std::string Utf8(const wchar_t* w, int n = -1) {
  if (!w) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
  if (len <= 0) return {};
  std::string s(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
  if (n < 0 && !s.empty()) s.pop_back();
  return s;
}
std::wstring Wide(const std::string& s) {
  const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (len <= 0) return {};
  std::wstring w(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
  w.pop_back();
  return w;
}

// Wine's kernel32 turns a Windows path into the host's.
std::string WineUnixPath(const std::wstring& windows_path) {
  using Fn = char*(__cdecl*)(const wchar_t*);
  const HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  const auto fn = k32 ? reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(k32, "wine_get_unix_file_name")))
                      : nullptr;
  if (!fn) return {};
  char* unix_path = fn(windows_path.c_str());
  if (!unix_path) return {};
  std::string out(unix_path);
  HeapFree(GetProcessHeap(), 0, unix_path);
  return out;
}

// A host file under Wine: Z: maps /, and \\?\unix\ works on newer Wine.
std::string ReadHostFile(const std::string& unix_path) {
  std::string s = ReadSmallFile(fs::path(Wide(UnixToWinePath(unix_path))));
  if (s.empty()) {
    std::string p = "\\\\?\\unix" + unix_path;
    std::replace(p.begin() + 8, p.end(), '/', '\\');
    s = ReadSmallFile(fs::path(Wide(p)));
  }
  return s;
}

uint64_t FileTimeU64(const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
#endif

#if defined(__APPLE__)
uint64_t MacStart(pid_t pid) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid)};
  struct kinfo_proc info {};
  size_t size = sizeof info;
  if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) return 0;
  return static_cast<uint64_t>(info.kp_proc.p_starttime.tv_sec) * 1000000u +
         static_cast<uint64_t>(info.kp_proc.p_starttime.tv_usec);
}
#endif

std::string SteamIdFromEnv() {
  std::string id = GetEnv("SteamGameId");
  if (id.empty() || id == "0") id = GetEnv("SteamAppId");
  if (id.empty() || id == "0") return {};
  for (const char c : id)
    if (c < '0' || c > '9') return {};
  if (id == GetEnv("RECOMP_ACHIEVEMENTS_PARENT_STEAM_ID")) return {};
  return id;
}

std::vector<std::string> SplitLines(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out.push_back(line);
  }
  return out;
}

// A program on PATH; empty when not found.
std::string FindOnPath(const std::string& name) {
  if (name.find('/') != std::string::npos) return name;
  const std::string path = GetEnv("PATH");
#if defined(_WIN32)
  const char sep = ';';
#else
  const char sep = ':';
#endif
  size_t at = 0;
  while (at <= path.size()) {
    const size_t end = std::min(path.find(sep, at), path.size());
    const std::string dir = path.substr(at, end - at);
    at = end + 1;
    if (dir.empty()) continue;
    std::error_code ec;
    const fs::path candidate = fs::path(dir) / name;
    if (fs::is_regular_file(candidate, ec)) {
#if defined(_WIN32)
      return candidate.string();
#else
      if (access(candidate.c_str(), X_OK) == 0) return candidate.string();
#endif
    }
  }
  return {};
}

// ---- Binary VDF (shortcuts.vdf) ------------------------------------------------

struct VdfReader {
  const std::vector<uint8_t>& b;
  size_t at = 0;
  bool ok = true;

  bool CString(std::string& out) {
    out.clear();
    while (at < b.size() && b[at]) out.push_back(static_cast<char>(b[at++]));
    if (at >= b.size()) return ok = false;
    ++at;
    return true;
  }
  bool Skip(size_t n) {
    if (b.size() - at < n) return ok = false;
    at += n;
    return true;
  }
  uint32_t U32() {
    if (b.size() - at < 4) return ok = false, 0;
    const uint32_t v = b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
    at += 4;
    return v;
  }

  // Reads one map's entries until its end byte. A shortcut's own map is
  // passed `fields`; deeper maps (tags) are skipped.
  void Map(int depth, std::map<std::string, std::string>* fields, std::vector<std::map<std::string, std::string>>* items) {
    while (ok && at < b.size()) {
      const uint8_t type = b[at++];
      if (type == 0x08) return;
      std::string key;
      if (!CString(key)) return;
      switch (type) {
        case 0x00: {
          if (depth == 1 && items) {
            std::map<std::string, std::string> item;
            Map(depth + 1, &item, nullptr);
            items->push_back(std::move(item));
          } else {
            // The root's "shortcuts" map holds the shortcuts.
            Map(depth + 1, nullptr, depth == 0 && Lower(key) == "shortcuts" ? items : nullptr);
          }
          break;
        }
        case 0x01: {
          std::string v;
          if (!CString(v)) return;
          if (fields) (*fields)[Lower(key)] = v;
          break;
        }
        case 0x02: {
          const uint32_t v = U32();
          if (fields) (*fields)[Lower(key)] = std::to_string(v);
          break;
        }
        case 0x03: Skip(4); break;  // float
        case 0x07: Skip(8); break;  // uint64
        default: ok = false; return;
      }
    }
  }
};

}  // namespace

// ---- Launch info --------------------------------------------------------------

json LaunchToJson(const LaunchInfo& info) {
  json j = {{"os", info.os}, {"exe", info.exe}, {"args", info.args}, {"cwd", info.cwd}};
  if (!info.steam_game_id.empty()) j["steam_game_id"] = info.steam_game_id;
  if (info.wine) j["wine"] = true;
  if (!info.wine_prefix.empty()) j["wine_prefix"] = info.wine_prefix;
  if (info.proton) j["proton"] = true;
  return j;
}

LaunchInfo LaunchFromJson(const json& j) {
  LaunchInfo info;
  if (!j.is_object()) return info;
  info.os = Str(j, "os");
  info.exe = Str(j, "exe");
  info.cwd = Str(j, "cwd");
  if (const auto it = j.find("args"); it != j.end() && it->is_array())
    for (const auto& a : *it)
      if (a.is_string()) info.args.push_back(a.get<std::string>());
  info.steam_game_id = Str(j, "steam_game_id");
  info.wine = Bool(j, "wine");
  info.wine_prefix = Str(j, "wine_prefix");
  info.proton = Bool(j, "proton");
  return info;
}

LaunchInfo CurrentLaunchInfo() {
  LaunchInfo info;
  info.os = ThisOs();
  std::error_code ec;
#if defined(_WIN32)
  std::wstring exe(32768, L'\0');
  exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
  std::wstring cwd(32768, L'\0');
  cwd.resize(GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data()));
  int argc = 0;
  if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    for (int i = 1; i < argc; ++i) info.args.push_back(Utf8(argv[i]));
    LocalFree(argv);
  }
  info.wine = RunningUnderWine();
  if (info.wine) {
    info.exe = WineUnixPath(exe);
    info.cwd = WineUnixPath(cwd);
    info.wine_prefix = GetEnv("WINEPREFIX");
    const std::string compat = GetEnv("STEAM_COMPAT_DATA_PATH");
    info.proton = !compat.empty();
    if (info.wine_prefix.empty() && info.proton) info.wine_prefix = compat + "/pfx";
  }
  if (info.exe.empty()) info.exe = Utf8(exe.c_str());
  if (info.cwd.empty()) info.cwd = Utf8(cwd.c_str());
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string exe(size, '\0');
  if (_NSGetExecutablePath(exe.data(), &size) == 0) {
    exe.resize(std::strlen(exe.c_str()));
    info.exe = Normal(exe);
  }
  info.cwd = fs::current_path(ec).string();
#else
  info.exe = fs::read_symlink("/proc/self/exe", ec).string();
  const std::string cmdline = ReadSmallFile("/proc/self/cmdline");
  size_t at = cmdline.find('\0');
  while (at != std::string::npos && at + 1 < cmdline.size()) {
    const size_t end = cmdline.find('\0', at + 1);
    info.args.push_back(cmdline.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1));
    at = end;
  }
  info.cwd = fs::current_path(ec).string();
#endif
  if (const std::string over = GetEnv("RECOMP_ACHIEVEMENTS_LAUNCH_EXE"); !over.empty()) {
    info.exe = over;
    info.args = SplitLines(GetEnv("RECOMP_ACHIEVEMENTS_LAUNCH_ARGS"));
  }
  info.steam_game_id = SteamIdFromEnv();
  return info;
}

// ---- Running marker ----------------------------------------------------------

json MarkerToJson(const RunningMarker& m) {
  json j = {{"pid", m.pid}, {"start", m.start}, {"os", m.os}, {"started", m.started}};
  if (!m.boot_id.empty()) j["boot_id"] = m.boot_id;
  return j;
}

std::optional<RunningMarker> MarkerFromJson(const json& j) {
  if (!j.is_object()) return std::nullopt;
  RunningMarker m;
  m.pid = U64(j, "pid");
  m.start = U64(j, "start");
  m.boot_id = Str(j, "boot_id");
  m.os = Str(j, "os");
  m.started = U64(j, "started");
  if (!m.pid || m.os.empty()) return std::nullopt;
  return m;
}

std::optional<RunningMarker> ReadRunningMarker(const fs::path& game_dir) {
  return MarkerFromJson(detail::ReadJsonFile(game_dir / "running.json"));
}

std::optional<RunningMarker> CurrentRunningMarker() {
  RunningMarker m;
  m.started = detail::UnixMillis() / 1000;
#if defined(_WIN32)
  if (RunningUnderWine()) {
    // This Windows process is a Unix process on the host; its /proc entry is
    // what an app there can check.
    if (!ParseStat(ReadHostFile("/proc/self/stat"), m.pid, m.start)) return std::nullopt;
    m.boot_id = Trim(ReadHostFile("/proc/sys/kernel/random/boot_id"));
    m.os = "linux";
    return m;
  }
  FILETIME created, exited, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return std::nullopt;
  m.pid = GetCurrentProcessId();
  m.start = FileTimeU64(created);
  m.os = "windows";
#elif defined(__APPLE__)
  m.pid = static_cast<uint64_t>(getpid());
  m.start = MacStart(getpid());
  m.os = "macos";
#else
  if (!ParseStat(ReadSmallFile("/proc/self/stat"), m.pid, m.start)) return std::nullopt;
  m.boot_id = Trim(ReadSmallFile("/proc/sys/kernel/random/boot_id"));
  m.os = "linux";
#endif
  return m;
}

bool MarkerAlive(const RunningMarker& m) {
  if (!m.pid) return false;
#if defined(_WIN32)
  if (m.os == "linux") {
    if (!RunningUnderWine()) return false;
    if (!m.boot_id.empty() && Trim(ReadHostFile("/proc/sys/kernel/random/boot_id")) != m.boot_id) return false;
    uint64_t pid = 0, start = 0;
    return ParseStat(ReadHostFile("/proc/" + std::to_string(m.pid) + "/stat"), pid, start) && start == m.start;
  }
  if (m.os != "windows") return false;
  const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(m.pid));
  if (!h) return false;
  FILETIME created, exited, kernel, user;
  DWORD code = 0;
  const bool alive = GetProcessTimes(h, &created, &exited, &kernel, &user) && FileTimeU64(created) == m.start &&
                     GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
#elif defined(__APPLE__)
  if (m.os != "macos") return false;
  if (kill(static_cast<pid_t>(m.pid), 0) != 0 && errno != EPERM) return false;
  return !m.start || MacStart(static_cast<pid_t>(m.pid)) == m.start;
#else
  if (m.os != "linux") return false;
  if (!m.boot_id.empty() && Trim(ReadSmallFile("/proc/sys/kernel/random/boot_id")) != m.boot_id) return false;
  uint64_t pid = 0, start = 0;
  if (!ParseStat(ReadSmallFile("/proc/" + std::to_string(m.pid) + "/stat"), pid, start)) return false;
  // A zombie has exited; anything else (running, sleeping, stopped) counts.
  const std::string stat = ReadSmallFile("/proc/" + std::to_string(m.pid) + "/stat");
  const size_t close = stat.rfind(')');
  if (close != std::string::npos && close + 2 < stat.size() && (stat[close + 2] == 'Z' || stat[close + 2] == 'X'))
    return false;
  return start == m.start;
#endif
}

// ---- Steam --------------------------------------------------------------------

std::vector<SteamShortcut> ParseSteamShortcuts(const std::vector<uint8_t>& bytes) {
  VdfReader r{bytes};
  std::vector<std::map<std::string, std::string>> items;
  r.Map(0, nullptr, &items);
  std::vector<SteamShortcut> out;
  if (!r.ok) return out;  // Cut short or not a shortcuts file.
  for (auto& f : items) {
    SteamShortcut s;
    s.name = f["appname"];
    s.exe = f["exe"];
    s.start_dir = f["startdir"];
    s.launch_options = f["launchoptions"];
    if (const auto it = f.find("appid"); it != f.end()) {
      s.appid = static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10));
    } else {
      // Older files: Steam derives it from the executable and name.
      const std::string seed = s.exe + s.name;
      s.appid = detail::Crc32({reinterpret_cast<const uint8_t*>(seed.data()), seed.size()}) | 0x80000000u;
    }
    if (!s.exe.empty()) out.push_back(std::move(s));
  }
  return out;
}

std::vector<SteamShortcut> ReadSteamShortcuts(const fs::path& vdf) {
  std::ifstream in(vdf, std::ios::binary);
  if (!in) return {};
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto out = ParseSteamShortcuts(bytes);
  for (auto& s : out) s.file = vdf;
  return out;
}

std::vector<fs::path> FindSteamShortcutFiles(const fs::path& home) {
  std::vector<fs::path> steams;
#if defined(_WIN32)
  (void)home;
  wchar_t buf[MAX_PATH];
  DWORD size = sizeof buf;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) ==
      ERROR_SUCCESS)
    steams.emplace_back(buf);
  steams.emplace_back(L"C:\\Program Files (x86)\\Steam");
#else
  if (home.empty()) return {};
  steams = {home / ".local" / "share" / "Steam", home / ".steam" / "steam", home / ".steam" / "root",
            home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam",
            home / ".var" / "app" / "com.valvesoftware.Steam" / "data" / "Steam"};
#if defined(__APPLE__)
  steams.push_back(home / "Library" / "Application Support" / "Steam");
#endif
#endif
  std::vector<fs::path> out;
  std::set<fs::path> seen;
  std::error_code ec;
  for (const auto& steam : steams) {
    fs::path canonical = fs::weakly_canonical(steam, ec);
    if (ec || !fs::is_directory(canonical, ec) || !seen.insert(canonical).second) continue;
    std::vector<fs::path> users;
    for (const auto& e : fs::directory_iterator(canonical / "userdata", ec)) users.push_back(e.path());
    std::sort(users.begin(), users.end());
    for (const auto& u : users)
      if (fs::is_regular_file(u / "config" / "shortcuts.vdf", ec)) out.push_back(u / "config" / "shortcuts.vdf");
  }
  return out;
}

std::string UnquoteExe(std::string exe) {
  while (!exe.empty() && exe.front() == ' ') exe.erase(exe.begin());
  if (exe.size() >= 2 && exe.front() == '"') {
    const size_t close = exe.find('"', 1);
    if (close != std::string::npos) return exe.substr(1, close - 1);
  }
  return Trim(exe);
}

const SteamShortcut* MatchShortcut(const LaunchInfo& info, const std::vector<SteamShortcut>& shortcuts) {
  if (info.exe.empty()) return nullptr;
  const std::string exe = Normal(info.exe);
  const std::string exe_dir = Normal(fs::path(info.exe).parent_path().string());
  const std::string cwd = Normal(info.cwd);
  // Best first: the same executable; a wrapper that names it in its options;
  // a wrapper in the game's folder that starts there.
  for (const auto& s : shortcuts)
    if (Normal(UnquoteExe(s.exe)) == exe) return &s;
  for (const auto& s : shortcuts)
    if (s.launch_options.find(info.exe) != std::string::npos) return &s;
  for (const auto& s : shortcuts) {
    const std::string start = Normal(UnquoteExe(s.start_dir));
    const std::string dir = Normal(fs::path(UnquoteExe(s.exe)).parent_path().string());
    if (!start.empty() && (start == exe_dir || start == cwd) && (dir == exe_dir || dir == cwd)) return &s;
  }
  return nullptr;
}

// ---- Launching ------------------------------------------------------------------

const char* LaunchMethodName(LaunchMethod method) { return method == LaunchMethod::kSteam ? "steam" : "direct"; }

std::optional<LaunchMethod> ParseLaunchMethod(const std::string& name) {
  if (name == "steam") return LaunchMethod::kSteam;
  if (name == "direct") return LaunchMethod::kDirect;
  return std::nullopt;
}

LaunchPlan PlanLaunch(const LibraryGame& game, const std::vector<SteamShortcut>& shortcuts) {
  LaunchPlan plan;
  plan.info = game.launch;
  plan.running = game.running && MarkerAlive(*game.running);
  const LaunchInfo& info = plan.info;

  if (!info.steam_game_id.empty()) {
    const uint64_t id = std::strtoull(info.steam_game_id.c_str(), nullptr, 10);
    bool keep = id != 0;
    // A shortcut's id only lasts while the shortcut does.
    if (keep && (id & 0x02000000u) && !shortcuts.empty())
      keep = std::any_of(shortcuts.begin(), shortcuts.end(), [&](const SteamShortcut& s) { return s.game_id() == id; });
    if (keep) plan.steam_game_id = info.steam_game_id, plan.steam_source = "recorded";
  }
  if (plan.steam_game_id.empty())
    if (const SteamShortcut* s = MatchShortcut(info, shortcuts)) {
      plan.steam_game_id = std::to_string(s->game_id());
      plan.steam_source = "shortcut";
    }

  std::error_code ec;
  if (info.empty()) {
    plan.direct_note = "Run the game once with this version so it can be started from here.";
  } else if (info.os == ThisOs() && !info.wine) {
    plan.direct = fs::exists(info.exe, ec);
    plan.direct_note = plan.direct ? info.exe : "Not found: " + info.exe;
  } else if (info.wine && std::string(ThisOs()) != "windows") {
    if (info.proton) {
      plan.direct_note = "Proton games start through Steam.";
    } else if (FindOnPath("wine").empty()) {
      plan.direct_note = "Needs Wine, which isn't installed.";
    } else {
      plan.direct = fs::exists(info.exe, ec);
      plan.direct_note = plan.direct ? "wine " + info.exe : "Not found: " + info.exe;
    }
  } else {
    plan.direct_note = "Built for " + info.os + ".";
  }
  return plan;
}

std::optional<LaunchCommand> BuildLaunch(const LaunchPlan& plan, LaunchMethod method, std::string* error) {
  const auto fail = [&](const std::string& why) -> std::optional<LaunchCommand> {
    if (error) *error = why;
    return std::nullopt;
  };
  LaunchCommand c;
  if (method == LaunchMethod::kSteam) {
    if (plan.steam_game_id.empty()) return fail("No Steam entry for this game.");
    const std::string url = "steam://rungameid/" + plan.steam_game_id;
#if defined(_WIN32)
    c.url = url;
#elif defined(__APPLE__)
    c.argv = {"open", url};
#else
    // A running Steam takes the URL from a second `steam` and that one exits.
    std::error_code ec;
    const std::string home = GetEnv("HOME");
    if (!FindOnPath("steam").empty())
      c.argv = {"steam", url};
    else if (!FindOnPath("flatpak").empty() && !home.empty() &&
             fs::is_directory(fs::path(home) / ".var" / "app" / "com.valvesoftware.Steam", ec))
      c.argv = {"flatpak", "run", "com.valvesoftware.Steam", url};
    else
      c.argv = {"xdg-open", url};
#endif
    return c;
  }
  if (!plan.direct) return fail(plan.direct_note.empty() ? "Can't start this game directly." : plan.direct_note);
  const LaunchInfo& info = plan.info;
  if (info.wine) {
    c.argv = {"wine", info.exe};
    if (!info.wine_prefix.empty()) c.env["WINEPREFIX"] = info.wine_prefix;
  } else {
    c.argv = {info.exe};
  }
  c.argv.insert(c.argv.end(), info.args.begin(), info.args.end());
  c.cwd = info.cwd;
  // Started from inside an app's own Steam session, the game inherits the
  // app's Steam id; this tells it not to record that id as its own.
  if (const std::string own = GetEnv("SteamGameId"); !own.empty()) c.env["RECOMP_ACHIEVEMENTS_PARENT_STEAM_ID"] = own;
  return c;
}

bool StartDetached(const LaunchCommand& command, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return false;
  };
#if defined(_WIN32)
  if (!command.url.empty()) {
    const auto r = reinterpret_cast<intptr_t>(
        ShellExecuteW(nullptr, L"open", Wide(command.url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return r > 32 ? true : fail("Couldn't open " + command.url);
  }
  if (command.argv.empty()) return fail("Nothing to run.");
  std::wstring line;
  for (const auto& a : command.argv) {
    if (!line.empty()) line += L' ';
    const std::wstring w = Wide(a);
    if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) {
      line += w;
      continue;
    }
    line += L'"';
    size_t slashes = 0;
    for (const wchar_t ch : w) {
      if (ch == L'\\') {
        ++slashes;
        continue;
      }
      line.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
      slashes = 0;
      line += ch;
    }
    line.append(slashes * 2, L'\\');
    line += L'"';
  }
  std::wstring env_block;
  if (!command.env.empty()) {
    std::map<std::wstring, std::wstring> vars;
    if (wchar_t* env = GetEnvironmentStringsW()) {
      for (const wchar_t* p = env; *p; p += wcslen(p) + 1) {
        const std::wstring kv = p;
        const size_t eq = kv.find(L'=', 1);
        if (eq != std::wstring::npos) vars[kv.substr(0, eq)] = kv.substr(eq + 1);
      }
      FreeEnvironmentStringsW(env);
    }
    for (const auto& [k, v] : command.env) vars[Wide(k)] = Wide(v);
    for (const auto& [k, v] : vars) env_block += k + L'=' + v + L'\0';
    env_block += L'\0';
  }
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  const std::wstring cwd = Wide(command.cwd);
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                      DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT,
                      env_block.empty() ? nullptr : env_block.data(), cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
    return fail("Couldn't start " + command.argv[0] + " (error " + std::to_string(GetLastError()) + ")");
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  if (command.argv.empty()) return fail("Nothing to run.");
  const std::string program = FindOnPath(command.argv[0]);
  if (program.empty()) return fail(command.argv[0] + " isn't installed.");

  // Everything the child needs is made here: after fork only exec-safe calls.
  std::vector<std::string> env_strings;
  for (char** e = environ; e && *e; ++e) {
    const std::string kv = *e;
    const std::string key = kv.substr(0, kv.find('='));
    if (!command.env.count(key)) env_strings.push_back(kv);
  }
  for (const auto& [k, v] : command.env) env_strings.push_back(k + "=" + v);
  std::vector<char*> envp, argv;
  for (auto& s : env_strings) envp.push_back(s.data());
  envp.push_back(nullptr);
  std::vector<std::string> args = command.argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  int fds[2];
  if (pipe(fds) != 0) return fail(std::strerror(errno));
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  const pid_t child = fork();
  if (child < 0) {
    close(fds[0]);
    close(fds[1]);
    return fail(std::strerror(errno));
  }
  if (child == 0) {
    // Its own session, then a second fork so the game is nobody's child and
    // outlives the app.
    setsid();
    if (fork() != 0) _exit(0);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    if (!command.cwd.empty() && chdir(command.cwd.c_str()) != 0) {
      const int e = errno;
      (void)!write(fds[1], &e, sizeof e);
      _exit(127);
    }
    execve(program.c_str(), argv.data(), envp.data());
    const int e = errno;
    (void)!write(fds[1], &e, sizeof e);
    _exit(127);
  }
  close(fds[1]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  int child_errno = 0;
  ssize_t n;
  while ((n = read(fds[0], &child_errno, sizeof child_errno)) < 0 && errno == EINTR) {
  }
  close(fds[0]);
  if (n == static_cast<ssize_t>(sizeof child_errno))
    return fail("Couldn't start " + command.argv[0] + ": " + std::strerror(child_errno));
  return true;
#endif
}

// ---- Preferences ----------------------------------------------------------------

LauncherPrefs LoadLauncherPrefs(const fs::path& file) {
  LauncherPrefs p;
  const json doc = detail::ReadJsonFile(file);
  if (!doc.is_object()) return p;
  if (const auto it = doc.find("quit_current"); it != doc.end() && it->is_boolean()) p.quit_current = it->get<bool>();
  if (const auto games = doc.find("games"); games != doc.end() && games->is_object())
    for (const auto& [id, g] : games->items())
      if (g.is_object())
        if (const auto m = ParseLaunchMethod(Str(g, "method"))) p.methods[id] = *m;
  return p;
}

bool SaveLauncherPrefs(const fs::path& file, const LauncherPrefs& prefs) {
  if (file.empty()) return false;
  // Unknown keys and other games' entries are kept; ours go first.
  const json old = detail::ReadJsonFile(file);
  nlohmann::ordered_json doc = {{"schema", kPrefsSchema}, {"quit_current", prefs.quit_current}};
  nlohmann::ordered_json games = nlohmann::ordered_json::object();
  if (old.is_object() && old.contains("games") && old["games"].is_object())
    games = nlohmann::ordered_json::parse(old["games"].dump());
  for (const auto& [id, m] : prefs.methods) games[id]["method"] = LaunchMethodName(m);
  doc["games"] = std::move(games);
  if (old.is_object())
    for (const auto& [k, v] : old.items())
      if (!doc.contains(k)) doc[k] = nlohmann::ordered_json::parse(v.dump());
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  return detail::WriteJsonAtomic(file, doc);
}

fs::path DefaultLauncherPrefsFile() {
  const fs::path dir = GlobalConfigDir();
  return dir.empty() ? fs::path() : dir / "launcher.json";
}

}  // namespace recomp::achievements
