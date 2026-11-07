#pragma once

// A believable achievement library with made-up games, written through the
// real export path (System + Library + Flush), for the library app's demo,
// screenshots and tests. Icons are generated shapes, never game art.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace recomp::achievements::demo {

struct DemoGameInfo {
  std::string id;
  std::string title;
};
// The games WriteDemoLibrary makes, in no particular order.
std::vector<DemoGameInfo> DemoGames();

// Writes every demo game into `root` (the library folder). Each game "plays"
// a few sessions over the past weeks, ending `now` (unix seconds; 0 = the
// clock), so unlock times and last played are spread out. The games' own
// saves go to `saves` (empty: <root>-saves next to it). False on a write
// failure or an unknown game.
bool WriteDemoLibrary(const std::filesystem::path& root, const std::filesystem::path& saves = {}, uint64_t now = 0,
                      std::string* error = nullptr);

// One short session of an already written demo game, at the real clock, like
// a game running beside the app: loads its saves, applies the step and
// flushes the library. kind "unlock" takes an achievement key; "event" an
// event name and target (a checklist item or count).
struct DemoStep {
  std::string kind;  // "unlock" or "event".
  std::string key;   // Achievement key, or the event name.
  std::string target;
};
bool PlayDemoGame(const std::filesystem::path& root, const std::string& game_id, const std::vector<DemoStep>& steps,
                  const std::filesystem::path& saves = {}, const std::string& profile = {},
                  std::string* error = nullptr);

// Launching (docs/launching.md). With a launcher set (the demo binary), the
// library writer also makes <root>/../games/<id>/play.sh for each game, which
// runs that binary with --run, records it as how the game starts, and writes
// a Steam shortcut for starfall_odyssey to <root>/../steam-home (a $HOME for
// FindSteamShortcutFiles), so the app has one game with a Steam entry.
void SetDemoLauncher(const std::filesystem::path& demo_binary);
std::filesystem::path DemoSteamHome(const std::filesystem::path& root);

// A demo game running like a real one: running.json while it runs, one step
// every `step_seconds` (an unlock or an event, at the real clock), then
// `linger` more seconds before it exits.
bool RunDemoGame(const std::filesystem::path& root, const std::string& game_id, const std::vector<DemoStep>& steps,
                 double step_seconds, double linger, const std::filesystem::path& saves = {},
                 const std::string& profile = {}, std::string* error = nullptr);

}  // namespace recomp::achievements::demo
