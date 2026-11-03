#pragma once

// A small CPU rasteriser for games that draw the achievement UI without Dear
// ImGui: anti-aliased rects, rounded rects, circles, lines, scaled images and
// TrueType text (stb_truetype) into an RGBA8 image the game uploads as a
// texture. Works in premultiplied alpha internally; image() is straight alpha
// like ui::Image. A Canvas is used by one thread at a time; Fonts may be
// shared between threads.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/ui/icon_cache.h"
#include "recomp/achievements/ui/painter.h"
#include "recomp/achievements/ui/style.h"

namespace recomp::achievements::ui {

// Integer pixel rectangle, [x0, x1) x [y0, y1).
struct PixelRect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
  int width() const { return x1 - x0; }
  int height() const { return y1 - y0; }
  bool operator==(const PixelRect&) const = default;
};
PixelRect Union(const PixelRect& a, const PixelRect& b);  // Empty rects are ignored.
PixelRect Intersect(const PixelRect& a, const PixelRect& b);

struct RectF {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float width() const { return x1 - x0; }
  float height() const { return y1 - y0; }
};

// A TrueType/OpenType font with a glyph cache per pixel size. Sizes are the
// line height like ImGui's (ascent - descent), so layouts measured here match
// ImGui with the same font file.
class Font {
 public:
  // Copies the font bytes; null when stb_truetype can't read them.
  static std::shared_ptr<Font> FromTtf(std::span<const uint8_t> ttf, int index = 0);
  // The bundled DejaVu Sans / DejaVu Sans Bold, shared; null when the SDK was
  // built with RECOMP_ACH_NO_EMBEDDED_FONTS.
  static std::shared_ptr<Font> Builtin(bool bold);

  ~Font();
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;

  // Pixels from the line top to the baseline.
  float Ascent(float size) const;
  float TextWidth(float size, std::string_view utf8) const;
  bool HasGlyph(uint32_t codepoint) const;
  // Used for code points this font lacks (e.g. a game font without accents).
  void SetFallback(std::shared_ptr<Font> fallback);
  // Thickens glyphs, for drawing "bold" with a font that has no bold face.
  void SetSyntheticBold(bool on);
  size_t cached_glyphs() const;

 private:
  Font();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend class Canvas;
};

// An Image with a mip chain, so large icons scale down smoothly. What
// UseImageTextures() hands out as "textures".
struct CanvasImage {
  Image image;
  std::vector<Image> mips;  // Halves of image, then of each other, down to 1 px.
  static CanvasImage From(Image image);
};

// Makes an IconCache's textures CanvasImage* (as void*) for Canvas drawing.
void UseImageTextures(IconCache& cache);

class Canvas {
 public:
  Canvas() = default;
  Canvas(int width, int height) { Resize(width, height); }

  // Clears to transparent.
  void Resize(int width, int height);
  int width() const { return width_; }
  int height() const { return height_; }

  // The pixels, straight alpha (converted from the working buffer on demand).
  const Image& image() const;
  // The working buffer, premultiplied alpha, for games that blend that way.
  const Image& premultiplied() const { return premul_; }

  // Pixels touched since ResetDirty(); empty when none.
  PixelRect dirty() const { return dirty_; }
  void ResetDirty() { dirty_ = {}; }

  void Clear();
  void Clear(PixelRect r);
  // Sets every pixel in r to c (no blending), e.g. a backdrop.
  void Fill(PixelRect r, const Color& c);

  // Drawing is limited to the clip, intersected with the canvas.
  void PushClip(PixelRect r);
  void PushClip(RectF r);  // Rounded to whole pixels.
  void PopClip();
  PixelRect clip() const;

  // Anti-aliased shapes. Coordinates are pixels; pixel (x, y) covers
  // [x, x + 1) x [y, y + 1). Strokes lie inside the rect, centred on circles.
  void FillRect(RectF r, const Color& c, float radius = 0.0f, int corners = kCornersAll);
  void StrokeRect(RectF r, const Color& c, float radius, float thickness);
  void FillCircle(float cx, float cy, float radius, const Color& c);
  void StrokeCircle(float cx, float cy, float radius, float thickness, const Color& c);
  void Line(float x0, float y0, float x1, float y1, float thickness, const Color& c);  // Round caps.
  void FillTriangle(Vec2 a, Vec2 b, Vec2 c, const Color& col);
  // A rounded track with `fraction` (0..1) of it filled.
  void ProgressBar(RectF r, float fraction, const Color& track, const Color& fill);

  // Bilinear-scaled blit; tint multiplies the image (alpha included). radius
  // rounds the corners (width / 2 for a circle).
  void DrawImage(const Image& image, RectF dst, const Color& tint = {}, float radius = 0.0f);
  void DrawImage(const CanvasImage& image, RectF dst, const Color& tint = {}, float radius = 0.0f);

  // UTF-8 text with its line top at y; returns the width drawn.
  float Text(const Font& font, float size, float x, float y, std::string_view text, const Color& c);
  static float MeasureText(const Font& font, float size, std::string_view text) {
    return font.TextWidth(size, text);
  }
  // Wraps text into at most max_lines; the last line is ellipsised.
  static std::vector<std::string> WrapText(const Font& font, float size, const std::string& text, float max_w,
                                           int max_lines);
  static std::string EllipsizeText(const Font& font, float size, std::string text, float max_w);

 private:
  struct Premul {
    uint32_t px;  // Premultiplied RGBA, little-endian byte order.
    int a;
  };
  static Premul ToPremul(const Color& c, float coverage);
  void Touch(PixelRect r);
  PixelRect Bounds(float x0, float y0, float x1, float y1) const;
  void Span(int y, int x0, int x1, const Color& c, float coverage);
  void Pixel(int x, int y, const Color& c, float coverage);
  void BlendPremul(uint32_t* d, uint32_t s) const;
  template <typename Coverage>
  void FillShape(PixelRect box, const Color& c, Coverage&& coverage);
  void Blit(const Image& img, RectF dst, const Color& tint, float radius);

  int width_ = 0, height_ = 0;
  Image premul_;
  mutable Image straight_;
  mutable PixelRect stale_;  // Area of straight_ older than premul_.
  PixelRect dirty_;
  std::vector<PixelRect> clips_;
};

// Painter over a Canvas, so popups look the same as with the ImGui renderer.
// Textures are CanvasImage* (see UseImageTextures). bold may be null.
class CanvasPainter : public Painter {
 public:
  CanvasPainter(Canvas& canvas, const Font* regular, const Font* bold = nullptr)
      : canvas_(canvas), regular_(regular), bold_(bold ? bold : regular) {}

  float TextWidth(bool bold, float size, std::string_view text) const override;
  void FillRect(Vec2 min, Vec2 max, const Color& c, float rounding = 0.0f, int corners = kCornersAll) override;
  void StrokeRect(Vec2 min, Vec2 max, const Color& c, float rounding, float thickness) override;
  void FillCircle(Vec2 center, float radius, const Color& c, int segments) override;
  void StrokeCircle(Vec2 center, float radius, const Color& c, int segments, float thickness) override;
  void DrawImage(void* texture, Vec2 min, Vec2 max, const Color& tint, float rounding) override;
  void Text(bool bold, float size, Vec2 pos, const Color& c, std::string_view text) override;
  void PushClip(Vec2 min, Vec2 max) override;
  void PopClip() override;

 private:
  Canvas& canvas_;
  const Font* regular_;
  const Font* bold_;
};

// Text widths without a canvas, for layout code.
class FontMeasurer : public TextMeasurer {
 public:
  FontMeasurer(const Font* regular, const Font* bold) : regular_(regular), bold_(bold ? bold : regular) {}
  float TextWidth(bool bold, float size, std::string_view text) const override {
    const Font* f = bold ? bold_ : regular_;
    return f ? f->TextWidth(size, text) : 0.0f;
  }

 private:
  const Font* regular_;
  const Font* bold_;
};

}  // namespace recomp::achievements::ui
