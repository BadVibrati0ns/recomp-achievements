#pragma once

// Popup styles as plain data. A Style is fully resolved: every field has a
// value. Partial JSON patches are applied on top of a built-in theme by
// StyleManager (see docs/styling.md for the JSON shape).

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "recomp/achievements/config_dir.h"
#include "recomp/achievements/types.h"

namespace recomp::achievements::ui {

// Linear 0..1 RGBA. JSON accepts "#RRGGBB", "#RRGGBBAA" or [r, g, b(, a)] in 0..255.
struct Color {
  float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
  static Color Rgba(int r, int g, int b, int a = 255) {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
  }
  bool operator==(const Color&) const = default;
};

enum class Anchor {
  kTopLeft,
  kTopCenter,
  kTopRight,
  kMiddleLeft,
  kCenter,
  kMiddleRight,
  kBottomLeft,
  kBottomCenter,
  kBottomRight,
};

enum class Animation {
  kNone,
  kFade,    // Alpha only.
  kSlide,   // Slides in from the nearest screen edge.
  kExpand,  // Icon first, then the card widens from it (Xbox 360 style).
  kPop,     // Scales up from the centre with a small overshoot.
};

enum class IconShape { kSquare, kCircle };

struct Timing {
  float in = 0.4f;    // Seconds.
  float hold = 4.0f;
  float out = 0.6f;
  float total() const { return in + hold + out; }
  bool operator==(const Timing&) const = default;
};

struct PopupColors {
  Color background = Color::Rgba(20, 20, 20, 235);
  Color border = Color::Rgba(255, 255, 255, 60);
  Color shadow = Color::Rgba(0, 0, 0, 140);
  Color title = Color::Rgba(200, 200, 200);  // Header line ("Achievement unlocked").
  Color name = Color::Rgba(255, 255, 255);
  Color description = Color::Rgba(190, 190, 190);
  Color points = Color::Rgba(255, 255, 255);
  Color accent = Color::Rgba(255, 255, 255);  // Fallback badge, highlights.
  Color icon_border = Color::Rgba(255, 255, 255, 0);
  Color bar_track = Color::Rgba(60, 60, 60, 220);
  Color bar_fill = Color::Rgba(255, 255, 255);
  bool operator==(const PopupColors&) const = default;
};

// Which lines a popup draws. A line whose text formats to "" is skipped too.
struct PopupLines {
  bool title = true;
  bool name = true;
  bool points = true;  // Points or trophy tier.
  bool description = true;
  bool icon = true;
  bool bar = false;  // Progress bar (progress popups).
  bool operator==(const PopupLines&) const = default;
};

// Format strings. Tokens: {name} {description} {points} {tier} {set} {item}
// {current} {total} {percent}; {points} is "" for 0 and {tier} "" for no tier.
// "[...]" is optional text, dropped when a token inside it is empty. Unknown
// tokens are left as they are; a line that formats to blank is not drawn.
struct PopupText {
  std::string title = "Achievement unlocked";
  std::string name = "{name}";
  std::string points = "[{points} pts][{tier}]";
  std::string description = "{description}";
  bool operator==(const PopupText&) const = default;
};

// Layout and look of one popup kind. Sizes are pixels at reference_height.
struct PopupStyle {
  Anchor anchor = Anchor::kBottomCenter;
  float offset_x = 0.0f;   // Away from the anchored edge(s), toward the centre.
  float offset_y = 60.0f;
  float width = 440.0f;
  float min_height = 76.0f;
  float padding = 12.0f;
  float rounding = 8.0f;
  float border_thickness = 1.0f;
  float icon_size = 52.0f;
  float icon_rounding = 6.0f;
  IconShape icon_shape = IconShape::kSquare;
  float font_scale = 1.0f;
  float title_size = 12.0f;  // Font pixel sizes before font_scale.
  float name_size = 17.0f;
  float points_size = 14.0f;
  float description_size = 13.5f;
  int description_lines = 2;
  bool uppercase_title = false;
  bool shadow = true;
  bool points_pill = false;  // Draw points in a rounded pill on the right.
  // Text in the fallback badge drawn when there is no icon ("" = a cup shape).
  std::string badge;
  bool tier_colors = true;   // Colour points/badge with the trophy tier colour.
  Timing timing;
  Animation animation = Animation::kSlide;
  float slide_distance = 50.0f;
  int max_visible = 1;  // >1 stacks popups.
  float spacing = 8.0f; // Between stacked popups.
  PopupColors colors;
  PopupLines lines;
  PopupText text;
  bool operator==(const PopupStyle&) const = default;
};

struct SoundStyle {
  bool enabled = true;
  float volume = 0.8f;  // 0..1.
  // Sound refs: "builtin:<name>", a WAV path (relative paths resolve from the
  // JSON file that set them), or "" / "none" for silence.
  std::string unlock = "builtin:minimal";
  std::string progress = "builtin:tick";
  // Per-tier unlock sounds; empty = use `unlock`.
  std::string bronze, silver, gold, platinum;
  bool operator==(const SoundStyle&) const = default;
};

struct TierStyle {
  Color bronze = Color::Rgba(205, 127, 50);
  Color silver = Color::Rgba(196, 202, 206);
  Color gold = Color::Rgba(232, 189, 48);
  Color platinum = Color::Rgba(170, 214, 240);
  std::string bronze_name = "Bronze", silver_name = "Silver", gold_name = "Gold",
              platinum_name = "Platinum";
  bool operator==(const TierStyle&) const = default;
};

// The achievement viewer (overlay.h). Empty strings leave the game's choice;
// unset colours follow the popups' (panel = unlock background, accent =
// progress bar fill, text = unlock name, subtext = unlock description).
struct ViewerStyle {
  std::string key;      // Toggle key: "F1".."F12", or "none".
  std::string pad;      // Toggle chord: buttons joined by '+', e.g. "start+lb", or "none".
  std::string buttons;  // Button labels: "xbox" or "playstation".
  float scale = 1.0f;   // On top of the display-height scale.
  std::optional<Color> backdrop, panel, accent, text, subtext;
  bool operator==(const ViewerStyle&) const = default;
};

struct Style {
  std::string theme = "minimal";  // Built-in theme this style started from.
  float scale = 1.0f;
  float reference_height = 1080.0f;  // Sizes scale with display height / this.
  float min_scale = 0.6f, max_scale = 2.5f;
  // Small screens (a Steam Deck's 800 lines): at or under this height, sizes
  // don't shrink below small_screen_scale (times height / small_screen_height
  // under it), so popups and the viewer stay readable on a handheld. 0: off.
  float small_screen_height = 800.0f;
  float small_screen_scale = 1.0f;
  int max_queued = 16;  // Waiting unlock popups; older ones are dropped beyond this.
  std::string font, font_bold;  // Names registered with the renderer; "" = default.

  PopupStyle unlock;
  PopupStyle progress;
  bool progress_enabled = true;
  int progress_max_queued = 8;
  bool progress_coalesce = true;       // A newer step replaces a waiting/showing one.
  bool progress_skip_complete = true;  // The unlock popup covers the last step.

  // Locked secret achievements in progress popups.
  std::string secret_name = "Secret achievement";
  std::string secret_description = "Keep playing to reveal it.";

  SoundStyle sound;
  TierStyle tiers;
  ViewerStyle viewer;

  const Color& TierColor(Tier tier, const Color& fallback) const;
  const std::string& TierName(Tier tier) const;
  // Unlock sound for a tier, falling back to sound.unlock.
  const std::string& UnlockSound(Tier tier) const;
  bool operator==(const Style&) const = default;
};

// ---- Built-in themes ------------------------------------------------------
// "xbox360", "ps3", "retro", "minimal", "classic" (the SDK's original look).
std::vector<std::string> BuiltinThemeNames();
// Unknown names return the "minimal" theme and set *found = false.
Style BuiltinTheme(std::string_view name, bool* found = nullptr);
std::string_view DefaultThemeFor(Platform platform);
// "custom", "xbox360", "ps3", "retro": the keys used under "platforms".
std::string_view PlatformKey(Platform platform);

// ---- JSON ----------------------------------------------------------------
// Applies a partial patch field by field. Relative sound paths resolve from
// base_dir. Problems (unknown keys, wrong types) are appended to warnings and
// the field is left as it was. "theme", "platforms" and "sets" are ignored
// here; StyleManager handles them.
void ApplyStyleJson(Style& style, const nlohmann::json& patch,
                    const std::filesystem::path& base_dir = {},
                    std::vector<std::string>* warnings = nullptr);
nlohmann::json StyleToJson(const Style& style);

bool ParseColor(const nlohmann::json& j, Color& out);
std::string ColorToHex(const Color& c);
bool ParseAnchor(std::string_view s, Anchor& out);
std::string_view AnchorName(Anchor a);
bool ParseAnimation(std::string_view s, Animation& out);
std::string_view AnimationName(Animation a);

// Replaces {tokens} in fmt. Values with no entry are left untouched.
std::string FormatText(std::string_view fmt, const std::map<std::string, std::string, std::less<>>& values);

// ---- Layering -------------------------------------------------------------
// Resolves the Style for a set from, lowest first:
//   built-in theme <- game JSON <- game hook <- user JSON
// Each JSON layer applies its root, then "platforms"[<platform>], then
// "sets"[<set id>]. The theme comes from the highest of those patches that
// names one ("auto" = by platform), and patches below that one are skipped
// because they were tuning a different theme. Thread-safe.
// The player's preferences for every game using this SDK (now in core,
// config_dir.h; kept here under its old name). Holds achievements_style.json
// (the same format as a game's style file) and sounds/*.wav.
using recomp::achievements::GlobalConfigDir;
// Creates GlobalConfigDir() with a commented achievements_style.json that
// changes nothing and an empty sounds/ folder, so players can find them.
// Leaves existing files alone. Returns the folder, or empty on failure.
std::filesystem::path EnsureGlobalConfigDir();

class StyleManager {
 public:
  using Hook = std::function<void(Style&, const AchievementSet* set)>;

  StyleManager();
  ~StyleManager();

  void SetGameStyle(nlohmann::json style, std::filesystem::path base_dir = {});
  // A missing file is not an error (returns true, no layer). Parse errors return false.
  bool LoadGameStyleFile(const std::filesystem::path& path, std::string* error = nullptr);
  // The player's preferences for every game (see GlobalConfigDir()). They
  // override the game's style and hook; the per-game user style overrides them.
  void SetGlobalStyle(nlohmann::json style, std::filesystem::path base_dir = {});
  bool LoadGlobalStyleFile(const std::filesystem::path& path, std::string* error = nullptr);
  void SetUserStyle(nlohmann::json style, std::filesystem::path base_dir = {});
  bool LoadUserStyleFile(const std::filesystem::path& path, std::string* error = nullptr);
  // Re-reads the files given to Load*StyleFile (e.g. from a settings menu).
  bool Reload(std::string* error = nullptr);
  // Runs after game JSON and before user JSON, for games that set styles in code.
  void SetGameHook(Hook hook);
  // Forces a theme for everything, overriding the layers' choice ("" clears).
  void SetThemeOverride(std::string theme);

  // set may be null (custom platform, no set overrides).
  std::shared_ptr<const Style> Resolve(const AchievementSet* set) const;
  std::shared_ptr<const Style> Resolve(std::string_view set_id, Platform platform) const;

  std::vector<std::string> warnings() const;
  // Bumped whenever a layer changes; lets callers drop their own caches.
  uint64_t generation() const;

 private:
  struct Layer {
    nlohmann::json json;
    std::filesystem::path base_dir;
    std::filesystem::path file;
    bool present = false;
  };
  std::shared_ptr<const Style> ResolveLocked(std::string_view set_id, Platform platform,
                                             const AchievementSet* set) const;
  bool LoadFile(Layer& layer, const std::filesystem::path& path, std::string* error);

  mutable std::mutex mutex_;
  Layer game_, global_, user_;
  Hook hook_;
  std::string theme_override_;
  uint64_t generation_ = 1;
  mutable std::map<std::string, std::shared_ptr<const Style>, std::less<>> cache_;
  mutable std::vector<std::string> warnings_;
};

}  // namespace recomp::achievements::ui
