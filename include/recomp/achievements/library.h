#pragma once

// The shared achievement library: every game using this SDK keeps a copy of
// its achievement sets, icons and progress in one folder for every game
// (<GlobalConfigDir()>/library), so a desktop app can show them all without
// reading any game's own files. The format is in docs/library.md; reading it
// back is library_reader.h.
//
// A Library watches a System and writes from a small worker thread: events
// are appended as they happen, snapshots are coalesced, and nothing is ever
// thrown or fatal when the folder can't be written.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/system.h"

namespace recomp::achievements {

inline constexpr std::string_view kSdkVersion = "0.1.0";
inline constexpr std::string_view kLibrarySchema = "recomp-achievements-library/1";

// An exported icon: PNG bytes, or RGBA pixels (straight alpha) the library
// encodes as PNG.
struct LibraryIcon {
  std::vector<uint8_t> png;
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;
};

// Resolves an icon ref ("xdbf:12", "trophy:TROP001.PNG", "ra:123", a file
// relative to base_dir). Called from the library's worker thread. The UI's
// IconCache provides one (ui::LibraryIconSourceFor); without a source only
// PNG files are exported.
using LibraryIconSource = std::function<bool(std::string_view ref, std::string_view set_id,
                                             const std::filesystem::path& base_dir, LibraryIcon& out)>;

struct LibraryOptions {
  // The game's folder name in the library, e.g. "my_game". Keep it stable
  // across versions. Empty: no library.
  std::string game_id;
  // Display title; empty uses the first set's title, else game_id.
  std::string title;
  std::string version;  // The game's version or build, recorded with every event.
  // The game's icon: a file path (relative to icon_base_dir) or an icon ref.
  // Empty tries the title image of the first Xbox 360 or PS3 set.
  std::string icon;
  std::filesystem::path icon_base_dir;
  // Empty: DefaultLibraryRoot(wine_host).
  std::filesystem::path root;
  bool enabled = true;
  // Store profile ids as salted hashes instead of the game's profile names.
  // The player can also force this in install.json ("hash_profiles": true).
  bool hash_profiles = false;
  // A Windows build under Wine/Proton writes to the Linux host's library
  // (through Z:) instead of the one inside the prefix, when it can find it.
  bool wine_host = true;
  LibraryIconSource icon_source;
  // Progress snapshots are written at most this often; events go out at once.
  std::chrono::milliseconds write_interval{500};
  // How often the worker checks System::generation() for resets, set
  // toggles and changed sets.
  std::chrono::milliseconds poll_interval{250};
  // events.log is renamed to events-<unix ms>.log past this size (0: never).
  uint64_t rotate_bytes = 4u << 20;
  // Unix milliseconds for event times, snapshots and last_played; null = the
  // system clock. For tests and tools that make demo data. Called from the
  // game thread and the worker.
  std::function<uint64_t()> clock;
};

class Library {
 public:
  // The System must outlive the Library. Does no I/O on the calling thread
  // beyond starting the worker.
  Library(System& system, LibraryOptions options);
  ~Library();  // Flushes.
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;

  // Writes everything pending now (sets, icons, progress, events, game.json)
  // and returns when done.
  void Flush();

  bool enabled() const;
  // <root>/<game id>; empty when disabled.
  std::filesystem::path dir() const;
  std::string install_id() const;
  // The id progress and events use for a profile (hashed or file-safe).
  std::string ProfileId(std::string_view profile) const;

  struct Stats {
    uint64_t snapshot_writes = 0;  // progress/<profile>.json
    uint64_t set_writes = 0;       // sets/*.achievements.json
    uint64_t icon_writes = 0;      // icons/*.png
    uint64_t events_written = 0;   // events.log lines
    uint64_t failures = 0;         // Writes that didn't go through.
  };
  Stats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Where a game writes its library: <RECOMP_ACHIEVEMENTS_CONFIG_DIR>/library
// when that is set, else under Wine (and wine_host) the Linux host's
// ~/.config/recomp-achievements/library, else <GlobalConfigDir()>/library.
// Empty when no folder can be found.
std::filesystem::path DefaultLibraryRoot(bool wine_host = true);

// RGBA (straight alpha, width * height * 4 bytes) as an uncompressed PNG.
// Empty on bad input.
std::vector<uint8_t> EncodePng(int width, int height, std::span<const uint8_t> rgba);

}  // namespace recomp::achievements
