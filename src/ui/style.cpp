#include "recomp/achievements/ui/style.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <set>

namespace recomp::achievements::ui {

using nlohmann::json;

// ---- Style helpers ---------------------------------------------------------

const Color& Style::TierColor(Tier tier, const Color& fallback) const {
  switch (tier) {
    case Tier::kBronze: return tiers.bronze;
    case Tier::kSilver: return tiers.silver;
    case Tier::kGold: return tiers.gold;
    case Tier::kPlatinum: return tiers.platinum;
    default: return fallback;
  }
}

const std::string& Style::TierName(Tier tier) const {
  static const std::string kEmpty;
  switch (tier) {
    case Tier::kBronze: return tiers.bronze_name;
    case Tier::kSilver: return tiers.silver_name;
    case Tier::kGold: return tiers.gold_name;
    case Tier::kPlatinum: return tiers.platinum_name;
    default: return kEmpty;
  }
}

const std::string& Style::UnlockSound(Tier tier) const {
  const std::string* s = nullptr;
  switch (tier) {
    case Tier::kBronze: s = &sound.bronze; break;
    case Tier::kSilver: s = &sound.silver; break;
    case Tier::kGold: s = &sound.gold; break;
    case Tier::kPlatinum: s = &sound.platinum; break;
    default: break;
  }
  return (s && !s->empty()) ? *s : sound.unlock;
}

// ---- Enums -----------------------------------------------------------------

namespace {

struct AnchorName_ {
  Anchor a;
  const char* name;
};
constexpr AnchorName_ kAnchors[] = {
    {Anchor::kTopLeft, "top_left"},       {Anchor::kTopCenter, "top_center"},
    {Anchor::kTopRight, "top_right"},     {Anchor::kMiddleLeft, "middle_left"},
    {Anchor::kCenter, "center"},          {Anchor::kMiddleRight, "middle_right"},
    {Anchor::kBottomLeft, "bottom_left"}, {Anchor::kBottomCenter, "bottom_center"},
    {Anchor::kBottomRight, "bottom_right"},
};

struct AnimationName_ {
  Animation a;
  const char* name;
};
constexpr AnimationName_ kAnimations[] = {
    {Animation::kNone, "none"},     {Animation::kFade, "fade"}, {Animation::kSlide, "slide"},
    {Animation::kExpand, "expand"}, {Animation::kPop, "pop"},
};

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '-' || c == ' ') c = '_';
  }
  return out;
}

}  // namespace

bool ParseAnchor(std::string_view s, Anchor& out) {
  std::string l = Lower(s);
  // Accept "centre" and "bottom" style shorthands too.
  if (l == "centre" || l == "middle") l = "center";
  if (l == "top") l = "top_center";
  if (l == "bottom") l = "bottom_center";
  if (l == "left") l = "middle_left";
  if (l == "right") l = "middle_right";
  if (auto p = l.find("centre"); p != std::string::npos) l.replace(p, 6, "center");
  for (const auto& a : kAnchors) {
    if (l == a.name) {
      out = a.a;
      return true;
    }
  }
  return false;
}

std::string_view AnchorName(Anchor a) {
  for (const auto& e : kAnchors)
    if (e.a == a) return e.name;
  return "bottom_center";
}

bool ParseAnimation(std::string_view s, Animation& out) {
  const std::string l = Lower(s);
  for (const auto& a : kAnimations) {
    if (l == a.name) {
      out = a.a;
      return true;
    }
  }
  return false;
}

std::string_view AnimationName(Animation a) {
  for (const auto& e : kAnimations)
    if (e.a == a) return e.name;
  return "slide";
}

bool ParseColor(const json& j, Color& out) {
  auto clamp255 = [](double v) { return static_cast<float>(std::clamp(v, 0.0, 255.0) / 255.0); };
  if (j.is_string()) {
    std::string s = j.get<std::string>();
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8) return false;
    unsigned v[4] = {0, 0, 0, 255};
    for (size_t i = 0; i < s.size() / 2; ++i) {
      unsigned byte = 0;
      for (int k = 0; k < 2; ++k) {
        const char c = s[i * 2 + k];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        byte = byte * 16 + static_cast<unsigned>(d);
      }
      v[i] = byte;
    }
    out = Color::Rgba(v[0], v[1], v[2], v[3]);
    return true;
  }
  if (j.is_array() && (j.size() == 3 || j.size() == 4)) {
    for (const auto& e : j)
      if (!e.is_number()) return false;
    out.r = clamp255(j[0].get<double>());
    out.g = clamp255(j[1].get<double>());
    out.b = clamp255(j[2].get<double>());
    out.a = j.size() == 4 ? clamp255(j[3].get<double>()) : 1.0f;
    return true;
  }
  return false;
}

std::string ColorToHex(const Color& c) {
  auto b = [](float v) { return static_cast<unsigned>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  char buf[16];
  std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", b(c.r), b(c.g), b(c.b), b(c.a));
  return buf;
}

namespace {

// Expands tokens in fmt[begin, end); sets *had_empty when a known token was "".
std::string ExpandTokens(std::string_view fmt, const std::map<std::string, std::string, std::less<>>& values,
                         bool* had_empty) {
  std::string out;
  size_t i = 0;
  while (i < fmt.size()) {
    if (fmt[i] == '{') {
      const size_t close = fmt.find('}', i + 1);
      if (close != std::string_view::npos) {
        const std::string_view token = fmt.substr(i + 1, close - i - 1);
        if (auto it = values.find(token); it != values.end()) {
          if (it->second.empty() && had_empty) *had_empty = true;
          out += it->second;
          i = close + 1;
          continue;
        }
      }
    }
    out += fmt[i++];
  }
  return out;
}

}  // namespace

std::string FormatText(std::string_view fmt, const std::map<std::string, std::string, std::less<>>& values) {
  // "[...]" is an optional part, dropped when any token inside it is empty,
  // so "[{points}G - ]{name}" reads "Name" for a 0-point achievement.
  std::string out;
  out.reserve(fmt.size() + 16);
  size_t i = 0;
  while (i < fmt.size()) {
    const size_t open = fmt.find('[', i);
    const size_t close = open == std::string_view::npos ? open : fmt.find(']', open + 1);
    if (open == std::string_view::npos || close == std::string_view::npos) {
      out += ExpandTokens(fmt.substr(i), values, nullptr);
      break;
    }
    out += ExpandTokens(fmt.substr(i, open - i), values, nullptr);
    bool had_empty = false;
    std::string group = ExpandTokens(fmt.substr(open + 1, close - open - 1), values, &had_empty);
    if (!had_empty) out += group;
    i = close + 1;
  }
  return out;
}

// ---- Themes ----------------------------------------------------------------

namespace {

// Shared progress-popup defaults: "<item>  3/14" over a bar and the parent name.
void ProgressDefaults(PopupStyle& p) {
  p.timing = {0.2f, 2.0f, 0.3f};
  p.min_height = 64.0f;
  p.width = 320.0f;
  p.icon_size = 40.0f;
  p.lines = {true, true, true, false, true, true};
  p.text = {"{item}", "{name}", "{current}/{total}", ""};
  p.title_size = 14.0f;
  p.name_size = 12.0f;
  p.points_size = 14.0f;
  p.max_visible = 1;
}

Style Xbox360Theme() {
  Style s;
  s.theme = "xbox360";
  auto& u = s.unlock;
  u.anchor = Anchor::kBottomCenter;
  u.offset_y = 70.0f;
  u.width = 420.0f;
  u.min_height = 68.0f;
  u.padding = 10.0f;
  u.rounding = 34.0f;  // Capsule.
  u.border_thickness = 0.0f;
  u.icon_size = 48.0f;
  u.icon_shape = IconShape::kCircle;
  u.colors.background = Color::Rgba(28, 28, 30, 238);
  u.colors.border = Color::Rgba(255, 255, 255, 0);
  u.colors.title = Color::Rgba(225, 225, 225);
  u.colors.name = Color::Rgba(255, 255, 255);
  u.colors.accent = Color::Rgba(92, 176, 54);
  u.colors.points = Color::Rgba(255, 255, 255);
  u.colors.icon_border = Color::Rgba(255, 255, 255, 200);
  u.lines = {true, true, false, false, true, false};
  u.text = {"Achievement unlocked", "[{points}G - ]{name}", "", ""};
  u.title_size = 14.0f;
  u.name_size = 16.0f;
  u.timing = {0.5f, 4.5f, 0.5f};
  u.animation = Animation::kExpand;

  auto& p = s.progress;
  p = u;
  ProgressDefaults(p);
  p.offset_y = 150.0f;
  p.rounding = 28.0f;
  p.width = 340.0f;
  p.min_height = 56.0f;
  p.colors.bar_track = Color::Rgba(70, 70, 72, 220);
  p.colors.bar_fill = Color::Rgba(92, 176, 54);
  p.animation = Animation::kFade;

  s.sound.unlock = "builtin:xbox360";
  s.sound.progress = "builtin:tick";
  return s;
}

Style Ps3Theme() {
  Style s;
  s.theme = "ps3";
  auto& u = s.unlock;
  u.anchor = Anchor::kTopRight;
  u.offset_x = 28.0f;
  u.offset_y = 28.0f;
  u.width = 380.0f;
  u.min_height = 66.0f;
  u.padding = 9.0f;
  u.rounding = 3.0f;
  u.border_thickness = 1.0f;
  u.icon_size = 48.0f;
  u.icon_rounding = 2.0f;
  u.colors.background = Color::Rgba(12, 12, 14, 215);
  u.colors.border = Color::Rgba(255, 255, 255, 45);
  u.colors.shadow = Color::Rgba(0, 0, 0, 90);
  u.colors.title = Color::Rgba(235, 235, 235);
  u.colors.name = Color::Rgba(255, 255, 255);
  u.colors.points = Color::Rgba(235, 235, 235);
  u.colors.accent = Color::Rgba(120, 140, 170);
  u.lines = {true, true, true, false, true, false};
  u.text = {"You have earned a trophy.", "{name}", "{tier}", ""};
  u.title_size = 13.0f;
  u.name_size = 16.0f;
  u.points_size = 13.0f;
  u.timing = {0.3f, 4.2f, 0.5f};
  u.animation = Animation::kFade;
  u.tier_colors = true;

  auto& p = s.progress;
  p = u;
  ProgressDefaults(p);
  p.offset_y = 110.0f;
  p.colors.bar_track = Color::Rgba(60, 60, 64, 220);
  p.colors.bar_fill = Color::Rgba(80, 140, 230);
  p.animation = Animation::kFade;

  s.sound.unlock = "builtin:ps3_bronze";
  s.sound.bronze = "builtin:ps3_bronze";
  s.sound.silver = "builtin:ps3_silver";
  s.sound.gold = "builtin:ps3_gold";
  s.sound.platinum = "builtin:ps3_platinum";
  s.sound.progress = "builtin:ps3_tick";
  return s;
}

Style RetroTheme() {
  Style s;
  s.theme = "retro";
  auto& u = s.unlock;
  u.anchor = Anchor::kBottomLeft;
  u.offset_x = 16.0f;
  u.offset_y = 16.0f;
  u.width = 400.0f;
  u.min_height = 72.0f;
  u.padding = 10.0f;
  u.rounding = 0.0f;
  u.border_thickness = 2.0f;
  u.icon_size = 56.0f;
  u.icon_rounding = 0.0f;
  u.colors.background = Color::Rgba(22, 22, 40, 240);
  u.colors.border = Color::Rgba(233, 196, 106, 255);
  u.colors.title = Color::Rgba(233, 196, 106);
  u.colors.name = Color::Rgba(255, 255, 255);
  u.colors.description = Color::Rgba(180, 180, 205);
  u.colors.points = Color::Rgba(233, 196, 106);
  u.colors.accent = Color::Rgba(233, 196, 106);
  u.colors.icon_border = Color::Rgba(233, 196, 106, 255);
  u.lines = {true, true, true, true, true, false};
  u.text = {"Achievement Unlocked", "{name}", "[{points} pts]", "{description}"};
  u.uppercase_title = false;
  u.timing = {0.25f, 4.0f, 0.35f};
  u.animation = Animation::kSlide;
  u.slide_distance = 40.0f;
  u.max_visible = 3;
  u.spacing = 6.0f;

  auto& p = s.progress;
  p = u;
  ProgressDefaults(p);
  p.anchor = Anchor::kBottomRight;
  p.width = 280.0f;
  p.icon_size = 36.0f;
  p.min_height = 52.0f;
  p.lines.name = true;
  p.colors.bar_track = Color::Rgba(50, 50, 80, 230);
  p.colors.bar_fill = Color::Rgba(233, 196, 106);
  p.animation = Animation::kSlide;

  s.sound.unlock = "builtin:retro";
  s.sound.progress = "builtin:retro_tick";
  return s;
}

Style MinimalTheme() {
  Style s;  // The struct defaults are the minimal look.
  s.theme = "minimal";
  auto& u = s.unlock;
  u.anchor = Anchor::kBottomRight;
  u.offset_x = 24.0f;
  u.offset_y = 24.0f;
  u.width = 360.0f;
  u.rounding = 10.0f;
  u.border_thickness = 0.0f;
  u.colors.background = Color::Rgba(24, 24, 27, 230);
  u.colors.accent = Color::Rgba(64, 64, 72);
  u.colors.title = Color::Rgba(160, 160, 170);
  u.colors.points = Color::Rgba(160, 160, 170);
  u.lines = {true, true, true, true, true, false};
  u.text = {"Achievement unlocked", "{name}", "[{points} pts][{tier}]", "{description}"};
  u.title_size = 11.0f;
  u.uppercase_title = true;
  u.timing = {0.3f, 4.0f, 0.4f};
  u.animation = Animation::kSlide;
  u.slide_distance = 30.0f;
  u.max_visible = 3;

  auto& p = s.progress;
  p = u;
  ProgressDefaults(p);
  p.uppercase_title = false;
  p.colors.bar_track = Color::Rgba(55, 55, 60, 220);
  p.colors.bar_fill = Color::Rgba(230, 230, 235);
  p.animation = Animation::kFade;

  s.sound.unlock = "builtin:minimal";
  s.sound.progress = "builtin:tick";
  return s;
}

// The My Game recomp's original look and sounds.
Style ClassicTheme() {
  Style s;
  s.theme = "classic";
  const Color lime = Color::Rgba(213, 235, 10);
  auto& u = s.unlock;
  u.anchor = Anchor::kBottomCenter;
  u.offset_y = 65.0f;
  u.width = 480.0f;
  u.min_height = 76.0f;
  u.padding = 12.0f;
  u.rounding = 8.0f;
  u.border_thickness = 1.5f;
  u.icon_size = 48.0f;
  u.icon_rounding = 6.0f;
  u.colors.background = Color::Rgba(10, 14, 16, 238);
  u.colors.border = Color::Rgba(213, 235, 10, 220);
  u.colors.shadow = Color::Rgba(0, 0, 0, 140);
  u.colors.title = lime;
  u.colors.name = Color::Rgba(250, 252, 252);
  u.colors.description = Color::Rgba(195, 206, 210, 245);
  u.colors.points = lime;
  u.colors.accent = Color::Rgba(16, 50, 46);
  u.colors.icon_border = Color::Rgba(213, 235, 10, 220);
  u.lines = {true, true, true, true, true, false};
  u.text = {"Achievement unlocked", "{name}", "[+{points} G]", "{description}"};
  u.uppercase_title = true;
  u.points_pill = true;
  u.title_size = 12.0f;
  u.name_size = 17.5f;
  u.points_size = 15.0f;
  u.description_size = 13.5f;
  u.timing = {0.4f, 4.0f, 0.6f};
  u.animation = Animation::kSlide;
  u.slide_distance = 50.0f;

  auto& p = s.progress;
  p = u;
  ProgressDefaults(p);
  p.anchor = Anchor::kMiddleRight;
  p.offset_x = 24.0f;
  p.offset_y = 0.0f;
  p.width = 320.0f;
  p.min_height = 74.0f;
  p.points_pill = false;
  p.uppercase_title = false;
  p.text = {"{item}", "{name}", "{current} / {total}", ""};
  p.colors.title = Color::Rgba(250, 252, 252);
  p.colors.name = Color::Rgba(180, 190, 195, 235);
  p.colors.bar_track = Color::Rgba(32, 40, 44, 220);
  p.colors.bar_fill = lime;
  p.slide_distance = 60.0f;
  p.animation = Animation::kSlide;

  s.sound.unlock = "builtin:classic";
  s.sound.progress = "builtin:classic_tick";
  s.sound.volume = 1.0f;
  return s;
}

}  // namespace

std::vector<std::string> BuiltinThemeNames() { return {"xbox360", "ps3", "retro", "minimal", "classic"}; }

Style BuiltinTheme(std::string_view name, bool* found) {
  const std::string l = Lower(name);
  if (found) *found = true;
  if (l == "xbox360" || l == "xbox_360" || l == "xbox") return Xbox360Theme();
  if (l == "ps3" || l == "playstation3" || l == "playstation_3") return Ps3Theme();
  if (l == "retro" || l == "retroachievements") return RetroTheme();
  if (l == "classic" || l == "my_game") return ClassicTheme();
  if (l != "minimal" && found) *found = false;
  return MinimalTheme();
}

std::string_view DefaultThemeFor(Platform platform) {
  switch (platform) {
    case Platform::kXbox360: return "xbox360";
    case Platform::kPlayStation3: return "ps3";
    case Platform::kRetroAchievements: return "retro";
    default: return "minimal";
  }
}

std::string_view PlatformKey(Platform platform) {
  switch (platform) {
    case Platform::kXbox360: return "xbox360";
    case Platform::kPlayStation3: return "ps3";
    case Platform::kRetroAchievements: return "retro";
    default: return "custom";
  }
}

// ---- JSON patching -----------------------------------------------------------

namespace {

class Patcher {
 public:
  Patcher(const std::filesystem::path& base_dir, std::vector<std::string>* warnings)
      : base_dir_(base_dir), warnings_(warnings) {}

  void Warn(const std::string& path, const std::string& what) {
    if (warnings_) warnings_->push_back(path + ": " + what);
  }

  static bool Ignored(const std::string& key) {
    return key.empty() || key[0] == '_' || key[0] == '$' || key == "comment";
  }

  template <typename Fn>
  void Object(const json& j, const std::string& path, Fn&& fn) {
    if (!j.is_object()) {
      Warn(path, "expected an object");
      return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (Ignored(it.key())) continue;
      const std::string sub = path.empty() ? it.key() : path + "." + it.key();
      if (!fn(it.key(), it.value(), sub)) Warn(sub, "unknown key");
    }
  }

  void Num(const json& j, const std::string& path, float& out, float lo = -1e6f, float hi = 1e6f) {
    if (!j.is_number()) return Warn(path, "expected a number");
    out = std::clamp(j.get<float>(), lo, hi);
  }
  void Int(const json& j, const std::string& path, int& out, int lo, int hi) {
    if (!j.is_number()) return Warn(path, "expected a number");
    out = std::clamp(static_cast<int>(std::lround(j.get<double>())), lo, hi);
  }
  void Bool(const json& j, const std::string& path, bool& out) {
    if (!j.is_boolean()) return Warn(path, "expected true or false");
    out = j.get<bool>();
  }
  void Str(const json& j, const std::string& path, std::string& out) {
    if (!j.is_string()) return Warn(path, "expected a string");
    out = j.get<std::string>();
  }
  void Col(const json& j, const std::string& path, Color& out) {
    if (!ParseColor(j, out)) Warn(path, "expected \"#RRGGBB(AA)\" or [r, g, b, a]");
  }
  void Sound(const json& j, const std::string& path, std::string& out) {
    if (j.is_null()) {
      out = "none";
      return;
    }
    if (!j.is_string()) return Warn(path, "expected a sound ref string");
    std::string s = j.get<std::string>();
    const bool is_path = s.find_first_of("/\\.") != std::string::npos && s.rfind("builtin:", 0) != 0;
    if (is_path && !base_dir_.empty()) {
      std::filesystem::path p(s);
      if (p.is_relative()) s = (base_dir_ / p).lexically_normal().string();
    }
    out = std::move(s);
  }

  void Popup(const json& j, const std::string& path, PopupStyle& p) {
    Object(j, path, [&](const std::string& k, const json& v, const std::string& sp) {
      if (k == "anchor") {
        if (!v.is_string() || !ParseAnchor(v.get<std::string>(), p.anchor))
          Warn(sp, "expected top_left/top_center/top_right/middle_left/center/middle_right/"
                   "bottom_left/bottom_center/bottom_right");
      } else if (k == "offset") {
        if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number()) {
          p.offset_x = v[0].get<float>();
          p.offset_y = v[1].get<float>();
        } else {
          Warn(sp, "expected [x, y]");
        }
      } else if (k == "offset_x") Num(v, sp, p.offset_x);
      else if (k == "offset_y") Num(v, sp, p.offset_y);
      else if (k == "width") Num(v, sp, p.width, 40.0f, 4000.0f);
      else if (k == "min_height") Num(v, sp, p.min_height, 0.0f, 2000.0f);
      else if (k == "padding") Num(v, sp, p.padding, 0.0f, 200.0f);
      else if (k == "rounding") Num(v, sp, p.rounding, 0.0f, 500.0f);
      else if (k == "border") Num(v, sp, p.border_thickness, 0.0f, 50.0f);
      else if (k == "icon_size") Num(v, sp, p.icon_size, 0.0f, 1000.0f);
      else if (k == "icon_rounding") Num(v, sp, p.icon_rounding, 0.0f, 500.0f);
      else if (k == "icon_shape") {
        if (v == "circle") p.icon_shape = IconShape::kCircle;
        else if (v == "square") p.icon_shape = IconShape::kSquare;
        else Warn(sp, "expected \"square\" or \"circle\"");
      } else if (k == "font_scale") Num(v, sp, p.font_scale, 0.1f, 10.0f);
      else if (k == "font_sizes") {
        Object(v, sp, [&](const std::string& fk, const json& fv, const std::string& fp) {
          if (fk == "title") Num(fv, fp, p.title_size, 1.0f, 200.0f);
          else if (fk == "name") Num(fv, fp, p.name_size, 1.0f, 200.0f);
          else if (fk == "points") Num(fv, fp, p.points_size, 1.0f, 200.0f);
          else if (fk == "description") Num(fv, fp, p.description_size, 1.0f, 200.0f);
          else return false;
          return true;
        });
      } else if (k == "description_lines") Int(v, sp, p.description_lines, 0, 8);
      else if (k == "uppercase_title") Bool(v, sp, p.uppercase_title);
      else if (k == "shadow") Bool(v, sp, p.shadow);
      else if (k == "points_pill") Bool(v, sp, p.points_pill);
      else if (k == "tier_colors") Bool(v, sp, p.tier_colors);
      else if (k == "badge") Str(v, sp, p.badge);
      else if (k == "timing") {
        Object(v, sp, [&](const std::string& tk, const json& tv, const std::string& tp) {
          if (tk == "in") Num(tv, tp, p.timing.in, 0.0f, 60.0f);
          else if (tk == "hold") Num(tv, tp, p.timing.hold, 0.0f, 600.0f);
          else if (tk == "out") Num(tv, tp, p.timing.out, 0.0f, 60.0f);
          else return false;
          return true;
        });
      } else if (k == "animation") {
        if (!v.is_string() || !ParseAnimation(v.get<std::string>(), p.animation))
          Warn(sp, "expected none/fade/slide/expand/pop");
      } else if (k == "slide_distance") Num(v, sp, p.slide_distance, 0.0f, 4000.0f);
      else if (k == "max_visible") Int(v, sp, p.max_visible, 1, 16);
      else if (k == "spacing") Num(v, sp, p.spacing, 0.0f, 500.0f);
      else if (k == "colors" || k == "colours") {
        auto& c = p.colors;
        Object(v, sp, [&](const std::string& ck, const json& cv, const std::string& cp) {
          Color* target = nullptr;
          if (ck == "background") target = &c.background;
          else if (ck == "border") target = &c.border;
          else if (ck == "shadow") target = &c.shadow;
          else if (ck == "title") target = &c.title;
          else if (ck == "name") target = &c.name;
          else if (ck == "description") target = &c.description;
          else if (ck == "points") target = &c.points;
          else if (ck == "accent") target = &c.accent;
          else if (ck == "icon_border") target = &c.icon_border;
          else if (ck == "bar_track") target = &c.bar_track;
          else if (ck == "bar_fill") target = &c.bar_fill;
          if (!target) return false;
          Col(cv, cp, *target);
          return true;
        });
      } else if (k == "lines") {
        auto& l = p.lines;
        Object(v, sp, [&](const std::string& lk, const json& lv, const std::string& lp) {
          bool* target = nullptr;
          if (lk == "title") target = &l.title;
          else if (lk == "name") target = &l.name;
          else if (lk == "points" || lk == "tier") target = &l.points;
          else if (lk == "description") target = &l.description;
          else if (lk == "icon") target = &l.icon;
          else if (lk == "bar") target = &l.bar;
          if (!target) return false;
          Bool(lv, lp, *target);
          return true;
        });
      } else if (k == "text") {
        auto& t = p.text;
        Object(v, sp, [&](const std::string& tk, const json& tv, const std::string& tp) {
          std::string* target = nullptr;
          if (tk == "title") target = &t.title;
          else if (tk == "name") target = &t.name;
          else if (tk == "points") target = &t.points;
          else if (tk == "description") target = &t.description;
          if (!target) return false;
          Str(tv, tp, *target);
          return true;
        });
      } else {
        return false;
      }
      return true;
    });
  }

  void Root(const json& j, Style& s) {
    Object(j, "", [&](const std::string& k, const json& v, const std::string& sp) {
      if (k == "theme" || k == "platforms" || k == "sets") return true;  // StyleManager's.
      if (k == "scale") Num(v, sp, s.scale, 0.1f, 10.0f);
      else if (k == "reference_height") Num(v, sp, s.reference_height, 100.0f, 10000.0f);
      else if (k == "min_scale") Num(v, sp, s.min_scale, 0.05f, 10.0f);
      else if (k == "max_scale") Num(v, sp, s.max_scale, 0.05f, 20.0f);
      else if (k == "small_screen_height") Num(v, sp, s.small_screen_height, 0.0f, 10000.0f);
      else if (k == "small_screen_scale") Num(v, sp, s.small_screen_scale, 0.1f, 10.0f);
      else if (k == "max_queued") Int(v, sp, s.max_queued, 1, 1000);
      else if (k == "font") Str(v, sp, s.font);
      else if (k == "font_bold") Str(v, sp, s.font_bold);
      else if (k == "unlock") Popup(v, sp, s.unlock);
      else if (k == "progress") {
        if (v.is_boolean()) {
          s.progress_enabled = v.get<bool>();
          return true;
        }
        if (!v.is_object()) {
          Warn(sp, "expected an object or true/false");
          return true;
        }
        // Progress-only keys first; the rest is a popup style.
        json rest = json::object();
        for (auto it = v.begin(); it != v.end(); ++it) {
          const std::string pk = it.key();
          const std::string pp = sp + "." + pk;
          if (pk == "enabled") Bool(it.value(), pp, s.progress_enabled);
          else if (pk == "max_queued") Int(it.value(), pp, s.progress_max_queued, 1, 1000);
          else if (pk == "coalesce") Bool(it.value(), pp, s.progress_coalesce);
          else if (pk == "skip_complete") Bool(it.value(), pp, s.progress_skip_complete);
          else rest[pk] = it.value();
        }
        Popup(rest, sp, s.progress);
      } else if (k == "secret") {
        Object(v, sp, [&](const std::string& sk, const json& sv, const std::string& spp) {
          if (sk == "name") Str(sv, spp, s.secret_name);
          else if (sk == "description") Str(sv, spp, s.secret_description);
          else return false;
          return true;
        });
      } else if (k == "sound") {
        if (v.is_boolean()) {
          s.sound.enabled = v.get<bool>();
          return true;
        }
        Object(v, sp, [&](const std::string& sk, const json& sv, const std::string& spp) {
          if (sk == "enabled") Bool(sv, spp, s.sound.enabled);
          else if (sk == "volume") Num(sv, spp, s.sound.volume, 0.0f, 1.0f);
          else if (sk == "unlock") Sound(sv, spp, s.sound.unlock);
          else if (sk == "progress") Sound(sv, spp, s.sound.progress);
          else if (sk == "bronze") Sound(sv, spp, s.sound.bronze);
          else if (sk == "silver") Sound(sv, spp, s.sound.silver);
          else if (sk == "gold") Sound(sv, spp, s.sound.gold);
          else if (sk == "platinum") Sound(sv, spp, s.sound.platinum);
          else return false;
          return true;
        });
      } else if (k == "viewer") {
        Object(v, sp, [&](const std::string& vk, const json& vv, const std::string& vp) {
          if (vk == "key") Str(vv, vp, s.viewer.key);
          else if (vk == "pad") Str(vv, vp, s.viewer.pad);
          else if (vk == "buttons") Str(vv, vp, s.viewer.buttons);
          else if (vk == "scale") Num(vv, vp, s.viewer.scale, 0.25f, 4.0f);
          else if (vk == "colors" || vk == "colours") {
            Object(vv, vp, [&](const std::string& ck, const json& cv, const std::string& cp) {
              std::optional<Color>* slot = nullptr;
              if (ck == "backdrop") slot = &s.viewer.backdrop;
              else if (ck == "panel") slot = &s.viewer.panel;
              else if (ck == "accent") slot = &s.viewer.accent;
              else if (ck == "text") slot = &s.viewer.text;
              else if (ck == "subtext") slot = &s.viewer.subtext;
              else return false;
              if (cv.is_null()) {
                slot->reset();  // Back to following the popups.
                return true;
              }
              Color col;
              if (ParseColor(cv, col)) *slot = col;
              else Warn(cp, "expected \"#RRGGBB(AA)\" or [r, g, b, a]");
              return true;
            });
          } else {
            return false;
          }
          return true;
        });
      } else if (k == "tiers") {
        Object(v, sp, [&](const std::string& tk, const json& tv, const std::string& tp) {
          Color* col = nullptr;
          std::string* name = nullptr;
          if (tk == "bronze") col = &s.tiers.bronze, name = &s.tiers.bronze_name;
          else if (tk == "silver") col = &s.tiers.silver, name = &s.tiers.silver_name;
          else if (tk == "gold") col = &s.tiers.gold, name = &s.tiers.gold_name;
          else if (tk == "platinum") col = &s.tiers.platinum, name = &s.tiers.platinum_name;
          else return false;
          // "gold": "#E8BD30" or {"color": ..., "name": ...}
          if (tv.is_object()) {
            Object(tv, tp, [&](const std::string& xk, const json& xv, const std::string& xp) {
              if (xk == "color" || xk == "colour") Col(xv, xp, *col);
              else if (xk == "name") Str(xv, xp, *name);
              else return false;
              return true;
            });
          } else {
            Col(tv, tp, *col);
          }
          return true;
        });
      } else {
        return false;
      }
      return true;
    });
  }

 private:
  const std::filesystem::path& base_dir_;
  std::vector<std::string>* warnings_;
};

json ColorJson(const Color& c) { return ColorToHex(c); }

json PopupToJson(const PopupStyle& p) {
  json j;
  j["anchor"] = AnchorName(p.anchor);
  j["offset"] = {p.offset_x, p.offset_y};
  j["width"] = p.width;
  j["min_height"] = p.min_height;
  j["padding"] = p.padding;
  j["rounding"] = p.rounding;
  j["border"] = p.border_thickness;
  j["icon_size"] = p.icon_size;
  j["icon_rounding"] = p.icon_rounding;
  j["icon_shape"] = p.icon_shape == IconShape::kCircle ? "circle" : "square";
  j["font_scale"] = p.font_scale;
  j["font_sizes"] = {{"title", p.title_size},
                     {"name", p.name_size},
                     {"points", p.points_size},
                     {"description", p.description_size}};
  j["description_lines"] = p.description_lines;
  j["uppercase_title"] = p.uppercase_title;
  j["shadow"] = p.shadow;
  j["points_pill"] = p.points_pill;
  j["tier_colors"] = p.tier_colors;
  j["badge"] = p.badge;
  j["timing"] = {{"in", p.timing.in}, {"hold", p.timing.hold}, {"out", p.timing.out}};
  j["animation"] = AnimationName(p.animation);
  j["slide_distance"] = p.slide_distance;
  j["max_visible"] = p.max_visible;
  j["spacing"] = p.spacing;
  const auto& c = p.colors;
  j["colors"] = {{"background", ColorJson(c.background)}, {"border", ColorJson(c.border)},
                 {"shadow", ColorJson(c.shadow)},         {"title", ColorJson(c.title)},
                 {"name", ColorJson(c.name)},             {"description", ColorJson(c.description)},
                 {"points", ColorJson(c.points)},         {"accent", ColorJson(c.accent)},
                 {"icon_border", ColorJson(c.icon_border)}, {"bar_track", ColorJson(c.bar_track)},
                 {"bar_fill", ColorJson(c.bar_fill)}};
  const auto& l = p.lines;
  j["lines"] = {{"title", l.title},   {"name", l.name}, {"points", l.points},
                {"description", l.description}, {"icon", l.icon}, {"bar", l.bar}};
  j["text"] = {{"title", p.text.title},
               {"name", p.text.name},
               {"points", p.text.points},
               {"description", p.text.description}};
  return j;
}

}  // namespace

void ApplyStyleJson(Style& style, const json& patch, const std::filesystem::path& base_dir,
                    std::vector<std::string>* warnings) {
  Patcher(base_dir, warnings).Root(patch, style);
}

json StyleToJson(const Style& s) {
  json j;
  j["theme"] = s.theme;
  j["scale"] = s.scale;
  j["reference_height"] = s.reference_height;
  j["min_scale"] = s.min_scale;
  j["max_scale"] = s.max_scale;
  j["small_screen_height"] = s.small_screen_height;
  j["small_screen_scale"] = s.small_screen_scale;
  j["max_queued"] = s.max_queued;
  j["font"] = s.font;
  j["font_bold"] = s.font_bold;
  j["unlock"] = PopupToJson(s.unlock);
  json p = PopupToJson(s.progress);
  p["enabled"] = s.progress_enabled;
  p["max_queued"] = s.progress_max_queued;
  p["coalesce"] = s.progress_coalesce;
  p["skip_complete"] = s.progress_skip_complete;
  j["progress"] = std::move(p);
  j["secret"] = {{"name", s.secret_name}, {"description", s.secret_description}};
  j["sound"] = {{"enabled", s.sound.enabled}, {"volume", s.sound.volume},
                {"unlock", s.sound.unlock},   {"progress", s.sound.progress},
                {"bronze", s.sound.bronze},   {"silver", s.sound.silver},
                {"gold", s.sound.gold},       {"platinum", s.sound.platinum}};
  json viewer = {{"key", s.viewer.key}, {"pad", s.viewer.pad}, {"buttons", s.viewer.buttons},
                 {"scale", s.viewer.scale}};
  json vc = json::object();
  auto opt = [&](const char* name, const std::optional<Color>& c) {
    if (c) vc[name] = ColorToHex(*c);
  };
  opt("backdrop", s.viewer.backdrop);
  opt("panel", s.viewer.panel);
  opt("accent", s.viewer.accent);
  opt("text", s.viewer.text);
  opt("subtext", s.viewer.subtext);
  if (!vc.empty()) viewer["colors"] = std::move(vc);
  j["viewer"] = std::move(viewer);
  const auto& t = s.tiers;
  j["tiers"] = {{"bronze", {{"color", ColorToHex(t.bronze)}, {"name", t.bronze_name}}},
                {"silver", {{"color", ColorToHex(t.silver)}, {"name", t.silver_name}}},
                {"gold", {{"color", ColorToHex(t.gold)}, {"name", t.gold_name}}},
                {"platinum", {{"color", ColorToHex(t.platinum)}, {"name", t.platinum_name}}}};
  return j;
}

// ---- StyleManager --------------------------------------------------------------

namespace {

// Written once so players can find the file; every setting is commented out.
constexpr const char* kGlobalStyleTemplate = R"(// Achievement popups for every game that uses recomp-achievements.
// Settings here override each game's own style; a game's own
// achievements_style.json (in its user folder) overrides these.
// Uncomment what you want. The full list is in the SDK's docs/styling.md:
// https://github.com/BadVibrati0ns/recomp-achievements/blob/main/docs/styling.md
{
  // "theme": "xbox360",           // xbox360, ps3, retro, minimal, classic, or "auto"
  // "scale": 1.0,
  // "sound": { "enabled": true, "volume": 0.8 },
  // "unlock": { "anchor": "bottom_center" },
  // "progress": { "enabled": true }
  // Replacement sounds go in the sounds folder next to this file:
  // unlock.wav, progress.wav, unlock_gold.wav, or <builtin name>.wav.
}
)";

}  // namespace

std::filesystem::path EnsureGlobalConfigDir() {
  const auto dir = GlobalConfigDir();
  if (dir.empty()) return {};
  std::error_code ec;
  std::filesystem::create_directories(dir / "sounds", ec);
  if (ec) return {};
  const auto file = dir / "achievements_style.json";
  if (!std::filesystem::exists(file, ec)) {
    std::ofstream out(file, std::ios::binary);
    out << kGlobalStyleTemplate;
  }
  return dir;
}

StyleManager::StyleManager() = default;
StyleManager::~StyleManager() = default;

void StyleManager::SetGameStyle(json style, std::filesystem::path base_dir) {
  std::lock_guard lock(mutex_);
  game_ = {std::move(style), std::move(base_dir), {}, true};
  cache_.clear();
  ++generation_;
}

void StyleManager::SetGlobalStyle(json style, std::filesystem::path base_dir) {
  std::lock_guard lock(mutex_);
  global_ = {std::move(style), std::move(base_dir), {}, true};
  cache_.clear();
  ++generation_;
}

bool StyleManager::LoadGlobalStyleFile(const std::filesystem::path& path, std::string* error) {
  std::lock_guard lock(mutex_);
  const bool ok = LoadFile(global_, path, error);
  cache_.clear();
  ++generation_;
  return ok;
}

void StyleManager::SetUserStyle(json style, std::filesystem::path base_dir) {
  std::lock_guard lock(mutex_);
  user_ = {std::move(style), std::move(base_dir), {}, true};
  cache_.clear();
  ++generation_;
}

bool StyleManager::LoadFile(Layer& layer, const std::filesystem::path& path, std::string* error) {
  layer.file = path;
  layer.base_dir = path.parent_path();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    layer.json = json::object();
    layer.present = false;
    return true;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (error) *error = "cannot open " + path.string();
    return false;
  }
  // Comments are allowed: these files are written by hand.
  json j = json::parse(in, nullptr, false, true);
  if (j.is_discarded() || !j.is_object()) {
    if (error) *error = path.string() + ": not a JSON object";
    return false;
  }
  layer.json = std::move(j);
  layer.present = true;
  return true;
}

bool StyleManager::LoadGameStyleFile(const std::filesystem::path& path, std::string* error) {
  std::lock_guard lock(mutex_);
  const bool ok = LoadFile(game_, path, error);
  cache_.clear();
  ++generation_;
  return ok;
}

bool StyleManager::LoadUserStyleFile(const std::filesystem::path& path, std::string* error) {
  std::lock_guard lock(mutex_);
  const bool ok = LoadFile(user_, path, error);
  cache_.clear();
  ++generation_;
  return ok;
}

bool StyleManager::Reload(std::string* error) {
  std::lock_guard lock(mutex_);
  bool ok = true;
  if (!game_.file.empty()) ok &= LoadFile(game_, game_.file, error);
  if (!global_.file.empty()) ok &= LoadFile(global_, global_.file, error);
  if (!user_.file.empty()) ok &= LoadFile(user_, user_.file, error);
  cache_.clear();
  warnings_.clear();
  ++generation_;
  return ok;
}

void StyleManager::SetGameHook(Hook hook) {
  std::lock_guard lock(mutex_);
  hook_ = std::move(hook);
  cache_.clear();
  ++generation_;
}

void StyleManager::SetThemeOverride(std::string theme) {
  std::lock_guard lock(mutex_);
  theme_override_ = std::move(theme);
  cache_.clear();
  ++generation_;
}

std::shared_ptr<const Style> StyleManager::Resolve(const AchievementSet* set) const {
  std::lock_guard lock(mutex_);
  if (!set) return ResolveLocked({}, Platform::kCustom, nullptr);
  return ResolveLocked(set->id, set->platform, set);
}

std::shared_ptr<const Style> StyleManager::Resolve(std::string_view set_id, Platform platform) const {
  std::lock_guard lock(mutex_);
  return ResolveLocked(set_id, platform, nullptr);
}

std::shared_ptr<const Style> StyleManager::ResolveLocked(std::string_view set_id, Platform platform,
                                                         const AchievementSet* set) const {
  std::string cache_key(set_id);
  cache_key += '|';
  cache_key += PlatformKey(platform);
  if (auto it = cache_.find(cache_key); it != cache_.end()) return it->second;

  // Lowest first; j == nullptr marks where the game hook runs.
  struct Patch {
    const json* j;
    const std::filesystem::path* base_dir;
    std::string where;
  };
  std::vector<Patch> patches;
  auto add_layer = [&](const Layer& layer, const char* name) {
    if (!layer.present || !layer.json.is_object()) return;
    const json& root = layer.json;
    patches.push_back({&root, &layer.base_dir, name});
    if (auto p = root.find("platforms"); p != root.end() && p->is_object()) {
      if (auto e = p->find(std::string(PlatformKey(platform))); e != p->end() && e->is_object())
        patches.push_back({&*e, &layer.base_dir, std::string(name) + ".platforms"});
    }
    if (!set_id.empty()) {
      if (auto s = root.find("sets"); s != root.end() && s->is_object()) {
        if (auto e = s->find(std::string(set_id)); e != s->end() && e->is_object())
          patches.push_back({&*e, &layer.base_dir, std::string(name) + ".sets"});
      }
    }
  };
  add_layer(game_, "game");
  const size_t user_start = patches.size() + 1;
  patches.push_back({nullptr, nullptr, "hook"});
  add_layer(global_, "global");
  add_layer(user_, "user");

  std::string theme;
  size_t start = 0;
  if (!theme_override_.empty()) {
    theme = theme_override_;
    start = user_start;  // A forced theme keeps only the player's tweaks.
  } else {
    for (size_t i = patches.size(); i-- > 0;) {
      if (!patches[i].j) continue;
      auto t = patches[i].j->find("theme");
      if (t != patches[i].j->end() && t->is_string()) {
        theme = t->get<std::string>();
        start = i;
        break;
      }
    }
  }
  if (theme.empty() || theme == "auto" || theme == "default") theme = DefaultThemeFor(platform);

  bool found = true;
  auto style = std::make_shared<Style>(BuiltinTheme(theme, &found));
  std::vector<std::string> warnings;
  if (!found) warnings.push_back("theme: unknown theme \"" + theme + "\", using minimal");

  for (size_t i = start; i < patches.size(); ++i) {
    if (!patches[i].j) {
      if (hook_) hook_(*style, set);
      continue;
    }
    std::vector<std::string> w;
    ApplyStyleJson(*style, *patches[i].j, *patches[i].base_dir, &w);
    for (auto& s : w) warnings.push_back(patches[i].where + ": " + s);
  }

  for (auto& w : warnings) {
    if (std::find(warnings_.begin(), warnings_.end(), w) == warnings_.end()) warnings_.push_back(w);
  }
  cache_.emplace(std::move(cache_key), style);
  return style;
}

std::vector<std::string> StyleManager::warnings() const {
  std::lock_guard lock(mutex_);
  return warnings_;
}

uint64_t StyleManager::generation() const {
  std::lock_guard lock(mutex_);
  return generation_;
}

}  // namespace recomp::achievements::ui
