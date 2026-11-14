// The software overlay against a real System: toggling, navigation, filters,
// secrets, redraw decisions and popups. No GPU, no ImGui.

#include <algorithm>
#include <cmath>
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

TEST(overlay_settings_page_navigation) {
  Rig r;
  r.overlay->Open();
  r.Settle();
  CHECK(!r.V().settings);
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  auto v = r.V();
  CHECK(v.settings);
  CHECK_EQ(v.setting_rows, (std::vector<std::string>{"sound", "volume", "theme", "popup_size", "popup_time",
                                                     "list_size", "set:x", "set:c", "reset:pick", "reset:set:x", "reset:set:c", "reset:all"}));
  CHECK_EQ(v.setting_selected, std::string("sound"));
  r.Tap([](OverlayInput& in) { in.pad_down = true; });
  CHECK_EQ(r.V().setting_selected, std::string("volume"));
  r.Tap([](OverlayInput& in) { in.end = true; });
  CHECK_EQ(r.V().setting_selected, std::string("reset:all"));
  // B goes back to the list, not out of the viewer; F2 opens it again.
  r.Tap([](OverlayInput& in) { in.pad_b = true; });
  CHECK(!r.V().settings);
  CHECK(r.overlay->is_open());
  r.Tap([](OverlayInput& in) { in.f[2] = true; });
  CHECK(r.V().settings);
  CHECK_EQ(r.V().setting_selected, std::string("reset:all"));  // Remembered.
  r.Tap([](OverlayInput& in) { in.escape = true; });
  CHECK(!r.V().settings);
  r.overlay->Close();
  r.Settle();
  r.overlay->OpenSettings();
  r.Settle();
  CHECK(r.overlay->is_open());
  CHECK(r.V().settings);
  // The chord still closes from the settings page, and Start's press is undone.
  OverlayInput chord;
  chord.pad_start = true;
  r.Step(chord);
  CHECK(!r.V().settings);
  chord.pad_lb = true;
  r.Step(chord);
  CHECK(!r.overlay->is_open());
  r.Settle();
  r.overlay->Open();
  r.Settle();
  CHECK(!r.V().settings);
}

TEST(overlay_settings_toggle_set) {
  Rig r;
  r.overlay->Open();
  r.Settle();
  CHECK_EQ(r.V().tabs, (std::vector<std::string>{"", "x", "c"}));
  CHECK_EQ(r.V().keys.size(), 11u);
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  while (r.V().setting_selected != "set:c") r.Tap([](OverlayInput& in) { in.pad_down = true; });
  const uint64_t gen = r.system.generation();
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  CHECK(!r.system.IsSetEnabled("c"));
  CHECK(r.system.generation() > gen);
  // The row stays (sets that are off are listed), selected.
  CHECK_EQ(r.V().setting_selected, std::string("set:c"));
  CHECK_EQ(r.V().tabs, (std::vector<std::string>{"x"}));  // One set left: no "All".
  r.Tap([](OverlayInput& in) { in.pad_b = true; });
  auto v = r.V();
  CHECK_EQ(v.keys.size(), 8u);
  CHECK(std::none_of(v.keys.begin(), v.keys.end(), [](const std::string& k) { return k.rfind("c:", 0) == 0; }));
  CHECK_EQ(r.system.GetTotals().count, 8u);
  // No popup can come from it while it's off.
  r.system.Unlock("c:c2");
  r.Settle(5);
  CHECK(r.overlay->center().Active().empty());
  // Left / right also switch it, and turning it back on restores everything.
  r.Tap([](OverlayInput& in) { in.f[2] = true; });
  CHECK_EQ(r.V().setting_selected, std::string("set:c"));
  r.Tap([](OverlayInput& in) { in.left = true; });  // Already off.
  CHECK(!r.system.IsSetEnabled("c"));
  r.Tap([](OverlayInput& in) { in.right = true; });
  CHECK(r.system.IsSetEnabled("c"));
  CHECK_EQ(r.V().tabs, (std::vector<std::string>{"", "x", "c"}));
  r.Tap([](OverlayInput& in) { in.pad_b = true; });
  CHECK_EQ(r.V().keys.size(), 11u);
}

TEST(overlay_settings_theme_and_sizes) {
  Rig r;
  r.overlay->Open();
  r.Settle();
  const std::string game_theme = r.overlay->styles().Resolve(nullptr)->theme;
  const float game_scale = r.overlay->styles().Resolve(nullptr)->scale;
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  for (int i = 0; i < 2; ++i) r.Tap([](OverlayInput& in) { in.pad_down = true; });
  CHECK_EQ(r.V().setting_selected, std::string("theme"));
  // Right steps from the game's choice to "by platform", then the built-ins.
  r.Tap([](OverlayInput& in) { in.pad_right = true; });
  CHECK_EQ(GetAppearanceSettings(r.system).theme, std::string("auto"));
  r.Tap([](OverlayInput& in) { in.pad_right = true; });
  CHECK_EQ(GetAppearanceSettings(r.system).theme, std::string("xbox360"));
  r.Step();
  CHECK_EQ(r.overlay->styles().Resolve(nullptr)->theme, std::string("xbox360"));
  CHECK(r.V().settings);  // Left / right adjust here instead of switching tabs.
  // Left past the start wraps round to the last theme; A steps forward.
  r.Tap([](OverlayInput& in) { in.pad_left = true; });
  r.Tap([](OverlayInput& in) { in.pad_left = true; });
  CHECK_EQ(GetAppearanceSettings(r.system).theme, std::string(""));
  r.Tap([](OverlayInput& in) { in.pad_left = true; });
  CHECK_EQ(GetAppearanceSettings(r.system).theme, std::string("classic"));
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  CHECK_EQ(GetAppearanceSettings(r.system).theme, std::string(""));
  r.Step();
  CHECK_EQ(r.overlay->styles().Resolve(nullptr)->theme, game_theme);

  // Popup size: one step up multiplies the popups' scale, and is saved.
  r.Tap([](OverlayInput& in) { in.pad_down = true; });
  CHECK_EQ(r.V().setting_selected, std::string("popup_size"));
  r.Tap([](OverlayInput& in) { in.pad_right = true; });
  CHECK_NEAR(GetAppearanceSettings(r.system).popup_size, 1.15f, 1e-6);
  CHECK_NEAR(r.system.GetSetting("appearance")["popup_size"].get<double>(), 1.15, 1e-9);
  r.Step();
  CHECK_NEAR(r.overlay->styles().Resolve(nullptr)->scale, game_scale * 1.15f, 1e-5);
  // The ends stop left / right; A wraps from the largest to the smallest.
  for (int i = 0; i < 6; ++i) r.Tap([](OverlayInput& in) { in.pad_right = true; });
  CHECK_NEAR(GetAppearanceSettings(r.system).popup_size, 1.5f, 1e-6);
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  CHECK_NEAR(GetAppearanceSettings(r.system).popup_size, 0.75f, 1e-6);

  // Popup time: from the game's choice to 2 s, which becomes the unlock
  // popup's hold; progress popups keep their length relative to it.
  r.Tap([](OverlayInput& in) { in.pad_down = true; });
  CHECK_EQ(r.V().setting_selected, std::string("popup_time"));
  const Style game = *r.overlay->styles().Resolve(nullptr);
  r.Tap([](OverlayInput& in) { in.pad_right = true; });
  CHECK_NEAR(GetAppearanceSettings(r.system).popup_seconds, 2.0f, 1e-6);
  CHECK_NEAR(r.system.GetSetting("appearance")["popup_seconds"].get<double>(), 2.0, 1e-9);
  r.Step();
  auto timed = r.overlay->styles().Resolve(nullptr);
  CHECK_NEAR(timed->unlock.timing.hold, 2.0f, 1e-5);
  CHECK_NEAR(timed->progress.timing.hold, game.progress.timing.hold * 2.0f / game.unlock.timing.hold, 1e-5);
  CHECK_NEAR(timed->unlock.timing.in, game.unlock.timing.in, 1e-6);
  // Left from the smallest goes back to the game's choice and stops there; A wraps.
  r.Tap([](OverlayInput& in) { in.pad_left = true; });
  r.Tap([](OverlayInput& in) { in.pad_left = true; });
  CHECK_NEAR(GetAppearanceSettings(r.system).popup_seconds, 0.0f, 1e-6);
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  CHECK_NEAR(GetAppearanceSettings(r.system).popup_seconds, 2.0f, 1e-6);

  // List size grows the viewer itself.
  r.Tap([](OverlayInput& in) { in.pad_down = true; });
  CHECK_EQ(r.V().setting_selected, std::string("list_size"));
  const float before = r.overlay->styles().Resolve(nullptr)->viewer.scale;
  r.Tap([](OverlayInput& in) { in.pad_right = true; });
  r.Step();
  CHECK_NEAR(r.overlay->styles().Resolve(nullptr)->viewer.scale, before * 1.15f, 1e-5);
  CHECK(r.V().settings);
  CHECK_EQ(r.V().setting_selected, std::string("list_size"));  // Still on the row after the relayout.
}

TEST(overlay_list_size_in_a_small_window) {
  // A short window (a game's 549 x 311, say) sits at the smallest readable
  // scale: List size still shrinks the list, and can't grow it past the room.
  Rig r(OverlayOptions{}, 549, 311);
  r.overlay->Open();
  r.Settle();
  const float base = r.V().list_scale;
  CHECK_NEAR(base, 0.75f, 1e-4);
  auto set_list = [&](float size) {
    AppearanceSettings look;
    look.list_size = size;
    SetAppearanceSettings(r.system, look);
    r.Settle(2);
  };
  set_list(0.75f);
  CHECK_NEAR(r.V().list_scale, base * 0.75f, 1e-4);
  set_list(1.5f);
  CHECK_NEAR(r.V().list_scale, base, 1e-4);  // No room for a bigger list.
  // A tall window has room: the same setting grows it there.
  r.overlay->SetSize(1920, 1080);
  r.Settle(2);
  const float tall = r.V().list_scale;
  set_list(1.0f);
  CHECK_NEAR(tall, r.V().list_scale * 1.5f, 1e-4);
}

TEST(overlay_settings_appearance_set_by_the_game) {
  // A game that offers these in its own menu writes the same setting; the
  // overlay picks it up on its next frame.
  OverlayOptions opt;
  opt.settings_appearance = false;
  Rig r(opt);
  AppearanceSettings look;
  look.theme = "ps3";
  look.list_size = 1.3f;
  SetAppearanceSettings(r.system, look);
  r.Step();
  CHECK_EQ(r.overlay->styles().Resolve(nullptr)->theme, std::string("ps3"));
  r.overlay->Open();
  r.Settle();
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  for (const auto& id : r.V().setting_rows) CHECK(id != "theme");
}

TEST(overlay_settings_volume_and_sound) {
  std::vector<float> played;
  OverlayOptions opt;
  opt.play_sound = [&](std::shared_ptr<const Pcm>, float volume, NotificationKind) { played.push_back(volume); };
  Rig r(opt);
  r.overlay->Open();
  r.Settle();
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  r.Tap([](OverlayInput& in) { in.pad_down = true; });
  CHECK_EQ(r.V().setting_selected, std::string("volume"));
  CHECK_NEAR(GetPopupSoundSettings(r.system).volume, 0.5f, 1e-6);  // Half until the player picks.
  // Left / right step 10%, saved in the System, with a preview each time.
  r.Tap([](OverlayInput& in) { in.pad_left = true; });
  r.Tap([](OverlayInput& in) { in.left = true; });
  CHECK_NEAR(GetPopupSoundSettings(r.system).volume, 0.3f, 1e-6);
  CHECK_EQ(played.size(), 2u);
  r.Tap([](OverlayInput& in) { in.pad_right = true; });
  CHECK_NEAR(GetPopupSoundSettings(r.system).volume, 0.4f, 1e-6);
  CHECK_NEAR(r.system.GetSetting("popup_sound")["volume"].get<double>(), 0.4, 1e-6);
  CHECK(r.overlay->is_open());
  CHECK(r.V().settings);  // Left / right adjust here instead of switching tabs.
  for (int i = 0; i < 15; ++i) r.Tap([](OverlayInput& in) { in.right = true; });
  CHECK_NEAR(GetPopupSoundSettings(r.system).volume, 1.0f, 1e-6);
  r.Tap([](OverlayInput& in) { in.pad_left = true; });

  // Popups play at the style's volume times the player's.
  r.Tap([](OverlayInput& in) { in.pad_b = true; });
  r.Tap([](OverlayInput& in) { in.pad_b = true; });
  played.clear();
  r.system.Unlock("c:c1");
  r.Settle(5);
  CHECK_EQ(played.size(), 1u);
  const float style_volume = r.overlay->styles().Resolve(r.system.FindSet("c"))->sound.volume;
  CHECK(!played.empty() && std::fabs(played[0] - style_volume * 0.9f) < 1e-4f);

  // Sound off: nothing reaches the game's mixer.
  r.overlay->Open();
  r.Settle();
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  r.Tap([](OverlayInput& in) { in.pad_up = true; });
  CHECK_EQ(r.V().setting_selected, std::string("sound"));
  r.Tap([](OverlayInput& in) { in.enter = true; });
  CHECK(!GetPopupSoundSettings(r.system).enabled);
  played.clear();
  r.overlay->Close();
  r.Settle();
  r.system.Unlock("c:c2");
  r.Settle(5);
  CHECK(!r.overlay->center().Active().empty());  // The popup still shows...
  CHECK(played.empty());                          // ...silently.

  // A game's own sound_handler gets the scaled volume too.
  System sys;
  SetPopupSoundSettings(sys, {true, 0.5f});
  AchievementSet c = CustomSet();
  sys.AddSet(c);
  std::vector<float> requested;
  OverlayOptions h;
  h.global_preferences = false;
  h.sound_handler = [&](const NotificationCenter::SoundRequest& req) { requested.push_back(req.volume); };
  Overlay o(sys, h);
  o.SetSize(640, 360);
  sys.Unlock("c:c0");
  for (int i = 0; i < 5; ++i) o.Frame(1.0f / 60, {});
  CHECK_EQ(requested.size(), 1u);
  const float sv = o.styles().Resolve(sys.FindSet("c"))->sound.volume;
  CHECK(!requested.empty() && std::fabs(requested[0] - sv * 0.5f) < 1e-4f);
}

TEST(overlay_settings_reset_needs_a_hold) {
  Rig r;
  r.system.Unlock("c:c0");
  r.overlay->Open();
  r.Settle();
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  while (r.V().setting_selected != "reset:set:c") r.Tap([](OverlayInput& in) { in.pad_down = true; });
  // A press, and a press held for under the time: nothing.
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  CHECK(r.system.IsUnlocked("c:c0"));
  OverlayInput a;
  a.pad_a = true;
  for (int i = 0; i < 40; ++i) r.Step(a);  // 0.67 s.
  CHECK(r.V().hold > 0.3f && r.V().hold < 1.0f);
  r.Step();
  CHECK_EQ(r.V().hold, 0.0f);
  CHECK(r.system.IsUnlocked("c:c0"));
  // One very long frame doesn't confirm either.
  r.Step(a, 5.0f);
  CHECK(r.system.IsUnlocked("c:c0"));
  r.Step();
  // Held through: that set only.
  for (int i = 0; i < 90; ++i) r.Step(a);
  CHECK(!r.system.IsUnlocked("c:c0"));
  CHECK(r.system.IsUnlocked("x:a0"));
  // Still held: it doesn't start again.
  for (int i = 0; i < 90; ++i) r.Step(a);
  CHECK_EQ(r.V().hold, 0.0f);
  r.Step();
  // Everything, with Enter held.
  r.Tap([](OverlayInput& in) { in.pad_down = true; });
  CHECK_EQ(r.V().setting_selected, std::string("reset:all"));
  OverlayInput enter;
  enter.enter = true;
  for (int i = 0; i < 90; ++i) r.Step(enter);
  CHECK(!r.system.IsUnlocked("x:a0"));
  CHECK(r.system.GetProgress("x:a3")->completed.empty());
}

TEST(overlay_reset_chosen_achievements) {
  Rig r;
  r.system.Unlock("x:a1");
  r.overlay->Open();
  r.Settle();
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  while (r.V().setting_selected != "reset:pick") r.Tap([](OverlayInput& in) { in.pad_down = true; });
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  auto v = r.V();
  CHECK(!v.settings);
  CHECK(v.reset_mode);
  CHECK_EQ(v.selected, std::string("x:a0"));
  // A on a row doesn't expand it here, and a tap resets nothing.
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  CHECK(r.V().expanded.empty());
  CHECK(r.system.IsUnlocked("x:a0"));
  OverlayInput a;
  a.pad_a = true;
  for (int i = 0; i < 90; ++i) r.Step(a);
  CHECK(!r.system.IsUnlocked("x:a0"));
  CHECK(r.system.IsUnlocked("x:a1"));
  r.Step();
  r.Tap([](OverlayInput& in) { in.pad_down = true; });
  CHECK_EQ(r.V().selected, std::string("x:a1"));
  // B leaves reset mode for the settings page, on the row that started it.
  r.Tap([](OverlayInput& in) { in.pad_b = true; });
  v = r.V();
  CHECK(v.settings);
  CHECK(!v.reset_mode);
  CHECK_EQ(v.setting_selected, std::string("reset:pick"));
  r.Tap([](OverlayInput& in) { in.pad_b = true; });
  CHECK(!r.V().settings);
  CHECK(!r.V().reset_mode);
  // Out of reset mode, A expands again.
  while (r.V().selected != "x:a3") r.Tap([](OverlayInput& in) { in.pad_down = true; });
  r.Tap([](OverlayInput& in) { in.pad_a = true; });
  CHECK_EQ(r.V().expanded.size(), 1u);
}

TEST(overlay_settings_mouse) {
  Rig r(OverlayOptions{}, 1280, 720);
  r.overlay->Open();
  r.Settle();
  // The gear beside the title opens the page.
  bool opened = false;
  for (float x = 330; x < 560 && !opened; x += 10) {
    OverlayInput in;
    in.mouse_present = true;
    in.mouse_x = x;
    in.mouse_y = 60;
    r.Step(in);
    in.mouse_left = true;
    r.Step(in);
    in.mouse_left = false;
    r.Step(in);
    opened = r.V().settings;
  }
  CHECK(opened);
  // Hover a row, then click it.
  auto click_row = [&](const std::string& id) {
    for (float y = 150; y < 700; y += 8) {
      OverlayInput in;
      in.mouse_present = true;
      in.mouse_x = 640;
      in.mouse_y = y;
      r.Step(in);
      if (r.V().setting_selected != id) continue;
      // Let any scroll the new selection started finish under the mouse.
      for (int k = 0; k < 30; ++k) r.Step(in);
      if (r.V().setting_selected != id) continue;
      in.mouse_left = true;
      r.Step(in);
      in.mouse_left = false;
      r.Step(in);
      return true;
    }
    return false;
  };
  // A theme row steps to the next theme.
  CHECK(click_row("theme"));
  CHECK_EQ(GetAppearanceSettings(r.system).theme, std::string("auto"));
  // The second set's switch row: off, once the pad has scrolled it into view.
  // (The pad leaves the selection on set:x, so only hovering set:c selects it.)
  while (r.V().setting_selected != "set:c") r.Tap([](OverlayInput& in) { in.pad_down = true; });
  r.Tap([](OverlayInput& in) { in.pad_up = true; });
  CHECK(click_row("set:c"));
  CHECK(!r.system.IsSetEnabled("c"));
}

TEST(overlay_settings_options_hide_it) {
  OverlayOptions none;
  none.show_settings = false;
  Rig r(none);
  r.overlay->Open();
  r.Settle();
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  CHECK(!r.V().settings);
  r.overlay->OpenSettings();
  r.Settle();
  CHECK(!r.V().settings);

  OverlayOptions some;
  some.settings_sound = false;
  some.settings_appearance = false;
  some.settings_reset = false;
  Rig q(some);
  q.overlay->Open();
  q.Settle();
  q.Tap([](OverlayInput& in) { in.f[2] = true; });
  CHECK(q.V().settings);
  CHECK_EQ(q.V().setting_rows, (std::vector<std::string>{"set:x", "set:c"}));
}

TEST(overlay_settings_render) {
  // Every page draws, with both label sets and at a small size.
  for (ButtonLabels labels : {ButtonLabels::kXbox, ButtonLabels::kPlayStation}) {
    OverlayOptions opt;
    opt.buttons = labels;
    Rig r(opt, 1280, 720);
    r.overlay->Open();
    r.Settle();
    r.Tap([](OverlayInput& in) { in.pad_start = true; });
    r.Settle();
    CHECK(Any(r.overlay->image()));
    r.system.SetSetEnabled("x", false);
    r.system.SetSetEnabled("c", false);
    r.Settle();
    r.Tap([](OverlayInput& in) { in.pad_b = true; });
    r.Settle();
    CHECK(r.V().keys.empty());  // "Every achievement set is off."
    r.overlay->SetSize(320, 200);
    r.Tap([](OverlayInput& in) { in.pad_start = true; });
    r.Settle();
    CHECK(Any(r.overlay->image()));
  }
}

TEST(overlay_show_disabled_sets_lists_them_off) {
  OverlayOptions opt;
  opt.show_disabled_sets = true;
  opt.show_settings = false;
  Rig r(opt);
  r.system.SetSetEnabled("c", false);
  r.overlay->Open();
  r.Settle();
  // The off set keeps a tab and its rows, but not its points in the totals.
  CHECK_EQ(r.V().tabs, (std::vector<std::string>{"", "x", "c"}));
  CHECK_EQ(r.V().keys.size(), size_t(11));
  CHECK_EQ(r.V().keys.back(), std::string("c:c2"));
  CHECK_EQ(r.system.GetTotals().count, 8u);
  r.Tap([](OverlayInput& in) { in.right = true; });
  r.Tap([](OverlayInput& in) { in.right = true; });
  CHECK_EQ(r.V().tab, std::string("c"));
  CHECK_EQ(r.V().keys.size(), size_t(3));

  // Without the option the off set is gone from the viewer.
  Rig plain;
  plain.system.SetSetEnabled("c", false);
  plain.overlay->Open();
  plain.Settle();
  CHECK_EQ(plain.V().tabs, (std::vector<std::string>{"x"}));
}

TEST(overlay_host_button) {
  OverlayOptions opt;
  opt.show_settings = false;
  Rig r(opt);
  int presses = 0;
  r.overlay->SetHostButton("Profile: one", "Profile", [&] {
    ++presses;
    r.overlay->SetHostButton("Profile: two", "Profile", [&] { ++presses; });
  });
  r.overlay->Open();
  r.Settle();
  CHECK_EQ(r.V().host_button, std::string("Profile: one"));
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  CHECK_EQ(presses, 1);
  CHECK_EQ(r.V().host_button, std::string("Profile: two"));
  r.Tap([](OverlayInput& in) { in.f[2] = true; });
  CHECK_EQ(presses, 2);
  CHECK(r.overlay->is_open());
  r.overlay->SetHostButton("", "", nullptr);
  r.Tap([](OverlayInput& in) { in.pad_start = true; });
  CHECK_EQ(presses, 2);
  CHECK(r.V().host_button.empty());
}
