#pragma once

// Draws a NotificationCenter's popups with Dear ImGui. Only built when
// RECOMP_ACH_IMGUI_TARGET names an imgui target.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "recomp/achievements/ui/icon_cache.h"
#include "recomp/achievements/ui/notification_center.h"

namespace recomp::achievements::ui {

// Where a popup lands this frame, after scale, stacking and animation.
struct PopupLayout {
  ImVec2 min, max;            // Card rectangle this frame (animated).
  ImVec2 rest_min, rest_max;  // Where the card sits once fully in; text lays out here.
  float scale = 1.0f;   // UI scale: style sizes * scale = pixels.
  float alpha = 1.0f;
  void* icon = nullptr; // Texture, or null to draw the fallback badge.
  ImFont* font = nullptr;
  ImFont* font_bold = nullptr;
};

// The trophy drawn in place of a missing icon, centred on c with radius r.
void DrawTrophy(ImDrawList* draw_list, ImVec2 c, float r, ImU32 col);

inline ImTextureID ToImTextureID(void* texture) {
  return (ImTextureID)(uintptr_t)texture;
}

class ImGuiRenderer {
 public:
  // Return true when the popup was drawn; false falls back to the built-in look.
  using DrawCallback =
      std::function<bool(ImDrawList* draw_list, const ActivePopup& popup, const PopupLayout& layout)>;

  // icons may be null (fallback badges only). Both must outlive the renderer.
  explicit ImGuiRenderer(NotificationCenter& center, IconCache* icons = nullptr);

  void SetCustomDraw(DrawCallback callback);
  // Default fonts; null = ImGui's current font.
  void SetFonts(ImFont* regular, ImFont* bold = nullptr);
  // Fonts a Style can pick by name ("font": "...", "font_bold": "...").
  void RegisterFont(std::string name, ImFont* font);

  // Call once per frame inside an ImGui frame. Advances the center by
  // io.DeltaTime when advance is true, then draws onto the foreground list.
  void Draw(bool advance = true);
  // Draws into a given list and area (e.g. a game window or a preview pane).
  void Draw(ImDrawList* draw_list, ImVec2 origin, ImVec2 size, float dt);

  // Computes layouts without drawing (used by Draw and by custom renderers).
  // LayoutAll stacks popups that share an anchor (unlocks nearest the edge);
  // Layout places one popup as if it were alone in its stack.
  std::vector<PopupLayout> LayoutAll(const std::vector<ActivePopup>& popups, ImVec2 origin, ImVec2 size) const;
  PopupLayout Layout(const ActivePopup& popup, ImVec2 origin, ImVec2 size) const;
  // The card height the built-in look needs for this popup at scale s.
  float MeasureHeight(const ActivePopup& popup, float s) const;

  // Draws one popup with the built-in look (handy inside a custom callback).
  void DrawDefault(ImDrawList* draw_list, const ActivePopup& popup, const PopupLayout& layout) const;

  bool WantsRepaint() const { return center_.Busy(); }

 private:
  ImFont* FontFor(const Style& style, bool bold) const;
  PopupLayout Place(const ActivePopup& popup, ImVec2 origin, ImVec2 size, float stack_px) const;

  NotificationCenter& center_;
  IconCache* icons_;
  DrawCallback custom_;
  ImFont* font_ = nullptr;
  ImFont* font_bold_ = nullptr;
  std::map<std::string, ImFont*, std::less<>> fonts_;
};

}  // namespace recomp::achievements::ui
