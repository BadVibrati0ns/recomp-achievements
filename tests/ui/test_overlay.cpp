// The software overlay against a real System: toggling, navigation, filters,
// secrets, redraw decisions and popups. No GPU, no ImGui.

#include <algorithm>
#include <functional>
#include <memory>

#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/overlay.h"
#include "test.h"

using namespace recomp::achievements;
using namespace recomp::achievements::ui;

namespace {

AchievementSet XboxSet() {
  AchievementSet s;
  s.id = "x";
  s.title = "Xbox set";
  s.platform = Platform::kXbox360;
  s.credits = {{"The recomp-achievements authors", "tests", ""}};
  for (int i = 0; i < 8; ++i) {
    Achievement a;
    a.local_id = "a" + std::to_string(i);
    a.name = "Achievement " + std::to_string(i);
    a.description = "Description " + std::to_string(i);
    a.points = 10;
    a.icon = "fake:" + std::to_string(i);
    a.category = i < 4 ? "Story" : "Extra";
    s.achievements.push_back(a);
  }
  s.achievements[2].secret = true;
  s.achievements[2].name = "Hidden Ending";
  s.achievements[3].trigger.type = "checklist";
  s.achievements[3].trigger.event = "trick";
  s.achievements[3].checklist = {"Kickflip", "Heelflip", "Varial"};
  return s;
}

AchievementSet CustomSet() {
  AchievementSet s;
  s.id = "c";
  s.title = "Custom pack";
  for (int i = 0; i < 3; ++i) {
    Achievement a;
    a.local_id = "c" + std::to_string(i);
    a.name = "Custom " + std::to_string(i);
    a.points = 5;
    s.achievements.push_back(a);
  }
  return s;
}

struct Rig {
  System system;
  std::unique_ptr<Overlay> overlay;

  explicit Rig(OverlayOptions opt = {}, int w = 960, int h = 540) {
    opt.global_preferences = false;  // Never touch the real player folder.
    system.AddSet(XboxSet());
    system.AddSet(CustomSet());
    system.Unlock("x:a0");
    system.ReportEvent("trick", "Kickflip");
    overlay = std::make_unique<Overlay>(system, std::move(opt));
    overlay->icons().AddProvider("fake", [](const IconRequest&, IconData& out) {
      out.image.width = out.image.height = 4;
      out.image.rgba.assign(4 * 4 * 4, 200);
      return IconResult::kOk;
    });
    overlay->SetSize(w, h);
    Step();
  }
  bool Step(const OverlayInput& in = {}, float dt = 1.0f / 60) { return overlay->Frame(dt, in); }
  void Tap(const std::function<void(OverlayInput&)>& set) {
    OverlayInput in;
    set(in);
    Step(in);
    Step();
  }
  void Settle(int frames = 30) {
    for (int i = 0; i < frames; ++i) Step();
  }
  Overlay::ViewState V() const { return overlay->view_state(); }
};

bool Any(const Image& img) {
  for (size_t i = 3; i < img.rgba.size(); i += 4)
    if (img.rgba[i]) return true;
  return false;
}

}  // namespace

TEST(overlay_parse_bindings) {
  uint32_t m = 0;
  CHECK(ParsePadChord("start+lb", m));
  CHECK_EQ(m, kPadStart | kPadLB);
  CHECK(ParsePadChord(" Options + L1 ", m));
  CHECK_EQ(m, kPadStart | kPadLB);
  CHECK(ParsePadChord("back", m));
  CHECK_EQ(m, kPadBack);
  CHECK(ParsePadChord("none", m));
  CHECK_EQ(m, 0u);
  CHECK(!ParsePadChord("start+zz", m));
  int k = 0;
  CHECK(ParseToggleKey("F8", k));
  CHECK_EQ(k, 8);
  CHECK(ParseToggleKey("f12", k));
  CHECK_EQ(k, 12);
  CHECK(ParseToggleKey("none", k));
  CHECK_EQ(k, 0);
  CHECK(!ParseToggleKey("F13", k));
  CHECK(!ParseToggleKey("Q", k));
  OverlayInput in;
  in.pad_a = in.pad_start = true;
  CHECK_EQ(PadButtons(in), kPadA | kPadStart);
}

TEST(overlay_f8_toggles_once_per_press) {
  Rig r;
  CHECK(!r.overlay->is_open());
  OverlayInput f8;
  f8.f[8] = true;
  for (int i = 0; i < 20; ++i) r.Step(f8);  // Held: one toggle.
  CHECK(r.overlay->is_open());
  CHECK(r.overlay->wants_input());
  r.Step();
  r.Tap([](OverlayInput& in) { in.f[8] = true; });
  CHECK(!r.overlay->is_open());
  CHECK(!r.overlay->wants_input());
  r.Tap([](OverlayInput& in) { in.f[7] = true; });
  CHECK(!r.overlay->is_open());
}

TEST(overlay_chord_either_order) {
  Rig r;
  OverlayInput in;
  // Start alone does nothing.
  in.pad_start = true;
  for (int i = 0; i < 5; ++i) r.Step(in);
  CHECK(!r.overlay->is_open());
  // Start held, then LB: opens once, however long both are held.
  in.pad_lb = true;
  for (int i = 0; i < 30; ++i) r.Step(in);
  CHECK(r.overlay->is_open());
  // Releasing one and pressing it again is a new chord: closes.
  in.pad_lb = false;
  r.Step(in);
  in.pad_lb = true;
  r.Step(in);
  CHECK(!r.overlay->is_open());
  r.Step();
  // LB first, then Start.
  OverlayInput lb;
  lb.pad_lb = true;
  for (int i = 0; i < 5; ++i) r.Step(lb);
  CHECK(!r.overlay->is_open());
  lb.pad_start = true;
  r.Step(lb);
  CHECK(r.overlay->is_open());
  r.Step(lb);
  CHECK(r.overlay->is_open());
  r.Step();
  // The held LB from the chord didn't switch sets.
  CHECK_EQ(r.V().tab, std::string(""));
}

TEST(overlay_chord_close_undoes_its_first_button) {
  Rig r;
  r.overlay->Open();
  r.Settle();
  CHECK_EQ(r.V().tab, std::string(""));
  // LB first switches the set...
  OverlayInput in;
  in.pad_lb = true;
  r.Step(in);
  CHECK(r.V().tab != std::string(""));
  // ...then Start completes the chord: closed, and the switch is undone.
  in.pad_start = true;
  r.Step(in);
  CHECK(!r.overlay->is_open());
  r.overlay->Open();
  r.Step();
  CHECK_EQ(r.V().tab, std::string(""));
}

TEST(overlay_navigation_sets_and_filters) {
  Rig r;
  r.overlay->Open();
  r.Settle();
  auto v = r.V();
  CHECK_EQ(v.keys.size(), 11u);  // Both sets.
  CHECK_EQ(v.selected, v.keys[0]);
  r.Tap([](OverlayInput& in) { in.pad_down = true; });
  CHECK_EQ(r.V().selected, std::string("x:a1"));
  r.Tap([](OverlayInput& in) { in.down = true; });
  CHECK_EQ(r.V().selected, std::string("x:a2"));
  r.Tap([](OverlayInput& in) { in.up = true; });
  CHECK_EQ(r.V().selected, std::string("x:a1"));

  // Sets: RB / right / LB / left.
  r.Tap([](OverlayInput& in) { in.pad_rb = true; });
  CHECK_EQ(r.V().tab, std::string("x"));
  CHECK_EQ(r.V().keys.size(), 8u);
  r.Tap([](OverlayInput& in) { in.right = true; });
  CHECK_EQ(r.V().tab, std::string("c"));
  r.Tap([](OverlayInput& in) { in.pad_lb = true; });
  CHECK_EQ(r.V().tab, std::string("x"));

  // Filters cycle All -> Unlocked -> Locked.
  r.Tap([](OverlayInput& in) { in.pad_x = true; });
  v = r.V();
  CHECK_EQ(v.filter, 1);
  CHECK_EQ(v.keys.size(), 1u);
  CHECK_EQ(v.keys[0], std::string("x:a0"));
  r.Tap([](OverlayInput& in) { in.tab = true; });
  v = r.V();
  CHECK_EQ(v.filter, 2);
  CHECK_EQ(v.keys.size(), 7u);
  CHECK(std::find(v.keys.begin(), v.keys.end(), "x:a0") == v.keys.end());
  r.Tap([](OverlayInput& in) { in.tab = in.shift = true; });
  CHECK_EQ(r.V().filter, 1);
  r.Tap([](OverlayInput& in) { in.pad_x = true; });
  r.Tap([](OverlayInput& in) { in.pad_x = true; });
  CHECK_EQ(r.V().filter, 0);

  // Expand the checklist row.
  while (r.V().selected != "x:a3") r.Tap([](OverlayInput& in) { in.pad_down = true; });
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  v = r.V();
  CHECK_EQ(v.expanded.size(), 1u);
  CHECK(!v.expanded.empty() && v.expanded[0] == "x:a3");
  r.Tap([](OverlayInput& in) { in.enter = true; });
  CHECK(r.V().expanded.empty());

  // Holding down repeats.
  OverlayInput hold;
  hold.pad_down = true;
  for (int i = 0; i < 60; ++i) r.Step(hold);
  CHECK_EQ(r.V().selected, std::string("x:a7"));
  r.Step();
  r.Tap([](OverlayInput& in) { in.home = true; });
  CHECK_EQ(r.V().selected, std::string("x:a0"));
  r.Tap([](OverlayInput& in) { in.end = true; });
  CHECK_EQ(r.V().selected, std::string("x:a7"));

  // B closes.
  r.Tap([](OverlayInput& in) { in.pad_b = true; });
  CHECK(!r.overlay->is_open());
}

TEST(overlay_secrets_and_credits) {
  Rig r;
  r.overlay->Open();
  r.Settle();
  auto v = r.V();
  auto has = [](const std::vector<std::string>& names, const char* n) {
    return std::find(names.begin(), names.end(), n) != names.end();
  };
  CHECK(!has(v.names, "Hidden Ending"));
  CHECK(has(v.names, "Secret achievement"));
  r.Tap([](OverlayInput& in) { in.pad_y = true; });
  v = r.V();
  CHECK(v.reveal_secrets);
  CHECK(has(v.names, "Hidden Ending"));
  r.Tap([](OverlayInput& in) { in.space = true; });
  CHECK(!r.V().reveal_secrets);

  r.Tap([](OverlayInput& in) { in.pad_back = true; });
  CHECK(r.V().credits);
  r.Tap([](OverlayInput& in) { in.escape = true; });  // Back to the list, still open.
  CHECK(!r.V().credits);
  CHECK(r.overlay->is_open());
  r.Tap([](OverlayInput& in) { in.f[1] = true; });
  CHECK(r.V().credits);

  OverlayOptions no_secrets;
  no_secrets.allow_reveal_secrets = false;
  Rig q(no_secrets);
  q.overlay->Open();
  q.Settle();
  q.Tap([](OverlayInput& in) { in.pad_y = true; });
  CHECK(!q.V().reveal_secrets);
}

TEST(overlay_redraws_only_on_change) {
  Rig r;
  // Closed and no popups: nothing to upload.
  for (int i = 0; i < 100; ++i) CHECK(!r.Step());
  CHECK(!r.overlay->visible());
  CHECK(!Any(r.overlay->image()));
  r.overlay->Open();
  CHECK(r.Step());  // Opening animates.
  r.Settle(60);
  int redraws = 0;
  for (int i = 0; i < 100; ++i) redraws += r.Step();
  CHECK_EQ(redraws, 0);
  CHECK(Any(r.overlay->image()));
  CHECK(r.overlay->visible());
  // Input that changes nothing doesn't redraw either.
  OverlayInput nothing;
  nothing.pad_ly = 0.1f;
  CHECK(!r.Step(nothing));
  // Moving the selection redraws the panel only.
  r.Tap([](OverlayInput& in) { in.pad_down = true; });
  r.Settle(60);
  r.Tap([](OverlayInput& in) { in.pad_down = true; });
  const PixelRect d = r.overlay->dirty_rect();
  CHECK(!d.empty());
  CHECK(d.width() < 960);
  r.Settle(60);
  // A data change while open redraws (the row shows unlocked).
  r.system.Unlock("x:a5");
  CHECK(r.Step());
  // Size changes redraw.
  r.overlay->SetSize(1280, 720);
  CHECK(r.Step());
  CHECK_EQ(r.overlay->image().width, 1280);
  r.overlay->Close();
  r.Settle(120);  // Closing animation and the unlock popup play out.
  for (int i = 0; i < 60 * 8; ++i) r.Step();
  CHECK(!r.Step());
  CHECK(!Any(r.overlay->image()));
}

TEST(overlay_popups_render_and_clear) {
  int sounds = 0;
  OverlayOptions opt;
  opt.play_sound = [&](std::shared_ptr<const Pcm> pcm, float volume, NotificationKind) {
    CHECK(pcm && !pcm->samples.empty());
    CHECK(volume > 0);
    ++sounds;
  };
  Rig r(opt, 1920, 1080);
  r.system.Unlock("c:c1");  // Custom: minimal theme, bottom centre, no icon (trophy).
  bool drew = false;
  for (int i = 0; i < 30; ++i) drew |= r.Step();
  CHECK(drew);
  CHECK_EQ(sounds, 1);
  CHECK(!r.overlay->is_open());
  CHECK(r.overlay->visible());
  const Image& img = r.overlay->image();
  // Ink only in the bottom half, around the centre.
  bool top = false, bottom = false;
  for (int y = 0; y < 1080; y += 4)
    for (int x = 0; x < 1920; x += 4)
      if (img.rgba[(static_cast<size_t>(y) * 1920 + x) * 4 + 3]) (y < 540 ? top : bottom) = true;
  CHECK(!top);
  CHECK(bottom);
  const PixelRect d = r.overlay->dirty_rect();
  CHECK(!d.empty() && d.width() < 800 && d.y0 > 540);
  // Holding: no redraws.
  int redraws = 0;
  for (int i = 0; i < 60; ++i) redraws += r.Step();
  CHECK_EQ(redraws, 0);
  // Plays out, then the image is clear and stays untouched.
  for (int i = 0; i < 60 * 8; ++i) r.Step();
  CHECK(!Any(r.overlay->image()));
  CHECK(!r.overlay->visible());
  CHECK(!r.Step());
}

TEST(overlay_popups_wait_for_viewer_when_asked) {
  OverlayOptions opt;
  opt.popups_over_viewer = false;
  Rig r(opt);
  r.overlay->Open();
  r.Settle();
  r.system.Unlock("x:a4");
  r.Settle(60);
  CHECK(r.overlay->center().Busy());
  CHECK(r.overlay->center().Active().empty() || r.overlay->center().Active()[0].elapsed == 0.0f);
  r.overlay->Close();
  r.Settle(30);
  CHECK(!r.overlay->center().Active().empty());
}

TEST(overlay_global_preferences_rebind) {
  StyleManager styles;
  styles.SetGlobalStyle(nlohmann::json::parse(R"({"viewer": {"key": "F9", "pad": "back", "buttons": "playstation"}})"));
  OverlayOptions opt;
  opt.styles = &styles;
  Rig r(opt);
  r.Tap([](OverlayInput& in) { in.f[8] = true; });
  CHECK(!r.overlay->is_open());
  r.Tap([](OverlayInput& in) {
    in.pad_start = true;
    in.pad_lb = true;
  });
  CHECK(!r.overlay->is_open());
  r.Tap([](OverlayInput& in) { in.f[9] = true; });
  CHECK(r.overlay->is_open());
  r.Tap([](OverlayInput& in) { in.pad_back = true; });
  CHECK(!r.overlay->is_open());
  CHECK(styles.warnings().empty());
}

TEST(overlay_mouse) {
  Rig r(OverlayOptions{}, 1280, 720);
  r.overlay->Open();
  r.Settle();
  OverlayInput in;
  in.mouse_present = true;
  in.mouse_x = 640;
  in.mouse_y = 360;
  in.mouse_wheel = -3;  // Toward the player: scroll down.
  r.Step(in);
  in.mouse_wheel = 0;
  r.Settle(60);
  CHECK(r.V().scroll > 0);
  // A click outside the panel closes.
  in.mouse_x = 5;
  in.mouse_y = 5;
  r.Step(in);
  in.mouse_left = true;
  r.Step(in);
  CHECK(!r.overlay->is_open());
}

TEST(overlay_style_viewer_json) {
  Style s = BuiltinTheme("minimal");
  std::vector<std::string> w;
  ApplyStyleJson(s, nlohmann::json::parse(R"({"viewer": {"key": "F10", "pad": "lb+rb", "scale": 1.5,
      "colors": {"accent": "#FF8800", "panel": [10, 10, 10]}}})"),
                 {}, &w);
  CHECK(w.empty());
  CHECK_EQ(s.viewer.key, std::string("F10"));
  CHECK_EQ(s.viewer.pad, std::string("lb+rb"));
  CHECK_NEAR(s.viewer.scale, 1.5, 1e-6);
  CHECK(s.viewer.accent.has_value() && *s.viewer.accent == Color::Rgba(255, 136, 0));
  CHECK(!s.viewer.text.has_value());
  ApplyStyleJson(s, nlohmann::json::parse(R"({"viewer": {"colors": {"accent": null, "glow": "#FFFFFF"}, "nope": 1}})"), {},
                 &w);
  CHECK(!s.viewer.accent.has_value());
  CHECK_EQ(w.size(), 2u);
  // Round trip.
  Style t = BuiltinTheme("minimal");
  ApplyStyleJson(s, StyleToJson(s), {}, nullptr);
  ApplyStyleJson(t, StyleToJson(s), {}, nullptr);
  CHECK(t == s);
}

TEST(overlay_ui_scale_and_tiny_sizes) {
  Rig r(OverlayOptions{}, 1920, 1080);
  r.overlay->SetSize(1920, 1080, 2.0f);  // E.g. a 4K-density UI on a 1080p buffer.
  r.system.Unlock("x:a6");
  for (int i = 0; i < 40; ++i) r.Step();
  const PixelRect d = r.overlay->dirty_rect();
  CHECK(!d.empty());
  r.overlay->Open();
  r.Settle();
  CHECK(Any(r.overlay->image()));
  // Degenerate sizes don't crash.
  r.overlay->SetSize(0, 0);
  CHECK(!r.Step());
  r.overlay->SetSize(64, 48);
  r.Settle();
  CHECK_EQ(r.overlay->image().width, 64);
}

TEST(overlay_follows_set_changes) {
  Rig r;
  r.overlay->Open();
  r.Settle();
  r.Tap([](OverlayInput& in) { in.pad_lb = true; });  // The custom pack's tab.
  CHECK_EQ(r.V().tab, std::string("c"));
  // Replaced and removed sets don't leave stale rows behind.
  AchievementSet c = CustomSet();
  c.achievements.pop_back();
  r.system.AddSet(c);
  r.Settle(2);
  CHECK_EQ(r.V().keys.size(), 2u);
  r.system.RemoveSet("c");
  r.Settle(2);
  CHECK_EQ(r.V().keys.size(), 8u);  // Only one set left: its tab.
  r.overlay->Close();
  r.system.ClearSets();
  r.Settle(20);
  r.overlay->Open();
  r.Settle(20);
  CHECK(r.V().keys.empty());
  CHECK(Any(r.overlay->image()));  // "No achievements yet."
}

TEST(overlay_game_input_holds_chord_buttons) {
  Rig r;
  // Start alone: held back during the grace period, then reaches the game.
  OverlayInput start;
  start.pad_start = true;
  r.Step(start);
  CHECK(!r.overlay->GameInput(start).pad_start);
  for (int i = 0; i < 12; ++i) r.Step(start);  // 0.2 s > 0.15 s.
  CHECK(r.overlay->GameInput(start).pad_start);
  r.Step();  // Release.
  CHECK(!r.overlay->GameInput({}).pad_start);

  // A quick tap of Start becomes a one-frame tap after release.
  r.Step(start);
  CHECK(!r.overlay->GameInput(start).pad_start);
  r.Step();
  CHECK(r.overlay->GameInput({}).pad_start);
  r.Step();
  CHECK(!r.overlay->GameInput({}).pad_start);

  // Start then LB: the chord opens the viewer and the game never sees Start.
  r.Step(start);
  CHECK(!r.overlay->GameInput(start).pad_start);
  OverlayInput chord = start;
  chord.pad_lb = true;
  r.Step(chord);
  CHECK(r.overlay->is_open());
  const OverlayInput seen = r.overlay->GameInput(chord);
  CHECK(!seen.pad_start && !seen.pad_lb);

  // Other buttons pass straight through while closed, and F8 never does.
  r.overlay->Close();
  r.Step();
  OverlayInput a;
  a.pad_a = true;
  a.f[8] = false;
  a.f[5] = true;
  r.Step(a);
  CHECK(r.overlay->GameInput(a).pad_a);
  CHECK(r.overlay->GameInput(a).f[5]);
  OverlayInput f8;
  f8.f[8] = true;
  CHECK(!r.overlay->GameInput(f8).f[8]);
}

TEST(overlay_close_input_is_latched_until_released) {
  Rig r;
  r.overlay->Open();
  r.Settle();
  CHECK(r.overlay->wants_input());
  OverlayInput b;
  b.pad_b = true;
  r.Step(b);  // B closes the viewer...
  CHECK(!r.overlay->is_open());
  CHECK(r.overlay->wants_input());  // ...but the game doesn't get B while it's held.
  r.Step(b);
  CHECK(!r.overlay->GameInput(b).pad_b);
  r.Step();  // Released.
  CHECK(!r.overlay->wants_input());
  OverlayInput again = b;
  r.Step(again);
  CHECK(r.overlay->GameInput(again).pad_b);  // A new press reaches the game.
}

TEST(overlay_full_dirty_rect_when_reappearing) {
  Rig r(OverlayOptions{}, 1920, 1080);
  r.system.Unlock("c:c1");
  CHECK(r.Step());
  const PixelRect first = r.overlay->dirty_rect();
  CHECK(first.x0 == 0 && first.y0 == 0 && first.x1 == 1920 && first.y1 == 1080);
  // Later frames of the same popup only touch its area.
  bool partial = false;
  for (int i = 0; i < 20; ++i)
    if (r.Step()) partial |= r.overlay->dirty_rect().width() < 1920;
  CHECK(partial);
  for (int i = 0; i < 60 * 8; ++i) r.Step();
  CHECK(!r.overlay->visible());
  // Showing again uploads everything.
  r.system.Unlock("c:c2");
  CHECK(r.Step());
  CHECK(r.overlay->dirty_rect().width() == 1920);
}
