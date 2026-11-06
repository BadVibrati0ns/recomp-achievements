// The library app, headless, over the demo library: grid totals, sort modes,
// live reloads, the game page and its profiles, and scripts.

#include <algorithm>
#include <filesystem>
#include <functional>

#include "app.h"
#include "demo_library.h"
#include "script.h"
#include "test.h"

using namespace recomp::achievements;
using namespace recomp::achievements::app;
namespace fs = std::filesystem;

namespace {

constexpr uint64_t kNow = 1790000000;  // Fixed, so "ago" texts and orders don't drift.

fs::path Fresh(const char* name) {
  const fs::path dir = fs::path(RECOMP_ACH_APP_TEST_TMP) / name;
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

// Written once (it takes a moment), then copied for each test with its saves.
fs::path DemoLibrary(const char* name) {
  static const fs::path base = [] {
    const fs::path root = Fresh("demo_base") / "library";
    std::string error;
    if (!demo::WriteDemoLibrary(root, {}, kNow, &error)) std::printf("  demo library: %s\n", error.c_str());
    return root;
  }();
  const fs::path root = Fresh(name) / "library";
  std::error_code ec;
  fs::copy(base, root, fs::copy_options::recursive, ec);
  fs::copy(base.parent_path() / "library-saves", root.parent_path() / "library-saves", fs::copy_options::recursive, ec);
  CHECK(!ec);
  return root;
}

struct Rig {
  std::unique_ptr<App> app;
  explicit Rig(const fs::path& root, int w = 1280, int h = 800) {
    AppOptions o;
    o.roots = {root};
    o.global_preferences = false;  // Never read the real player folder.
    o.now = [] { return kNow; };
    app = std::make_unique<App>(o);
    app->SetSize(w, h);
    Step();
  }
  bool Step(const ui::OverlayInput& in = {}, float dt = 1.0f / 30) { return app->Frame(dt, in); }
  void Tap(const std::function<void(ui::OverlayInput&)>& set) {
    ui::OverlayInput in;
    set(in);
    Step(in);
    Step();
  }
  void Settle(int frames = 20) {
    for (int i = 0; i < frames; ++i) Step();
  }
  App::State S() const { return app->state(); }
  std::vector<std::string> Order() const {
    std::vector<std::string> ids;
    for (const auto& g : S().games) ids.push_back(g.id);
    return ids;
  }
  const GameSummary* Game(const std::string& id) const {
    static App::State keep;
    keep = S();
    for (const auto& g : keep.games)
      if (g.id == id) return &g;
    return nullptr;
  }
};

}  // namespace

TEST(app_grid_totals_count_only_sets_that_are_on) {
  Rig r(DemoLibrary("totals"));
  CHECK_EQ(r.S().games.size(), size_t(5));
  const GameSummary* neon = r.Game("neon_drift");
  CHECK(neon != nullptr);
  if (!neon) return;
  // The base set is done; the online pack (off) has five more that don't count.
  CHECK_EQ(neon->count, 12u);
  CHECK_EQ(neon->unlocked, 12u);
  CHECK_EQ(neon->points, 1000u);
  CHECK_EQ(neon->unlocked_points, 1000u);
  CHECK(neon->complete());
  CHECK_EQ(neon->sets_off, size_t(1));
  CHECK_EQ(neon->platform, Platform::kXbox360);

  const GameSummary* star = r.Game("starfall_odyssey");
  CHECK(star && star->trophies);
  if (star) {
    CHECK_EQ(star->profile, std::string("Captain"));
    CHECK_EQ(star->count, 24u);
    CHECK_EQ(star->unlocked, 10u);
    CHECK_EQ(star->tiers[static_cast<int>(Tier::kPlatinum)].total, 1u);
    CHECK_EQ(star->tiers[static_cast<int>(Tier::kGold)].unlocked, 1u);
    CHECK_EQ(star->tiers[static_cast<int>(Tier::kSilver)].unlocked, 2u);
    CHECK_EQ(star->tiers[static_cast<int>(Tier::kBronze)].unlocked, 7u);
  }
  const GameSummary* garden = r.Game("pocket_garden");
  CHECK(garden && garden->icon.empty());  // The trophy is drawn.

  // Turning the pack on (as the game would) brings its five into the totals.
  auto game = LoadLibraryGame(r.S().games[0].root, "neon_drift");
  CHECK(game.has_value());
  if (!game) return;
  for (auto& s : game->sets) s.enabled = true;
  const GameSummary on = Summarize(*game);
  CHECK_EQ(on.count, 17u);
  CHECK(!on.complete());
}

TEST(app_sort_modes) {
  Rig r(DemoLibrary("sort"));
  CHECK_EQ(r.S().sort, SortMode::kLastPlayed);
  using V = std::vector<std::string>;
  CHECK_EQ(r.Order(), (V{"neon_drift", "starfall_odyssey", "pocket_garden", "castle_quest_dx", "turbo_kart"}));
  r.Tap([](ui::OverlayInput& in) { in.tab = true; });
  CHECK_EQ(r.S().sort, SortMode::kPercent);
  CHECK_EQ(r.Order(), (V{"neon_drift", "castle_quest_dx", "starfall_odyssey", "pocket_garden", "turbo_kart"}));
  r.Tap([](ui::OverlayInput& in) { in.pad_x = true; });
  CHECK_EQ(r.S().sort, SortMode::kClosest);
  // Fewest left first; the finished game goes last.
  CHECK_EQ(r.Order(), (V{"castle_quest_dx", "pocket_garden", "starfall_odyssey", "turbo_kart", "neon_drift"}));
  r.Tap([](ui::OverlayInput& in) { in.tab = true; });
  CHECK_EQ(r.S().sort, SortMode::kTitle);
  CHECK_EQ(r.Order(), (V{"castle_quest_dx", "neon_drift", "pocket_garden", "starfall_odyssey", "turbo_kart"}));
  r.Tap([](ui::OverlayInput& in) { in.tab = in.shift = true; });
  CHECK_EQ(r.S().sort, SortMode::kClosest);
}

TEST(app_live_reload_picks_up_a_new_unlock) {
  const fs::path root = DemoLibrary("live");
  Rig r(root);
  // Select a game that isn't first, so keeping the selection means something.
  r.Tap([](ui::OverlayInput& in) { in.right = true; });
  r.Tap([](ui::OverlayInput& in) { in.right = true; });
  CHECK_EQ(r.S().selected, std::string("pocket_garden"));
  const uint64_t reloads = r.S().reloads;
  CHECK_EQ(r.Game("turbo_kart")->unlocked, 2u);

  // A game running beside the app: one unlock, one checklist step.
  CHECK(demo::PlayDemoGame(root, "turbo_kart", {{"unlock", "turbo_kart:k2", ""}}));
  CHECK(demo::PlayDemoGame(root, "pocket_garden", {{"event", "plant", "Mint"}}));
  r.Settle(40);  // Past the 1 s poll.
  CHECK(r.S().reloads > reloads);
  CHECK_EQ(r.Game("turbo_kart")->unlocked, 3u);
  CHECK_EQ(r.S().popups, uint64_t(2));  // The unlock and the step.
  CHECK_EQ(r.S().recent.front(), std::string("turbo_kart/turbo_kart:k2"));
  // Both just played: they move to the front, and the selection stays put.
  const auto order = r.Order();
  CHECK(std::find(order.begin(), order.begin() + 2, "turbo_kart") != order.begin() + 2);
  CHECK_EQ(r.S().selected, std::string("pocket_garden"));

  // Nothing changed: no reload.
  const uint64_t after = r.S().reloads;
  r.Settle(40);
  CHECK_EQ(r.S().reloads, after);
}

TEST(app_game_page_and_back) {
  Rig r(DemoLibrary("page"));
  CHECK(!r.S().game_page);
  r.Tap([](ui::OverlayInput& in) { in.pad_a = true; });  // Neon Drift, played last.
  r.Settle();
  App::State s = r.S();
  CHECK(s.game_page);
  CHECK_EQ(s.game, std::string("neon_drift"));
  // The online pack is listed, as Off, beside the base set.
  CHECK_EQ(s.page.tabs, (std::vector<std::string>{"", "neon_drift", "neon_drift_online"}));
  CHECK_EQ(s.page.keys.size(), size_t(17));
  CHECK(s.page.host_button.empty());  // One profile: no picker.
  // The viewer's own navigation works here: a set tab, then back.
  r.Tap([](ui::OverlayInput& in) { in.pad_rb = true; });
  CHECK_EQ(r.S().page.tab, std::string("neon_drift"));
  r.Tap([](ui::OverlayInput& in) { in.pad_b = true; });
  r.Settle();
  CHECK(!r.S().game_page);
  CHECK_EQ(r.S().selected, std::string("neon_drift"));
  // The B that closed the page did nothing on the grid.
  CHECK_EQ(r.S().sort, SortMode::kLastPlayed);
}

TEST(app_game_page_profiles) {
  Rig r(DemoLibrary("profiles"));
  r.Tap([](ui::OverlayInput& in) { in.right = true; });
  CHECK_EQ(r.S().selected, std::string("starfall_odyssey"));
  r.Tap([](ui::OverlayInput& in) { in.enter = true; });
  r.Settle();
  App::State s = r.S();
  CHECK(s.game_page);
  CHECK_EQ(s.profiles.size(), size_t(2));
  CHECK_EQ(s.profile, std::string("Captain"));  // BestProfile: played last on a tie.
  CHECK(s.page.host_button.find("Captain") != std::string::npos);
  r.Tap([](ui::OverlayInput& in) { in.pad_start = true; });
  s = r.S();
  CHECK_EQ(s.profile, std::string("Navigator"));
  CHECK(s.page.host_button.find("Navigator") != std::string::npos);
  CHECK_EQ(r.Game("starfall_odyssey")->unlocked, 3u);  // The grid follows the pick.
  r.Tap([](ui::OverlayInput& in) { in.escape = true; });
  r.Settle();
  CHECK(!r.S().game_page);
}

TEST(app_redraws_only_on_change) {
  Rig r(DemoLibrary("redraw"));
  r.Settle(5);
  int redraws = 0;
  for (int i = 0; i < 20; ++i) redraws += r.Step();
  CHECK_EQ(redraws, 0);
  ui::OverlayInput in;
  in.down = true;
  CHECK(r.Step(in));
  const auto& img = r.app->image();
  CHECK_EQ(img.width, 1280);
  CHECK_EQ(img.rgba[3], 255);  // Opaque.
}

TEST(app_deck_scale_and_resize) {
  Rig r(DemoLibrary("resize"), 1280, 800);
  r.app->SetSize(900, 600);
  r.Settle(3);
  CHECK_EQ(r.app->image().width, 900);
  r.app->SetSize(1280, 800);
  r.app->SetScale(1.25f);
  r.Settle(3);
  CHECK_EQ(r.app->image().height, 800);
}

TEST(app_script_parse_and_run) {
  std::vector<ScriptCommand> script;
  std::string error;
  CHECK(ParseScript("# tour\nwait 0.5\nkey Down\npad a 0.2\nstick ly 0.8\nmouse 10 20 click\nwheel -1\n"
                    "size 640 400\nscale 1.5\nlabels playstation\nexec true # not a comment\npoll\n",
                    script, &error));
  CHECK_EQ(script.size(), size_t(11));
  CHECK_EQ(script[9].args.at(0), std::string("true # not a comment"));
  CHECK(!ParseScript("wait\n", script, &error));
  CHECK(error.find("line 1") != std::string::npos);
  CHECK(!ParseScript("key Nope\n", script, &error));
  CHECK(!ParseScript("dance 3\n", script, &error));

  CHECK(ParseScript("wait 1\nkey Tab\npad x 0.2\nsize 320 200\n", script, &error));
  int frames = 0, sized = 0, tabs = 0;
  ScriptTarget t;
  t.frame = [&](float, const ui::OverlayInput& in) {
    ++frames;
    tabs += in.tab;
  };
  t.resize = [&](int w, int h) { sized = w * h; };
  const int n = RunScript(script, 30, t);
  CHECK_EQ(n, frames);
  CHECK_EQ(frames, 30 + 4 + 1 + 6 + 1);
  CHECK_EQ(tabs, 4);
  CHECK_EQ(sized, 320 * 200);
}

TEST(app_relative_time) {
  CHECK_EQ(RelativeTime(0, kNow), std::string("never"));
  CHECK_EQ(RelativeTime(kNow - 10, kNow), std::string("just now"));
  CHECK_EQ(RelativeTime(kNow - 300, kNow), std::string("5 minutes ago"));
  CHECK_EQ(RelativeTime(kNow - 7200, kNow), std::string("2 hours ago"));
  CHECK_EQ(RelativeTime(kNow - 30 * 3600, kNow), std::string("yesterday"));
  CHECK_EQ(RelativeTime(kNow - 4 * 86400, kNow), std::string("4 days ago"));
  CHECK_EQ(RelativeTime(kNow - 21 * 86400, kNow), std::string("3 weeks ago"));
}
