#pragma once

// One-object wiring of the whole UI for the common case: styles from game and
// user JSON, popups from a System, sounds through SDL3, icons through the
// game's texture callback and an optional list window. Needs the imgui target.

#include <filesystem>
#include <memory>

#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/achievement_list.h"
#include "recomp/achievements/ui/icon_cache.h"
#include "recomp/achievements/ui/imgui_renderer.h"
#include "recomp/achievements/ui/notification_center.h"
#include "recomp/achievements/ui/sound_player.h"
#include "recomp/achievements/ui/style.h"

namespace recomp::achievements::ui {

struct PopupUiOptions {
  // Game defaults shipped with the game, e.g. "assets/achievements_style.json".
  std::filesystem::path game_style_file;
  // The end user's folder: achievements_style.json and sounds/*.wav live here.
  std::filesystem::path user_dir;
  // Extra folders with replacement WAVs (see docs/styling.md); user_dir/sounds
  // is searched first, then these in reverse order.
  std::vector<std::filesystem::path> sound_dirs;
  // The player's preferences for every game (GlobalConfigDir()): its
  // achievements_style.json and sounds/ apply over the game's, under user_dir's.
  bool global_preferences = true;
  // Create that folder with a commented starter file if it's missing.
  bool create_global_preferences = true;
  // Icon for achievements without one (most custom ones): usually the game's
  // desktop icon. A file path or any icon ref.
  std::string fallback_icon;
  IconCache::CreateTexture create_texture;
  IconCache::DestroyTexture destroy_texture;
  bool sounds = true;
};

class PopupUi {
 public:
  PopupUi(System& system, PopupUiOptions options);
  ~PopupUi();

  StyleManager& styles() { return styles_; }
  NotificationCenter& center() { return center_; }
  SoundPlayer& sound() { return sound_; }
  IconCache& icons() { return icons_; }
  ImGuiRenderer& renderer() { return renderer_; }
  AchievementListWindow& list() { return list_; }

  // Re-reads both style files and drops cached sounds (e.g. after the user edits them).
  bool ReloadStyles(std::string* error = nullptr);

  // Per frame, inside the ImGui frame: popups, plus the list when *list_open.
  void Draw(bool* list_open = nullptr);

 private:
  System& system_;
  PopupUiOptions options_;
  StyleManager styles_;
  NotificationCenter center_;
  SoundPlayer sound_;
  IconCache icons_;
  ImGuiRenderer renderer_;
  AchievementListWindow list_;
};

}  // namespace recomp::achievements::ui
