#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/painter.h"
#include "recomp/achievements/ui/style.h"
#include "test.h"

using namespace recomp::achievements;
using namespace recomp::achievements::ui;
using nlohmann::json;

namespace {

std::filesystem::path TempDir(const char* name) {
  auto dir = std::filesystem::temp_directory_path() / "recomp_ach_ui_tests" / name;
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

AchievementSet MakeSet(const char* id, Platform platform) {
  AchievementSet s;
  s.id = id;
  s.platform = platform;
  return s;
}

}  // namespace

TEST(style_builtin_themes) {
  for (const auto& name : BuiltinThemeNames()) {
    bool found = false;
    const Style s = BuiltinTheme(name, &found);
    CHECK(found);
    CHECK(s.theme == name);
    CHECK(s.unlock.timing.total() > 1.0f);
    CHECK(s.progress.timing.total() > 0.5f);
    CHECK(!s.sound.unlock.empty());
    CHECK(!s.sound.progress.empty());
  }
  bool found = true;
  CHECK(BuiltinTheme("nope", &found).theme == "minimal");
  CHECK(!found);
  CHECK(BuiltinTheme("XBOX360").theme == "xbox360");
  CHECK(BuiltinTheme("classic").theme == "classic");
  // Themes really differ.
  CHECK(!(BuiltinTheme("xbox360") == BuiltinTheme("ps3")));
  CHECK(BuiltinTheme("ps3").sound.gold != BuiltinTheme("ps3").sound.bronze);
}

TEST(style_default_theme_by_platform) {
  CHECK(DefaultThemeFor(Platform::kXbox360) == "xbox360");
  CHECK(DefaultThemeFor(Platform::kPlayStation3) == "ps3");
  CHECK(DefaultThemeFor(Platform::kRetroAchievements) == "retro");
  CHECK(DefaultThemeFor(Platform::kCustom) == "minimal");
}

TEST(style_parse_color) {
  Color c;
  CHECK(ParseColor("#FF8000", c));
  CHECK_NEAR(c.r, 1.0, 1e-6);
  CHECK_NEAR(c.g, 128 / 255.0, 1e-6);
  CHECK_NEAR(c.b, 0.0, 1e-6);
  CHECK_NEAR(c.a, 1.0, 1e-6);
  CHECK(ParseColor("ff800080", c));
  CHECK_NEAR(c.a, 128 / 255.0, 1e-6);
  CHECK(ParseColor(json::array({10, 20, 30}), c));
  CHECK_NEAR(c.b, 30 / 255.0, 1e-6);
  CHECK(ParseColor(json::array({10, 20, 30, 0}), c));
  CHECK_NEAR(c.a, 0.0, 1e-6);
  CHECK(!ParseColor("#12345", c));
  CHECK(!ParseColor("#GG0000", c));
  CHECK(!ParseColor(json::array({1, 2}), c));
  CHECK(!ParseColor(5, c));
  CHECK(ColorToHex(Color::Rgba(1, 2, 255, 128)) == "#0102FF80");
}

TEST(style_patch_is_field_by_field) {
  Style s = BuiltinTheme("classic");
  Style expected = s;
  std::vector<std::string> warnings;
  ApplyStyleJson(s, json::parse(R"({
    "unlock": { "anchor": "top-right", "offset": [10, 20], "colors": { "border": "#FF0000" },
                "timing": { "hold": 6 } },
    "progress": { "enabled": false, "lines": { "bar": false } },
    "sound": { "volume": 0.25 }
  })"), {}, &warnings);
  CHECK(warnings.empty());
  expected.unlock.anchor = Anchor::kTopRight;
  expected.unlock.offset_x = 10;
  expected.unlock.offset_y = 20;
  expected.unlock.colors.border = Color::Rgba(255, 0, 0);
  expected.unlock.timing.hold = 6;
  expected.progress_enabled = false;
  expected.progress.lines.bar = false;
  expected.sound.volume = 0.25f;
  CHECK(s == expected);
}

TEST(style_patch_warnings_keep_old_values) {
  Style s = BuiltinTheme("minimal");
  const Style before = s;
  std::vector<std::string> w;
  ApplyStyleJson(s, json::parse(R"({
    "unlock": { "width": "wide", "anchor": "upstairs", "bogus": 1, "colors": { "name": "red" } },
    "sound": { "volume": "loud" },
    "nonsense": true,
    "_comment": "ignored",
    "theme": "ps3"
  })"), {}, &w);
  CHECK(s == before);
  CHECK_EQ(w.size(), 6u);
}

TEST(style_patch_clamps) {
  Style s;
  ApplyStyleJson(s, json::parse(R"({"sound": {"volume": 7}, "unlock": {"max_visible": 0, "width": -5}})"));
  CHECK_NEAR(s.sound.volume, 1.0, 1e-6);
  CHECK_EQ(s.unlock.max_visible, 1);
  CHECK(s.unlock.width >= 40.0f);
}

TEST(style_json_round_trip) {
  for (const auto& name : BuiltinThemeNames()) {
    const Style theme = BuiltinTheme(name);
    Style s = BuiltinTheme(name == "minimal" ? "retro" : "minimal");
    std::vector<std::string> w;
    ApplyStyleJson(s, StyleToJson(theme), {}, &w);
    s.theme = theme.theme;  // "theme" is the manager's, not a field patch.
    CHECK(w.empty());
    CHECK(s == theme);
  }
}

TEST(style_sound_paths_resolve_from_file) {
  Style s;
  ApplyStyleJson(s, json::parse(R"({"sound": {"unlock": "sounds/pop.wav", "progress": "builtin:retro_tick",
                                              "gold": null, "silver": "/abs/x.wav"}})"),
                 "/games/mygame");
  CHECK(std::filesystem::path(s.sound.unlock) == std::filesystem::path("/games/mygame/sounds/pop.wav"));
  CHECK(s.sound.progress == "builtin:retro_tick");
  CHECK(s.sound.gold == "none");
  CHECK(s.sound.silver == "/abs/x.wav");
  CHECK(s.UnlockSound(Tier::kGold) == "none");
  CHECK(s.UnlockSound(Tier::kBronze) == s.sound.unlock);
}

TEST(style_format_text) {
  std::map<std::string, std::string, std::less<>> v{{"name", "Pro"}, {"points", ""}, {"tier", "Gold"}};
  CHECK(FormatText("[{points}G - ]{name}", v) == "Pro");
  v["points"] = "15";
  CHECK(FormatText("[{points}G - ]{name}", v) == "15G - Pro");
  CHECK(FormatText("{name} {unknown}", v) == "Pro {unknown}");
  CHECK(FormatText("[{tier}] trophy", v) == "Gold trophy");
  CHECK(FormatText("[no tokens]", v) == "no tokens");
  CHECK(FormatText("{name", v) == "{name");
  CHECK(FormatText("[{name}", v) == "[Pro");
}

TEST(style_manager_layers) {
  StyleManager m;
  m.SetGameStyle(json::parse(R"({
    "theme": "classic",
    "unlock": { "width": 500 },
    "sound": { "volume": 0.9 },
    "platforms": { "ps3": { "unlock": { "offset_y": 11 } } },
    "sets": { "dlc": { "unlock": { "offset_x": 33 } } }
  })"));
  m.SetUserStyle(json::parse(R"({
    "sound": { "volume": 0.3 },
    "sets": { "dlc": { "unlock": { "width": 600 } } }
  })"));

  auto base = MakeSet("base", Platform::kCustom);
  auto s = m.Resolve(&base);
  CHECK(s->theme == "classic");
  CHECK_NEAR(s->unlock.width, 500, 1e-6);
  CHECK_NEAR(s->sound.volume, 0.3, 1e-6);  // User wins.
  CHECK_NEAR(s->unlock.offset_y, BuiltinTheme("classic").unlock.offset_y, 1e-6);

  auto ps3 = MakeSet("trophies", Platform::kPlayStation3);
  s = m.Resolve(&ps3);
  CHECK(s->theme == "classic");  // The game picked a theme for every set.
  CHECK_NEAR(s->unlock.offset_y, 11, 1e-6);

  auto dlc = MakeSet("dlc", Platform::kCustom);
  s = m.Resolve(&dlc);
  CHECK_NEAR(s->unlock.offset_x, 33, 1e-6);
  CHECK_NEAR(s->unlock.width, 600, 1e-6);

  CHECK(m.Resolve(&dlc) == s);  // Cached.
  CHECK(m.warnings().empty());
}

TEST(style_manager_theme_choice_skips_lower_layers) {
  StyleManager m;
  m.SetGameStyle(json::parse(R"({"theme": "classic", "unlock": {"width": 500}, "sound": {"volume": 0.9}})"));
  m.SetUserStyle(json::parse(R"({"theme": "retro", "unlock": {"rounding": 4}})"));
  auto s = m.Resolve(nullptr);
  CHECK(s->theme == "retro");
  CHECK_NEAR(s->unlock.width, BuiltinTheme("retro").unlock.width, 1e-6);
  CHECK_NEAR(s->sound.volume, BuiltinTheme("retro").sound.volume, 1e-6);
  CHECK_NEAR(s->unlock.rounding, 4, 1e-6);

  // "auto" goes back to the platform's theme.
  m.SetUserStyle(json::parse(R"({"platforms": {"xbox360": {"theme": "auto"}}})"));
  auto x = MakeSet("x", Platform::kXbox360);
  CHECK(m.Resolve(&x)->theme == "xbox360");
  auto c = MakeSet("c", Platform::kCustom);
  CHECK(m.Resolve(&c)->theme == "classic");
}

TEST(style_manager_default_by_platform) {
  StyleManager m;
  auto r = MakeSet("r", Platform::kRetroAchievements);
  CHECK(m.Resolve(&r)->theme == "retro");
  CHECK(m.Resolve("p", Platform::kPlayStation3)->theme == "ps3");
  CHECK(m.Resolve(nullptr)->theme == "minimal");
}

TEST(style_manager_hook_and_override) {
  StyleManager m;
  m.SetGameStyle(json::parse(R"({"theme": "ps3", "unlock": {"width": 500}})"));
  int calls = 0;
  m.SetGameHook([&](Style& s, const AchievementSet* set) {
    ++calls;
    s.unlock.width = 650;
    s.unlock.padding = 3;
    if (set) s.unlock.text.title = set->id;
  });
  m.SetUserStyle(json::parse(R"({"unlock": {"padding": 9}})"));
  auto set = MakeSet("hooked", Platform::kCustom);
  auto s = m.Resolve(&set);
  CHECK_EQ(calls, 1);
  CHECK_NEAR(s->unlock.width, 650, 1e-6);  // Hook beats game JSON...
  CHECK_NEAR(s->unlock.padding, 9, 1e-6);  // ...and user JSON beats the hook.
  CHECK(s->unlock.text.title == "hooked");

  const uint64_t gen = m.generation();
  m.SetThemeOverride("xbox360");
  CHECK(m.generation() > gen);
  s = m.Resolve(&set);
  CHECK(s->theme == "xbox360");
  CHECK_NEAR(s->unlock.width, BuiltinTheme("xbox360").unlock.width, 1e-6);
  CHECK_NEAR(s->unlock.padding, 9, 1e-6);  // User tweaks survive a forced theme.
  m.SetThemeOverride("");
  CHECK(m.Resolve(&set)->theme == "ps3");
}

TEST(style_manager_files) {
  const auto dir = TempDir("style_files");
  StyleManager m;
  std::string err;
  CHECK(m.LoadUserStyleFile(dir / "missing.json", &err));  // Optional file.
  CHECK(m.Resolve(nullptr)->theme == "minimal");

  {
    std::ofstream f(dir / "achievements_style.json");
    f << "// written by hand\n{ \"theme\": \"retro\", /* inline */ \"sound\": {\"unlock\": \"my.wav\"} }\n";
  }
  CHECK(m.LoadUserStyleFile(dir / "achievements_style.json", &err));
  auto s = m.Resolve(nullptr);
  CHECK(s->theme == "retro");
  CHECK(std::filesystem::path(s->sound.unlock) == (dir / "my.wav").lexically_normal());

  {
    std::ofstream f(dir / "achievements_style.json");
    f << "{ \"theme\": \"ps3\" }";
  }
  CHECK(m.Reload(&err));
  CHECK(m.Resolve(nullptr)->theme == "ps3");

  {
    std::ofstream f(dir / "broken.json");
    f << "{ not json";
  }
  CHECK(!m.LoadGameStyleFile(dir / "broken.json", &err));
  CHECK(!err.empty());
}

TEST(style_manager_unknown_theme_warns) {
  StyleManager m;
  m.SetUserStyle(json::parse(R"({"theme": "dreamcast", "unlock": {"widht": 3}})"));
  CHECK(m.Resolve(nullptr)->theme == "minimal");
  const auto w = m.warnings();
  CHECK_EQ(w.size(), 2u);
}

#ifdef RECOMP_ACH_UI_STYLING_DOC
TEST(style_doc_examples_are_valid) {
  std::ifstream in(RECOMP_ACH_UI_STYLING_DOC);
  CHECK(static_cast<bool>(in));
  std::string line, block;
  bool inside = false;
  int checked = 0;
  while (std::getline(in, line)) {
    if (!inside && line.rfind("```json", 0) == 0) {
      inside = true;
      block.clear();
    } else if (inside && line.rfind("```", 0) == 0) {
      inside = false;
      // Fragments like `"sound": {...}` are wrapped into an object.
      json j = json::parse(block, nullptr, false, true);
      if (j.is_discarded()) j = json::parse("{" + block + "}", nullptr, false, true);
      CHECK(j.is_object());
      StyleManager m;
      m.SetUserStyle(j);
      for (Platform p : {Platform::kCustom, Platform::kXbox360, Platform::kPlayStation3, Platform::kRetroAchievements})
        m.Resolve("my_game", p);
      for (const auto& w : m.warnings()) std::printf("  doc example: %s\n", w.c_str());
      CHECK(m.warnings().empty());
      ++checked;
    } else if (inside) {
      block += line + "\n";
    }
  }
  CHECK(checked >= 5);
}
#endif

TEST(style_manager_global_preferences) {
  StyleManager m;
  m.SetGameStyle(json::parse(R"({"theme": "classic", "unlock": {"width": 500}, "sound": {"volume": 0.9}})"));
  m.SetGameHook([](Style& s, const AchievementSet*) { s.unlock.padding = 3; });
  // The player's global choice beats the game's style and hook...
  m.SetGlobalStyle(json::parse(R"({"theme": "xbox360", "sound": {"volume": 0.2}, "unlock": {"padding": 7}})"));
  auto s = m.Resolve(nullptr);
  CHECK(s->theme == "xbox360");
  CHECK_NEAR(s->sound.volume, 0.2, 1e-6);
  CHECK_NEAR(s->unlock.padding, 7, 1e-6);
  CHECK_NEAR(s->unlock.width, BuiltinTheme("xbox360").unlock.width, 1e-6);  // Game layer skipped by the theme.

  // ...and this game's own user style beats the global one.
  m.SetUserStyle(json::parse(R"({"theme": "retro", "sound": {"volume": 0.5}})"));
  s = m.Resolve(nullptr);
  CHECK(s->theme == "retro");
  CHECK_NEAR(s->sound.volume, 0.5, 1e-6);

  // Without a theme of its own the global layer only tweaks the game's choice.
  StyleManager t;
  t.SetGameStyle(json::parse(R"({"theme": "classic"})"));
  t.SetGlobalStyle(json::parse(R"({"sound": {"enabled": false}})"));
  auto u = t.Resolve(nullptr);
  CHECK(u->theme == "classic");
  CHECK(!u->sound.enabled);
}

#if !defined(_WIN32)
TEST(global_config_dir_and_template) {
  const auto dir = std::filesystem::temp_directory_path() / "recomp_ach_ui_tests" / "global";
  std::filesystem::remove_all(dir);
  setenv("RECOMP_ACHIEVEMENTS_CONFIG_DIR", dir.c_str(), 1);
  CHECK(GlobalConfigDir() == dir);
  CHECK(EnsureGlobalConfigDir() == dir);
  CHECK(std::filesystem::is_directory(dir / "sounds"));
  // The starter file parses and changes nothing.
  StyleManager m;
  m.SetGameStyle(json::parse(R"({"theme": "ps3"})"));
  std::string error;
  CHECK(m.LoadGlobalStyleFile(dir / "achievements_style.json", &error));
  CHECK(m.Resolve(nullptr)->theme == "ps3");
  CHECK(m.warnings().empty());
  // An existing file is left alone.
  std::ofstream(dir / "achievements_style.json") << R"({"theme": "retro"})";
  EnsureGlobalConfigDir();
  CHECK(m.LoadGlobalStyleFile(dir / "achievements_style.json", &error));
  CHECK(m.Resolve(nullptr)->theme == "retro");

  unsetenv("RECOMP_ACHIEVEMENTS_CONFIG_DIR");
  setenv("XDG_CONFIG_HOME", "/tmp/xdg-test", 1);
  CHECK(GlobalConfigDir() == std::filesystem::path("/tmp/xdg-test/recomp-achievements"));
  unsetenv("XDG_CONFIG_HOME");
  const char* home = std::getenv("HOME");
  const std::string saved_home = home ? home : "";
  setenv("HOME", "/home/someone", 1);
  CHECK(GlobalConfigDir() == std::filesystem::path("/home/someone/.config/recomp-achievements"));
  if (home) setenv("HOME", saved_home.c_str(), 1);
}
#endif

TEST(style_small_screens_keep_their_size) {
  StyleManager m;
  auto st = m.Resolve(nullptr);
  const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
  // A Steam Deck's 800 lines stay at 1 instead of 800 / 1080.
  CHECK(near(PopupScale(*st, {1280, 800}), 1.0f));
  CHECK(near(PopupScale(*st, {1280, 720}), 0.9f));  // Under it, in proportion.
  CHECK(near(PopupScale(*st, {1920, 1080}), 1.0f));
  CHECK(near(PopupScale(*st, {2560, 1440}), 1440.0f / 1080.0f));
  CHECK(near(SmallScreenScale(*st, 1080), 1.0f));  // Above it the height rule wins anyway.
  // The player can make handheld popups larger, or turn the rule off.
  m.SetGlobalStyle(nlohmann::json::parse(R"({"small_screen_scale": 1.3})"));
  CHECK(near(PopupScale(*m.Resolve(nullptr), {1280, 800}), 1.3f));
  m.SetGlobalStyle(nlohmann::json::parse(R"({"small_screen_height": 0})"));
  CHECK(near(PopupScale(*m.Resolve(nullptr), {1280, 800}), 800.0f / 1080.0f));
}

TEST(appearance_settings_round_trip_and_steps) {
  System system;
  CHECK(GetAppearanceSettings(system) == AppearanceSettings{});
  AppearanceSettings look;
  look.theme = "retro";
  look.popup_size = 1.3f;
  look.list_size = 0.85f;
  SetAppearanceSettings(system, look);
  CHECK(GetAppearanceSettings(system) == look);
  // Out-of-range sizes in the file are clamped to the offered ones.
  system.SetSetting(kAppearanceSetting, {{"theme", "ps3"}, {"popup_size", 9.0}, {"list_size", 0.1}});
  CHECK_NEAR(GetAppearanceSettings(system).popup_size, 1.5f, 1e-6);
  CHECK_NEAR(GetAppearanceSettings(system).list_size, 0.75f, 1e-6);

  CHECK_NEAR(StepAppearanceSize(1.0f, 1), 1.15f, 1e-6);
  CHECK_NEAR(StepAppearanceSize(1.0f, -1), 0.85f, 1e-6);
  CHECK_NEAR(StepAppearanceSize(1.5f, 1), 1.5f, 1e-6);
  CHECK_NEAR(StepAppearanceSize(1.07f, 0), 1.0f, 1e-6);  // Snaps to the nearest.
  CHECK_EQ(StepAppearanceTheme("", 1), std::string("auto"));
  CHECK_EQ(StepAppearanceTheme("", -1), std::string("classic"));
  CHECK_EQ(StepAppearanceTheme("classic", 1), std::string(""));
  CHECK_EQ(AppearanceThemeName(""), std::string("Game's choice"));
  CHECK_EQ(AppearanceThemeName("ps3"), std::string("PlayStation 3"));
}

TEST(player_appearance_beats_the_game_and_the_override) {
  StyleManager m;
  m.SetGameStyle({{"theme", "retro"}, {"scale", 1.2}});
  m.SetThemeOverride("minimal");
  CHECK_EQ(m.Resolve(nullptr)->theme, std::string("minimal"));
  const uint64_t gen = m.generation();
  AppearanceSettings look;
  look.theme = "xbox360";
  look.popup_size = 1.5f;
  look.list_size = 0.75f;
  m.SetPlayerAppearance(look);
  CHECK(m.generation() != gen);
  auto st = m.Resolve(nullptr);
  CHECK_EQ(st->theme, std::string("xbox360"));
  CHECK_NEAR(st->scale, BuiltinTheme("xbox360").scale * 1.5f, 1e-5);
  CHECK_NEAR(st->viewer.scale, BuiltinTheme("xbox360").viewer.scale * 0.75f, 1e-5);
  // The same choice again changes nothing.
  const uint64_t gen2 = m.generation();
  m.SetPlayerAppearance(look);
  CHECK_EQ(m.generation(), gen2);
  // "Game's choice" with default sizes leaves the game's style (and the override) alone.
  m.SetPlayerAppearance({});
  CHECK_EQ(m.Resolve(nullptr)->theme, std::string("minimal"));
  m.SetThemeOverride("");
  st = m.Resolve(nullptr);
  CHECK_EQ(st->theme, std::string("retro"));
  CHECK_NEAR(st->scale, 1.2f, 1e-5);
  // "By platform": the platform's default theme.
  look = {};
  look.theme = "auto";
  m.SetPlayerAppearance(look);
  CHECK_EQ(m.Resolve("s", Platform::kPlayStation3)->theme, std::string(DefaultThemeFor(Platform::kPlayStation3)));
}
