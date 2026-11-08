#pragma once

// A ready-made Dear ImGui achievements window for games with no settings UI
// of their own: sets and categories, filters, progress bars, checklists,
// tiers/points, credits and secret handling, plus a settings view (popup
// sound, turning sets on and off, resets with a confirmation). Only built
// with an imgui target.

#include <string>
#include <vector>

#include "imgui.h"
#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/icon_cache.h"

namespace recomp::achievements::ui {

enum class ListFilter { kAll, kUnlocked, kLocked };

struct ListOptions {
  const char* window_title = "Achievements";
  bool show_credits = true;
  bool allow_reveal_secrets = true;  // Offer a "Show secret achievements" toggle.
  bool show_test_button = false;     // Per-row "Test popup" (debug builds).
  float icon_size = 48.0f;           // Pixels, before ImGui's font scaling.
  // The "Settings" button and view. Hide it, or the sections a game already
  // offers in its own menus.
  bool show_settings = true;
  bool settings_appearance = true;  // Theme, popup size and list size.
  bool settings_sound = true;  // Popup sound on/off and volume.
  bool settings_sets = true;   // Turning each set on and off.
  bool settings_reset = true;  // Resetting chosen achievements, a set, or everything.
  std::string secret_name = "Secret achievement";
  std::string secret_description = "Keep playing to reveal it.";
};

class AchievementListWindow {
 public:
  // icons may be null. Both must outlive the window.
  explicit AchievementListWindow(System& system, IconCache* icons = nullptr);

  ListOptions& options() { return options_; }

  // A full window; open toggles it like ImGui::Begin's p_open.
  void Draw(bool* open = nullptr);
  // Only the contents, for embedding in the game's own window or tab.
  void DrawContents();
  // Only the settings, for the game's own options screen. The reset
  // confirmation is a modal opened from here.
  void DrawSettings();

  bool settings_open() const { return settings_open_; }
  void SetSettingsOpen(bool open) { settings_open_ = open; }
  // Each row shows a Reset button (with a confirmation) while on.
  bool reset_mode() const { return reset_mode_; }
  void SetResetMode(bool on) { reset_mode_ = on; }

 private:
  void DrawSettingsBody();
  void AskReset(std::string label, std::vector<std::string> keys, std::string set_id, bool all);
  void DrawResetConfirmation();

  System& system_;
  IconCache* icons_;
  ListOptions options_;
  std::string selected_set_;  // "" = all sets.
  ListFilter filter_ = ListFilter::kAll;
  bool reveal_secrets_ = false;
  char search_[96] = {};
  bool settings_open_ = false;
  bool reset_mode_ = false;
  float volume_edit_ = -1.0f;  // While the slider is dragged; saved on release.
  // The player's list size (AppearanceSettings), re-read when settings change.
  float list_size_ = 1.0f;
  uint64_t list_size_generation_ = 0;
  // The reset waiting for confirmation.
  struct PendingReset {
    std::string label;
    std::vector<std::string> keys;
    std::string set_id;
    bool all = false;
    bool open = false;
  } pending_;
};

}  // namespace recomp::achievements::ui
