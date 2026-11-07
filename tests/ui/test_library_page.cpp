// The viewer's All games page: the tab after the sets, the library app inside
// the game, going back, the toggle closing it, and "Quit current game".

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>

#include "recomp/achievements/app/app.h"
#include "recomp/achievements/library.h"
#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/overlay.h"
#include "test.h"

using namespace recomp::achievements;
using namespace recomp::achievements::ui;
namespace fs = std::filesystem;

namespace {

fs::path Fresh(const char* name) {
  auto dir = fs::temp_directory_path() / "recomp_ach_ui_tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

AchievementSet OneSet(const std::string& id, const std::string& title) {
  AchievementSet s;
  s.id = id;
  s.title = title;
  for (int i = 0; i < 3; ++i) {
    Achievement a;
    a.local_id = "a" + std::to_string(i);
    a.name = title + " " + std::to_string(i);
    a.points = 10;
    s.achievements.push_back(a);
  }
  return s;
}

// Another game that has run before, with a launch it can be started by.
void WriteOtherGame(const fs::path& root) {
  System system;
  system.AddSet(OneSet("other", "Other Game"));
  system.Unlock("other:a0");
  LibraryOptions o;
  o.game_id = "other_game";
  o.root = root;
  o.launch.exe = (root / "other.sh").string();
  std::ofstream(root / "other.sh") << "#!/bin/sh\n";
  Library library(system, o);
  library.Flush();
}

struct Rig {
  System system;
  std::unique_ptr<Overlay> overlay;
  std::vector<LaunchCommand> started;
  int quits = 0;

  explicit Rig(const fs::path& root, bool offer_quit = true) {
    WriteOtherGame(root);
    system.AddSet(OneSet("mine", "This Game"));
    OverlayOptions o;
    o.global_preferences = false;
    o.library.game_id = "this_game";
    o.library.root = root;
    o.library_roots = {root};
    o.launcher_prefs = root.parent_path() / "launcher.json";
    o.steam_shortcuts = [] { return std::vector<SteamShortcut>{}; };
    o.start_game = [this](const LaunchCommand& c, std::string*) {
      started.push_back(c);
      return true;
    };
    if (offer_quit) o.on_quit_request = [this] { ++quits; };
    overlay = std::make_unique<Overlay>(system, std::move(o));
    overlay->SetSize(1280, 800);
    Step();
  }
  bool Step(const OverlayInput& in = {}) { return overlay->Frame(1.0f / 30, in); }
  void Tap(const std::function<void(OverlayInput&)>& set) {
    OverlayInput in;
    set(in);
    Step(in);
    Step();
  }
  // Moves the grid's selection to a game (cards are in one row here).
  void SelectGame(const std::string& id) {
    for (int i = 0; i < 3 && Page().selected != id; ++i) Tap([](OverlayInput& in) { in.pad_right = true; });
    for (int i = 0; i < 3 && Page().selected != id; ++i) Tap([](OverlayInput& in) { in.pad_left = true; });
  }
  Overlay::ViewState V() const { return overlay->view_state(); }
  app::App::State Page() const {
    app::App* p = overlay->library_page();
    return p ? p->state() : app::App::State{};
  }
};

}  // namespace

TEST(library_page_is_the_last_tab) {
  Rig r(Fresh("page_tab"));
  r.Tap([](OverlayInput& in) { in.f[8] = true; });
  CHECK(r.overlay->is_open());
  const auto tabs = r.V().tabs;
  CHECK(!tabs.empty() && tabs.back() == ":library");
  CHECK(!r.V().library_page);
  // RB past the game's set opens it.
  r.Tap([](OverlayInput& in) { in.pad_rb = true; });
  r.Step();
  CHECK(r.V().library_page);
  CHECK(r.overlay->library_page() != nullptr);
  CHECK(r.overlay->wants_input());
  // Both games, and this one marked as being played.
  const auto page = r.Page();
  CHECK_EQ(page.games.size(), size_t(2));
  CHECK(r.overlay->image().width == 1280);
  // The RB that opened it didn't change the sort.
  CHECK(page.sort == app::SortMode::kLastPlayed);

  // B goes back to the viewer, on the same tab.
  const std::string tab = r.V().tab;
  r.Tap([](OverlayInput& in) { in.pad_b = true; });
  CHECK(!r.V().library_page);
  CHECK(r.overlay->is_open());
  CHECK_EQ(r.V().tab, tab);
  // And B again closes the viewer: the first B didn't leak through.
  r.Tap([](OverlayInput& in) { in.pad_b = true; });
  CHECK(!r.overlay->is_open());
}

TEST(library_page_toggle_closes_everything) {
  Rig r(Fresh("page_toggle"));
  CHECK(r.overlay->OpenLibraryPage());
  r.Step();
  CHECK(r.V().library_page);
  r.Tap([](OverlayInput& in) { in.f[8] = true; });
  CHECK(!r.overlay->is_open());
  CHECK(!r.V().library_page);
  for (int i = 0; i < 30; ++i) r.Step();
  CHECK(!r.overlay->visible());
}

TEST(library_page_quit_current_game) {
  Rig r(Fresh("page_quit"));
  CHECK(r.overlay->OpenLibraryPage());
  r.Step();
  // The other game: Play, the box is offered and ticked by default.
  r.SelectGame("other_game");
  CHECK_EQ(r.Page().selected, std::string("other_game"));
  r.Tap([](OverlayInput& in) { in.pad_y = true; });
  CHECK(r.Page().launch_menu);
  CHECK(r.Page().quit_box);
  CHECK(r.Page().quit_current);
  // X unticks it; starting then leaves this game running.
  r.Tap([](OverlayInput& in) { in.pad_x = true; });
  CHECK(!r.Page().quit_current);
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  CHECK_EQ(r.started.size(), size_t(1));
  CHECK_EQ(r.quits, 0);
  // Ticked again (remembered either way): starting asks this game to quit.
  r.Tap([](OverlayInput& in) { in.pad_y = true; });
  CHECK(!r.Page().quit_current);
  r.Tap([](OverlayInput& in) { in.pad_down = true; });  // Down to the box.
  r.Tap([](OverlayInput& in) { in.pad_a = true; });     // A ticks it there.
  CHECK(r.Page().quit_current);
  CHECK(r.Page().launch_menu);
  r.Tap([](OverlayInput& in) { in.pad_y = true; });  // Y starts from anywhere.
  CHECK_EQ(r.started.size(), size_t(2));
  CHECK_EQ(r.quits, 1);
  CHECK(r.V().library_page);
  CHECK_EQ(r.Page().games.size(), size_t(2));

  // This game's own card: Play only says it's being played.
  r.SelectGame("this_game");
  CHECK_EQ(r.Page().selected, std::string("this_game"));
  r.Tap([](OverlayInput& in) { in.pad_y = true; });
  CHECK(r.Page().launch_menu);
  CHECK(!r.Page().quit_box);
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  CHECK_EQ(r.started.size(), size_t(2));
}

TEST(library_page_no_quit_box_without_a_handler) {
  Rig r(Fresh("page_no_quit"), false);
  CHECK(r.overlay->OpenLibraryPage());
  r.Step();
  r.SelectGame("other_game");
  r.Tap([](OverlayInput& in) { in.pad_y = true; });
  CHECK(r.Page().launch_menu);
  CHECK(!r.Page().quit_box);
}

TEST(library_page_needs_the_library) {
  System system;
  system.AddSet(OneSet("mine", "This Game"));
  OverlayOptions o;
  o.global_preferences = false;
  Overlay overlay(system, o);
  overlay.SetSize(640, 360);
  CHECK(!overlay.OpenLibraryPage());
  overlay.Open();
  overlay.Frame(0.1f, {});
  for (const auto& t : overlay.view_state().tabs) CHECK(t != ":library");
}
