// Library paths: the Wine host candidates (fake environments; real Wine
// detection needs a Windows build under Wine) and finding prefixes.

#include <algorithm>
#include <cstdlib>
#include <map>

#include "recomp/achievements/config_dir.h"
#include "recomp/achievements/library.h"
#include "recomp/achievements/library_reader.h"
#include "test.h"

using namespace recomp::achievements;
namespace fs = std::filesystem;

namespace {

EnvLookup FakeEnv(std::map<std::string, std::string> values) {
  return [values = std::move(values)](const char* name) {
    const auto it = values.find(name);
    return it == values.end() ? std::string() : it->second;
  };
}

std::vector<std::string> Dirs(const std::vector<WineHostCandidate>& c) {
  std::vector<std::string> out;
  for (const auto& x : c) out.push_back(x.config_dir);
  return out;
}

}  // namespace

TEST(wine_unix_paths_map_to_z) {
  CHECK_EQ(UnixToWinePath("/home/player/.config"), std::string("Z:\\home\\player\\.config"));
  CHECK_EQ(UnixToWinePath("/home/player/"), std::string("Z:\\home\\player"));
  CHECK_EQ(UnixToWinePath("/"), std::string("Z:\\"));
  CHECK_EQ(UnixToWinePath("C:\\Users\\x"), std::string("C:\\Users\\x"));
}

TEST(wine_host_candidates_from_env) {
  const auto all = WineHostConfigCandidates(FakeEnv({{"XDG_CONFIG_HOME", "/home/player/.cfg"},
                                                     {"WINEHOMEDIR", "\\??\\Z:\\home\\player"},
                                                     {"HOME", "/home/player"},
                                                     {"USER", "player"},
                                                     {"LOGNAME", "other"}}));
  const std::vector<std::string> expected = {"Z:\\home\\player\\.cfg\\recomp-achievements",
                                             "Z:\\home\\player\\.config\\recomp-achievements",
                                             "Z:\\home\\other\\.config\\recomp-achievements"};
  CHECK(Dirs(all) == expected);
  CHECK_EQ(all[0].home, std::string("Z:\\home\\player\\.cfg"));
  CHECK_EQ(all[1].home, std::string("Z:\\home\\player"));

  // Proton: HOME is the host's, USER is passed through, the Windows user is steamuser.
  const auto proton = WineHostConfigCandidates(FakeEnv({{"USER", "steamuser"}, {"HOME", "/home/deck"}}));
  CHECK(Dirs(proton) == std::vector<std::string>{"Z:\\home\\deck\\.config\\recomp-achievements"});
  // Only a user name.
  const auto user = WineHostConfigCandidates(FakeEnv({{"USER", "player"}}));
  CHECK(Dirs(user) == std::vector<std::string>{"Z:\\home\\player\\.config\\recomp-achievements"});
  // Windows-style values and odd names are not host paths.
  CHECK(WineHostConfigCandidates(FakeEnv({{"HOME", "C:\\Users\\x"},
                                          {"XDG_CONFIG_HOME", "relative"},
                                          {"USER", "root"},
                                          {"LOGNAME", "a/b"}}))
            .empty());
  CHECK(WineHostConfigCandidates(FakeEnv({})).empty());
  CHECK(WineHostConfigCandidates(FakeEnv({{"WINEHOMEDIR", "Z:\\home\\p"}})).at(0).home == "Z:\\home\\p");
}

#if !defined(_WIN32)
TEST(wine_detection_is_off_natively) {
  CHECK(!RunningUnderWine());
  CHECK(WineHostConfigDir().empty());
}

TEST(library_root_defaults) {
  const fs::path dir = test::TempDir("paths_root");
  setenv("RECOMP_ACHIEVEMENTS_CONFIG_DIR", dir.c_str(), 1);
  CHECK(GlobalConfigDir() == dir);
  CHECK(DefaultLibraryRoot() == dir / "library");
  unsetenv("RECOMP_ACHIEVEMENTS_CONFIG_DIR");
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  const std::string saved = xdg ? xdg : "";
  setenv("XDG_CONFIG_HOME", "/tmp/xdg-library", 1);
  CHECK(DefaultLibraryRoot(true) == fs::path("/tmp/xdg-library/recomp-achievements/library"));
  CHECK(DefaultLibraryRoots().at(0) == fs::path("/tmp/xdg-library/recomp-achievements/library"));
  if (xdg) setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
  else unsetenv("XDG_CONFIG_HOME");
}

TEST(wine_prefix_libraries_are_found) {
  const fs::path home = test::TempDir("paths_prefixes");
  const fs::path roaming = fs::path("pfx") / "drive_c" / "users" / "steamuser" / "AppData" / "Roaming" /
                           "recomp-achievements" / "library";
  const fs::path native = home / ".local" / "share" / "Steam";
  fs::create_directories(native / "steamapps" / "compatdata" / "100" / roaming);
  fs::create_directories(native / "steamapps" / "compatdata" / "200" / "pfx");  // No library.
  // ~/.steam/steam is usually a link to the same folder.
  fs::create_directories(home / ".steam");
  fs::create_directory_symlink(native, home / ".steam" / "steam");
  // An extra Steam library on another drive.
  const fs::path extra = home / "drive2" / "SteamLibrary";
  fs::create_directories(extra / "steamapps" / "compatdata" / "300" / roaming);
  test::WriteFile(native / "steamapps" / "libraryfolders.vdf",
                  "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + native.string() +
                      "\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + extra.string() + "\"\n\t}\n}\n");
  // Flatpak Steam.
  const fs::path flatpak = home / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam";
  fs::create_directories(flatpak / "steamapps" / "compatdata" / "400" / roaming);
  // Plain Wine.
  fs::create_directories(home / ".wine" / "drive_c" / "users" / "player" / "AppData" / "Roaming" /
                         "recomp-achievements" / "library");

  const auto found = FindWinePrefixLibraries(home);
  CHECK_EQ(found.size(), size_t(4));
  const auto has = [&](const fs::path& p) { return std::find(found.begin(), found.end(), p) != found.end(); };
  CHECK(has(native / "steamapps" / "compatdata" / "100" / roaming));
  CHECK(has(extra / "steamapps" / "compatdata" / "300" / roaming));
  CHECK(has(flatpak / "steamapps" / "compatdata" / "400" / roaming));
  CHECK(has(home / ".wine" / "drive_c" / "users" / "player" / "AppData" / "Roaming" / "recomp-achievements" /
            "library"));
  CHECK(FindWinePrefixLibraries(home / "nobody").empty());
  CHECK(FindWinePrefixLibraries({}).empty());
}
#endif
