#pragma once

// The library app without a window: every recomp's achievements from the
// shared library (docs/library.md), drawn in software like the in-game
// viewer. A games grid with totals and recent unlocks, and a game page that
// is the overlay's viewer fed from the library, and a Play menu that starts
// games through Steam or directly (launch.h). Reads the library only; writes
// nothing but the player's launch choices (launcher.json).
// main.cpp puts it in an SDL3 window, or renders scripted input headless.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "recomp/achievements/app/library_view.h"
#include "recomp/achievements/launch.h"
#include "recomp/achievements/ui/overlay.h"

namespace recomp::achievements::app {

struct AppOptions {
  // Library folders to show. Empty: DefaultLibraryRoots() (the host library
  // and Wine/Proton prefixes), looked for again now and then.
  std::vector<std::filesystem::path> roots;
  // On top of the window's scale (height / 1080, but at least 1 at 800
  // lines) and the player's viewer.scale.
  float scale = 1.0f;
  // The player's achievements_style.json (colours, labels, viewer scale).
  bool global_preferences = true;
  // Unix seconds, for "2 hours ago"; null = the clock.
  std::function<uint64_t()> now;
  float poll_seconds = 1.0f;
  // Popups for unlocks and progress noticed while the app runs.
  bool popups = true;
  // Launching games (docs/launching.md). Starts a command; null = StartDetached.
  std::function<bool(const LaunchCommand& command, std::string* error)> start;
  // Steam shortcuts, read each time the Play menu opens so one added since
  // the game ran is found; null = every shortcuts.vdf (FindSteamShortcutFiles).
  std::function<std::vector<SteamShortcut>()> shortcuts;
  // The player's launch choices; empty = DefaultLauncherPrefsFile().
  std::filesystem::path launcher_prefs;

  // Inside a game (the viewer's All games page, docs/overlay.md).
  // The game this runs in: its card says Playing, and Play on it says so.
  std::string current_game;
  // B / Esc on the grid goes back to the game's own viewer. Null: nothing.
  std::function<void()> on_back;
  // Set: the Play menu offers "Quit current game" (ticked as the player last
  // left it, launcher.json), and this is called once another game has started
  // with it ticked. The game should save and exit.
  std::function<void()> quit_current;
};

class App {
 public:
  explicit App(AppOptions options = {});
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  void SetSize(int width, int height);
  int width() const;
  int height() const;
  // Input, library polling, animation; true when image() changed.
  bool Frame(float dt, const ui::OverlayInput& input);
  // Opaque RGBA, the window's size.
  const ui::Image& image() const;
  // The part of image() the last Frame() that returned true changed.
  ui::PixelRect dirty_rect() const;
  void SetButtonLabels(ui::ButtonLabels labels);
  // AppOptions::scale, changed while running.
  void SetScale(float scale);
  // F11, or Back on the grid, asked to toggle fullscreen since the last call.
  bool TakeFullscreenToggle();
  // Buttons and keys held now (the press that opened this) stay ignored until
  // they're let go.
  void HoldInput(const ui::OverlayInput& held);
  // Looks at the library now instead of at the next poll; true when it changed.
  bool Poll();

  struct State {
    bool game_page = false;
    SortMode sort = SortMode::kLastPlayed;
    std::vector<GameSummary> games;  // As shown, in order.
    std::string selected;            // Game id.
    float scroll = 0.0f;
    std::vector<std::string> recent;  // "<game id>/<key>", newest first.
    // The game page.
    std::string game;
    std::string profile;
    std::vector<std::string> profiles;
    ui::Overlay::ViewState page;
    // The Play menu.
    bool launch_menu = false;
    std::string launch_game;
    std::string launch_choice;  // "steam", "direct", or empty when neither can start it.
    std::string launch_error;
    bool quit_box = false;      // The menu offers "Quit current game".
    bool quit_current = false;  // ... ticked.
    std::vector<std::string> running;   // Game ids running now.
    std::vector<std::string> starting;  // Started from here, not running yet.
    std::vector<std::string> launched;  // Commands started, oldest first: argv (or the URL) joined by spaces.
    uint64_t reloads = 0;  // Times the library was read again.
    uint64_t popups = 0;   // Popups queued for changes seen.
  };
  State state() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace recomp::achievements::app
