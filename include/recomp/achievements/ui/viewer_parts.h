#pragma once

// Pieces of the achievement viewer's look (overlay.h), for apps that draw
// their own screens around it, such as the library app: the viewer's colours
// for a style and the footer's button and key hints.

#include <string_view>

#include "recomp/achievements/ui/canvas.h"
#include "recomp/achievements/ui/style.h"

namespace recomp::achievements::ui {

enum class ButtonLabels { kXbox, kPlayStation };

// The viewer's colours: the style's viewer colours, else its popups'.
struct ViewerTheme {
  Color backdrop, panel, accent, text, subtext, track, border, row, row_selected;
};
ViewerTheme ResolveViewerTheme(const Style& style);

Color MixColor(const Color& a, const Color& b, float t);
// Text colour that reads on top of c.
Color OnColor(const Color& c);

// What a hint glyph shows on a controller. Positions use Xbox names; with
// ButtonLabels::kPlayStation they draw as cross, circle, square, triangle,
// L1, R1, SELECT and OPTIONS.
enum class PadGlyph { kA, kB, kX, kY, kLB, kRB, kBack, kStart, kDpad, kDpadHorizontal };

// Draws the footer's hints: a controller glyph, or a key cap when the
// keyboard (or mouse) was used last.
struct HintPainter {
  Canvas& canvas;
  const Font& regular;
  const Font& bold;
  float scale = 1.0f;
  ButtonLabels labels = ButtonLabels::kXbox;
  bool keyboard = true;
  Color text, subtext;
  float alpha = 1.0f;

  // A glyph centred vertically on cy, left edge at x; key is the key cap's
  // label. Returns its width; draw = false only measures.
  float Glyph(PadGlyph pad, std::string_view key, float x, float cy, bool draw = true) const;
  // Glyph, then label in the subtext colour. Returns the total width.
  float Hint(PadGlyph pad, std::string_view key, std::string_view label, float x, float cy, bool draw = true) const;
};

// The tick of unlocked rows and checklist items.
void DrawCheckMark(Canvas& canvas, float cx, float cy, float k, float thickness, const Color& c);

}  // namespace recomp::achievements::ui
