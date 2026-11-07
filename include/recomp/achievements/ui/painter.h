#pragma once

// The built-in popup look, written once against a few drawing primitives so
// every renderer draws the same popups: the ImGui renderer implements Painter
// with an ImDrawList, the overlay with a software Canvas. Layout (anchors,
// stacking, animation), text formatting and the fallback trophy live here.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/ui/notification_center.h"
#include "recomp/achievements/ui/style.h"

namespace recomp::achievements::ui {

struct Vec2 {
  float x = 0.0f, y = 0.0f;
};

enum Corners : int {
  kCornerTopLeft = 1,
  kCornerTopRight = 2,
  kCornerBottomLeft = 4,
  kCornerBottomRight = 8,
  kCornersTop = kCornerTopLeft | kCornerTopRight,
  kCornersBottom = kCornerBottomLeft | kCornerBottomRight,
  kCornersAll = kCornersTop | kCornersBottom,
};

// Text metrics for layout. A line of text at `size` is `size` pixels tall and
// its top sits at the y passed to Painter::Text.
class TextMeasurer {
 public:
  virtual ~TextMeasurer() = default;
  virtual float TextWidth(bool bold, float size, std::string_view text) const = 0;
};

// Colours arrive with the popup's fade already multiplied into alpha.
class Painter : public TextMeasurer {
 public:
  virtual void FillRect(Vec2 min, Vec2 max, const Color& c, float rounding = 0.0f, int corners = kCornersAll) = 0;
  virtual void StrokeRect(Vec2 min, Vec2 max, const Color& c, float rounding, float thickness) = 0;
  // segments is a hint for tessellating renderers.
  virtual void FillCircle(Vec2 center, float radius, const Color& c, int segments) = 0;
  virtual void StrokeCircle(Vec2 center, float radius, const Color& c, int segments, float thickness) = 0;
  // texture is whatever the IconCache's CreateTexture returned.
  virtual void DrawImage(void* texture, Vec2 min, Vec2 max, const Color& tint, float rounding) = 0;
  virtual void Text(bool bold, float size, Vec2 pos, const Color& c, std::string_view text) = 0;
  virtual void PushClip(Vec2 min, Vec2 max) = 0;
  virtual void PopClip() = 0;
};

// c with its alpha multiplied by alpha (clamped to 0..1).
Color Fade(const Color& c, float alpha);

// The trophy drawn in place of a missing icon, centred on c with radius r.
void DrawTrophy(Painter& painter, Vec2 c, float r, const Color& col);

// UTF-8 aware. Ellipsize cuts s to max_w with "..."; WrapText word-wraps into
// at most max_lines, ellipsising the last one.
std::string Ellipsize(const TextMeasurer& m, bool bold, float size, std::string s, float max_w);
std::vector<std::string> WrapText(const TextMeasurer& m, bool bold, float size, const std::string& text,
                                  float max_w, int max_lines);

// Where a popup lands this frame, after scale, stacking and animation.
struct PopupRect {
  Vec2 min, max;            // Card rectangle this frame (animated).
  Vec2 rest_min, rest_max;  // Where the card sits once fully in; text lays out here.
  float scale = 1.0f;       // UI scale: style sizes * scale = pixels.
  float alpha = 1.0f;
  void* icon = nullptr;     // Texture, or null to draw the fallback badge.
};

// Style sizes * this = pixels for a display of the given size.
float PopupScale(const Style& style, Vec2 display_size);
// The least scale a display this tall gets (the style's small_screen_* rule);
// 0 above small_screen_height or when it's off.
float SmallScreenScale(const Style& style, float display_height);
// The card height the built-in look needs for this popup at scale s.
float MeasurePopupHeight(const ActivePopup& popup, float s, const TextMeasurer& m);
// One popup at a stack offset in pixels (0 = nearest its anchor).
PopupRect PlacePopup(const ActivePopup& popup, Vec2 origin, Vec2 size, float stack_px, const TextMeasurer& m);
// Every popup; those sharing an anchor and offset stack (unlocks nearest the edge).
std::vector<PopupRect> LayoutPopups(const std::vector<ActivePopup>& popups, Vec2 origin, Vec2 size,
                                    const TextMeasurer& m);
// The built-in look. Draws nothing for a fully faded or collapsed card.
void DrawPopup(Painter& painter, const ActivePopup& popup, const PopupRect& rect);

}  // namespace recomp::achievements::ui
