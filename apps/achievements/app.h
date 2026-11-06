#pragma once

// The library app without a window: every recomp's achievements from the
// shared library (docs/library.md), drawn in software like the in-game
// viewer. A games grid with totals and recent unlocks, and a game page that
// is the overlay's viewer fed from the library. Reads only; never writes.
// main.cpp puts it in an SDL3 window, or renders scripted input headless.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "library_view.h"
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
    uint64_t reloads = 0;  // Times the library was read again.
    uint64_t popups = 0;   // Popups queued for changes seen.
  };
  State state() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace recomp::achievements::app
