#pragma once

// A ready-made Dear ImGui achievements window for games with no settings UI
// of their own: sets and categories, filters, progress bars, checklists,
// tiers/points, credits and secret handling. Only built with an imgui target.

#include <string>

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

 private:
  System& system_;
  IconCache* icons_;
  ListOptions options_;
  std::string selected_set_;  // "" = all sets.
  ListFilter filter_ = ListFilter::kAll;
  bool reveal_secrets_ = false;
  char search_[96] = {};
};

}  // namespace recomp::achievements::ui
