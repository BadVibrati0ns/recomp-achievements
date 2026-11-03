// The software canvas: shapes, images, text and the shared popup painter.

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "recomp/achievements/ui/canvas.h"
#include "recomp/achievements/ui/painter.h"
#include "test.h"

using namespace recomp::achievements;
using namespace recomp::achievements::ui;

namespace {

struct Px {
  int r, g, b, a;
};

Px At(const Image& img, int x, int y) {
  const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
  return {p[0], p[1], p[2], p[3]};
}

bool Near(Px p, Px q, int tol = 2) {
  return std::abs(p.r - q.r) <= tol && std::abs(p.g - q.g) <= tol && std::abs(p.b - q.b) <= tol &&
         std::abs(p.a - q.a) <= tol;
}

// Bounding box of pixels with any alpha.
PixelRect Ink(const Image& img, PixelRect within = {0, 0, 1 << 20, 1 << 20}) {
  PixelRect r{1 << 20, 1 << 20, -1, -1};
  for (int y = std::max(0, within.y0); y < std::min(img.height, within.y1); ++y)
    for (int x = std::max(0, within.x0); x < std::min(img.width, within.x1); ++x)
      if (At(img, x, y).a) {
        r.x0 = std::min(r.x0, x);
        r.y0 = std::min(r.y0, y);
        r.x1 = std::max(r.x1, x + 1);
        r.y1 = std::max(r.y1, y + 1);
      }
  return r;
}

// Records the calls the shared popup look makes.
struct RecordingPainter : Painter {
  int fills = 0, bottom_rounded = 0, rings12 = 0, images = 0, texts = 0;
  std::vector<std::string> strings;
  float TextWidth(bool, float size, std::string_view t) const override { return size * 0.5f * t.size(); }
  void FillRect(Vec2, Vec2, const Color&, float, int corners) override {
    ++fills;
    if (corners == kCornersBottom) ++bottom_rounded;
  }
  void StrokeRect(Vec2, Vec2, const Color&, float, float) override {}
  void FillCircle(Vec2, float, const Color&, int) override {}
  void StrokeCircle(Vec2, float, const Color&, int segments, float) override { rings12 += segments == 12; }
  void DrawImage(void*, Vec2, Vec2, const Color&, float) override { ++images; }
  void Text(bool, float, Vec2, const Color&, std::string_view t) override {
    ++texts;
    strings.emplace_back(t);
  }
  void PushClip(Vec2, Vec2) override {}
  void PopClip() override {}
};

}  // namespace

TEST(canvas_fill_rect_pixels) {
  Canvas c(40, 30);
  CHECK_EQ(c.dirty(), (PixelRect{0, 0, 40, 30}));  // A resize is a change.
  c.ResetDirty();
  c.FillRect({10, 5, 20, 15}, Color::Rgba(255, 0, 0));
  const Image& img = c.image();
  CHECK(Near(At(img, 15, 10), {255, 0, 0, 255}));
  CHECK(Near(At(img, 10, 5), {255, 0, 0, 255}));
  CHECK(Near(At(img, 19, 14), {255, 0, 0, 255}));
  CHECK_EQ(At(img, 9, 10).a, 0);
  CHECK_EQ(At(img, 20, 10).a, 0);
  CHECK_EQ(At(img, 15, 15).a, 0);
  CHECK_EQ(c.dirty(), (PixelRect{10, 5, 20, 15}));

  // Half-pixel edges are anti-aliased: half coverage, straight colour.
  Canvas h(10, 10);
  h.FillRect({2.5f, 2, 6, 6}, Color::Rgba(0, 0, 255));
  const Px edge = At(h.image(), 2, 3);
  CHECK(edge.a > 110 && edge.a < 145);
  CHECK(edge.b > 250 && edge.r == 0);
}

TEST(canvas_rounded_rect_corners) {
  Canvas c(60, 60);
  c.FillRect({0, 0, 60, 60}, Color::Rgba(0, 255, 0), 20.0f);
  const Image& img = c.image();
  CHECK_EQ(At(img, 0, 0).a, 0);       // Outside the corner arc.
  CHECK_EQ(At(img, 30, 30).a, 255);   // Inside.
  CHECK_EQ(At(img, 30, 0).a, 255);    // Straight top edge.
  CHECK(At(img, 5, 6).a > 0 && At(img, 5, 6).a < 255);  // On the arc.
  // Only the bottom corners rounded.
  Canvas b(60, 60);
  b.FillRect({0, 0, 60, 60}, Color::Rgba(0, 255, 0), 20.0f, kCornersBottom);
  CHECK_EQ(At(b.image(), 0, 0).a, 255);
  CHECK_EQ(At(b.image(), 0, 59).a, 0);
}

TEST(canvas_stroke_circle_line_triangle) {
  Canvas c(100, 100);
  c.StrokeRect({10, 10, 90, 90}, Color::Rgba(255, 255, 255), 0.0f, 2.0f);
  CHECK_EQ(At(c.image(), 50, 10).a, 255);
  CHECK_EQ(At(c.image(), 50, 11).a, 255);
  CHECK_EQ(At(c.image(), 50, 13).a, 0);  // Hollow.
  CHECK_EQ(At(c.image(), 89, 50).a, 255);

  Canvas d(100, 100);
  d.FillCircle(50, 50, 20, Color::Rgba(255, 255, 0));
  CHECK_EQ(At(d.image(), 50, 50).a, 255);
  CHECK_EQ(At(d.image(), 50, 72).a, 0);
  CHECK_EQ(At(d.image(), 66, 66).a, 0);  // Outside the circle, inside its box.
  d.Clear();
  d.StrokeCircle(50, 50, 20, 3, Color::Rgba(255, 255, 0));
  CHECK_EQ(At(d.image(), 50, 50).a, 0);
  CHECK_EQ(At(d.image(), 70, 50).a, 255);
  d.Clear();
  d.Line(10, 10, 90, 10, 4, Color::Rgba(255, 0, 255));
  CHECK_EQ(At(d.image(), 50, 10).a, 255);
  CHECK_EQ(At(d.image(), 50, 14).a, 0);
  d.Clear();
  d.FillTriangle({50, 10}, {90, 90}, {10, 90}, Color::Rgba(0, 255, 255));
  CHECK_EQ(At(d.image(), 50, 60).a, 255);
  CHECK_EQ(At(d.image(), 15, 20).a, 0);
}

TEST(canvas_blend_and_premultiplied) {
  Canvas c(4, 4);
  c.FillRect({0, 0, 4, 4}, Color::Rgba(0, 0, 255));
  c.FillRect({0, 0, 4, 4}, Color::Rgba(255, 0, 0, 128));
  CHECK(Near(At(c.image(), 1, 1), {128, 0, 127, 255}));
  // Translucent over transparent keeps its colour in straight alpha...
  Canvas t(4, 4);
  t.FillRect({0, 0, 4, 4}, Color::Rgba(200, 100, 50, 64));
  CHECK(Near(At(t.image(), 1, 1), {200, 100, 50, 64}, 3));
  // ...and is scaled by alpha in the premultiplied buffer.
  CHECK(Near(At(t.premultiplied(), 1, 1), {50, 25, 13, 64}, 2));
  t.Fill({0, 0, 2, 4}, Color::Rgba(10, 20, 30, 255));
  CHECK(Near(At(t.image(), 0, 0), {10, 20, 30, 255}));
  CHECK(Near(At(t.image(), 3, 0), {200, 100, 50, 64}, 3));
}

TEST(canvas_clip_and_progress_bar) {
  Canvas c(50, 20);
  c.PushClip(PixelRect{0, 0, 25, 20});
  c.FillRect({0, 0, 50, 20}, Color::Rgba(255, 255, 255));
  c.PopClip();
  CHECK_EQ(At(c.image(), 24, 10).a, 255);
  CHECK_EQ(At(c.image(), 25, 10).a, 0);

  Canvas p(100, 10);
  p.ProgressBar({0, 0, 100, 10}, 0.5f, Color::Rgba(0, 0, 0), Color::Rgba(255, 255, 255));
  CHECK(Near(At(p.image(), 25, 5), {255, 255, 255, 255}));
  CHECK(Near(At(p.image(), 75, 5), {0, 0, 0, 255}));
  CHECK_EQ(At(p.image(), 0, 0).a, 0);  // Round ends.
}

TEST(canvas_image_blit_scaled_tinted) {
  Image src;
  src.width = src.height = 2;
  src.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 0};
  Canvas c(40, 40);
  c.DrawImage(src, {0, 0, 40, 40});
  CHECK(Near(At(c.image(), 2, 2), {255, 0, 0, 255}));
  CHECK(Near(At(c.image(), 37, 2), {0, 255, 0, 255}));
  CHECK(Near(At(c.image(), 2, 37), {0, 0, 255, 255}));
  CHECK_EQ(At(c.image(), 37, 37).a, 0);  // Transparent texel stays transparent.
  // No dark fringe from the transparent texel's colour: premultiplied filtering.
  const Px mid = At(c.image(), 30, 30);
  CHECK(mid.a < 255);

  Canvas t(10, 10);
  Image white;
  white.width = white.height = 1;
  white.rgba = {255, 255, 255, 255};
  t.DrawImage(white, {0, 0, 10, 10}, Color{0.5f, 0.5f, 0.5f, 1.0f});
  CHECK(Near(At(t.image(), 5, 5), {128, 128, 128, 255}));
  // Rounded to a circle.
  Canvas r(20, 20);
  r.DrawImage(white, {0, 0, 20, 20}, Color{}, 10.0f);
  CHECK_EQ(At(r.image(), 0, 0).a, 0);
  CHECK_EQ(At(r.image(), 10, 10).a, 255);

  // A large image goes through its mips.
  Image big;
  big.width = big.height = 256;
  big.rgba.assign(256 * 256 * 4, 255);
  const CanvasImage ci = CanvasImage::From(big);
  CHECK_EQ(ci.mips.size(), 8u);
  CHECK_EQ(ci.mips.back().width, 1);
  Canvas m(16, 16);
  m.DrawImage(ci, {0, 0, 16, 16});
  CHECK(Near(At(m.image(), 8, 8), {255, 255, 255, 255}));
}

TEST(font_measure_and_draw) {
  auto font = Font::Builtin(false);
  auto bold = Font::Builtin(true);
  CHECK(font != nullptr);
  CHECK(bold != nullptr);
  if (!font || !bold) return;
  CHECK(font->TextWidth(20, "") == 0.0f);
  CHECK(font->TextWidth(20, "iii") < font->TextWidth(20, "WWW"));
  CHECK_NEAR(font->TextWidth(40, "Hello"), 2 * font->TextWidth(20, "Hello"), 0.01);
  CHECK(bold->TextWidth(20, "Hello") > font->TextWidth(20, "Hello"));
  CHECK(font->HasGlyph(0xE9));  // é
  CHECK(font->TextWidth(20, "caf\xC3\xA9") > font->TextWidth(20, "caf"));
  CHECK(font->TextWidth(20, "\xFF") > 0.0f);  // Invalid UTF-8 draws a replacement.
  CHECK(font->Ascent(20) > 10 && font->Ascent(20) < 20);

  Canvas c(200, 40);
  const float w = c.Text(*font, 20, 10, 10, "Hello", Color::Rgba(255, 255, 255));
  CHECK_NEAR(w, font->TextWidth(20, "Hello"), 0.01);
  const PixelRect ink = Ink(c.image());
  CHECK(ink.x0 >= 10 && ink.x0 <= 13);
  CHECK(ink.x1 <= 10 + static_cast<int>(w) + 2);
  CHECK(ink.y0 >= 10 && ink.y1 <= 31);  // Within the 20 px line.
  CHECK(font->cached_glyphs() > 0);
}

TEST(font_wrap_and_ellipsis) {
  auto font = Font::Builtin(false);
  if (!font) return;
  const std::string text = "The quick brown fox jumps over the lazy dog and keeps on running far away";
  const float max_w = 150;
  const auto lines = Canvas::WrapText(*font, 16, text, max_w, 3);
  CHECK_EQ(lines.size(), 3u);
  for (const auto& l : lines) CHECK(font->TextWidth(16, l) <= max_w + 0.01f);
  CHECK(lines.back().size() >= 3 && lines.back().substr(lines.back().size() - 3) == "...");
  CHECK(lines[0].rfind("The quick", 0) == 0);
  const auto all = Canvas::WrapText(*font, 16, text, max_w, 20);
  CHECK(all.size() > 3);
  CHECK(all.back().find("...") == std::string::npos);
  // One word wider than the line is cut by characters.
  const auto cut = Canvas::WrapText(*font, 16, "Supercalifragilisticexpialidocious", 60, 4);
  CHECK(cut.size() >= 2);
  const std::string e = Canvas::EllipsizeText(*font, 16, "A long achievement name", 80);
  CHECK(font->TextWidth(16, e) <= 80);
  CHECK(e.substr(e.size() - 3) == "...");
  CHECK_EQ(Canvas::EllipsizeText(*font, 16, "Short", 200), std::string("Short"));
  // Never splits a UTF-8 sequence.
  const std::string u = Canvas::EllipsizeText(*font, 16, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 40);
  CHECK((static_cast<unsigned char>(u[u.size() - 4]) & 0xC0) != 0xC0);
}

TEST(font_from_ttf_and_synthetic_bold) {
  CHECK(Font::FromTtf({}) == nullptr);
  const uint8_t junk[16] = {1, 2, 3};
  CHECK(Font::FromTtf(junk) == nullptr);
  auto font = Font::Builtin(false);
  if (!font) return;
#ifdef RECOMP_ACH_UI_FONT_DIR
  // A game's own TTF bytes load the same as the bundled copy.
  std::vector<uint8_t> bytes;
  CHECK(ReadFileBytes(std::string(RECOMP_ACH_UI_FONT_DIR) + "/DejaVuSans-Bold.ttf", bytes));
  auto loaded = Font::FromTtf(bytes);
  CHECK(loaded != nullptr);
  if (loaded && Font::Builtin(true))
    CHECK_NEAR(loaded->TextWidth(18, "Bold"), Font::Builtin(true)->TextWidth(18, "Bold"), 1e-4);
#endif
  // Thickened glyphs measure wider, as the overlay uses for a game font with no bold face.
  const float plain = font->TextWidth(18, "Bold");
  font->SetSyntheticBold(true);
  CHECK(font->TextWidth(18, "Bold") > plain);
  font->SetSyntheticBold(false);
  CHECK_NEAR(font->TextWidth(18, "Bold"), plain, 1e-4);
}

TEST(painter_popup_draws_trophy_without_icon) {
  auto style = std::make_shared<Style>(BuiltinTheme("minimal"));
  ActivePopup a;
  a.n.name = "Flip Book";
  a.n.style = style;
  a.alpha = 1.0f;
  a.expand = 1.0f;
  const PopupRect r = PlacePopup(a, {0, 0}, {1920, 1080}, 0, RecordingPainter());
  CHECK(r.max.x > r.min.x && r.max.y > r.min.y);
  RecordingPainter p;
  DrawPopup(p, a, r);
  CHECK_EQ(p.bottom_rounded, 1);  // The cup.
  CHECK_EQ(p.rings12, 2);         // Its handles.
  CHECK_EQ(p.images, 0);
  CHECK(std::find(p.strings.begin(), p.strings.end(), "Flip Book") != p.strings.end());

  // With an icon the trophy isn't drawn; with a badge the text replaces it.
  PopupRect with_icon = r;
  int dummy = 0;
  with_icon.icon = &dummy;
  RecordingPainter q;
  DrawPopup(q, a, with_icon);
  CHECK_EQ(q.images, 1);
  CHECK_EQ(q.rings12, 0);
  auto badged = std::make_shared<Style>(*style);
  badged->unlock.badge = "XP";
  a.n.style = badged;
  RecordingPainter b;
  DrawPopup(b, a, r);
  CHECK_EQ(b.rings12, 0);
  CHECK(std::find(b.strings.begin(), b.strings.end(), "XP") != b.strings.end());
}

TEST(painter_layout_stacks_and_scales) {
  auto style = std::make_shared<Style>(BuiltinTheme("minimal"));
  style->unlock.animation = Animation::kNone;
  style->unlock.anchor = Anchor::kTopLeft;
  style->unlock.offset_x = 20;
  style->unlock.offset_y = 30;
  std::vector<ActivePopup> ps(2);
  for (int i = 0; i < 2; ++i) {
    ps[i].n.name = "Name";
    ps[i].n.style = style;
    ps[i].slot = i;
    ps[i].stack_pos = static_cast<float>(i);
    ps[i].alpha = 1;
  }
  RecordingPainter m;
  const auto rects = LayoutPopups(ps, {0, 0}, {3840, 2160}, m);
  CHECK_EQ(rects.size(), 2u);
  CHECK_NEAR(rects[0].scale, 2.0, 1e-5);
  CHECK_NEAR(rects[0].min.x, 40, 1e-3);
  CHECK_NEAR(rects[0].min.y, 60, 1e-3);
  CHECK(rects[1].min.y >= rects[0].max.y);  // Stacked below, away from the top edge.

  // The canvas painter draws the same popup into pixels.
  auto font = Font::Builtin(false), bold = Font::Builtin(true);
  if (!font) return;
  Canvas c(1920, 1080);
  CanvasPainter cp(c, font.get(), bold.get());
  const auto r = PlacePopup(ps[0], {0, 0}, {1920, 1080}, 0, cp);
  DrawPopup(cp, ps[0], r);
  const PixelRect ink = Ink(c.image());
  CHECK(ink.x0 >= static_cast<int>(r.min.x) - 4 && ink.x1 <= static_cast<int>(r.max.x) + 4);
  CHECK(ink.y0 >= static_cast<int>(r.min.y) - 4 && ink.y1 <= static_cast<int>(r.max.y) + 8);
  // The trophy sits in the icon square, in the name colour.
  const int ix = static_cast<int>(r.min.x + style->unlock.padding + style->unlock.icon_size * 0.5f);
  const int iy = static_cast<int>((r.min.y + r.max.y) * 0.5f - style->unlock.icon_size * 0.15f);
  const Px cup = At(c.image(), ix, iy);
  CHECK(cup.r > 200 && cup.g > 200 && cup.b > 200);
}
