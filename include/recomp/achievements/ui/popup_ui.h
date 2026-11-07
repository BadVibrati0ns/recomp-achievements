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
#include "recomp/achievements/ui/viewer_toggle.h"

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
  // The shared achievement library every game writes to (docs/library.md).
  // On once library.game_id is set; its icons come from icons() and its game
  // icon defaults to fallback_icon.
  LibraryOptions library;
  IconCache::CreateTexture create_texture;
  IconCache::DestroyTexture destroy_texture;
  bool sounds = true;
  // The list window's toggle, the same as Overlay's (HandleInput()): F8 or
  // Start + LB unless the player's "viewer" style keys say otherwise.
  std::string toggle_key = "F8";
  std::string toggle_pad = "start+lb";
  float chord_grace = 0.15f;
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
  // Null unless options.library is on.
  Library* library() { return library_.get(); }

  // Re-reads both style files and drops cached sounds (e.g. after the user edits them).
  bool ReloadStyles(std::string* error = nullptr);

  // The list window's toggle, handled as the software overlay handles it
  // (docs/imgui-hosts.md). Once per frame, before Draw(), with the raw input:
  // F8 or Start + LB opens and closes the list. True on the frame it toggled.
  bool HandleInput(float dt, const OverlayInput& input);
  // What the game should see this frame: nothing while the list is open (or
  // until the keys that closed it are let go), the toggle key removed, the
  // chord's buttons held back for chord_grace.
  OverlayInput GameInput(const OverlayInput& raw) const;
  bool wants_input() const { return toggle_.wants_input(list_open_); }
  bool list_open() const { return list_open_; }
  void SetListOpen(bool open);
  ViewerToggle& toggle() { return toggle_; }

  // Per frame, inside the ImGui frame: popups, plus the list while it's open
  // (list_open(), or *list_open for hosts that keep their own flag).
  void Draw();
  void Draw(bool* list_open);

 private:
  System& system_;
  PopupUiOptions options_;
  StyleManager styles_;
  NotificationCenter center_;
  SoundPlayer sound_;
  IconCache icons_;
  ImGuiRenderer renderer_;
  AchievementListWindow list_;
  std::unique_ptr<Library> library_;
  ViewerToggle toggle_;
  bool list_open_ = false;
  bool back_held_ = false;  // B / Esc last frame: closing needs a fresh press.
  uint64_t bindings_generation_ = 0;
};

}  // namespace recomp::achievements::ui
