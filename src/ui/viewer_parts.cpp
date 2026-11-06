#include "recomp/achievements/ui/viewer_parts.h"

#include <algorithm>

namespace recomp::achievements::ui {

namespace {

Color WithAlpha(Color c, float a) {
  c.a *= a;
  return c;
}

float Luma(const Color& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

}  // namespace

Color MixColor(const Color& a, const Color& b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

Color OnColor(const Color& c) { return Luma(c) > 0.55f ? Color::Rgba(16, 16, 18) : Color::Rgba(255, 255, 255); }

ViewerTheme ResolveViewerTheme(const Style& st) {
  const ViewerStyle& v = st.viewer;
  ViewerTheme t;
  t.backdrop = v.backdrop.value_or(Color::Rgba(0, 0, 0, 165));
  // Opaque: a see-through panel lets the game's edges ghost through the text.
  Color panel = st.unlock.colors.background;
  panel.a = 1.0f;
  t.panel = v.panel.value_or(panel);
  t.accent = v.accent.value_or(st.progress.colors.bar_fill);
  t.text = v.text.value_or(st.unlock.colors.name);
  t.subtext = v.subtext.value_or(st.unlock.colors.description);
  t.track = st.progress.colors.bar_track;
  t.border = WithAlpha(t.text, 0.10f);
  t.row = MixColor(t.panel, t.text, 0.045f);
  t.row_selected = MixColor(t.panel, t.accent, 0.20f);
  t.row.a = t.row_selected.a = 1.0f;
  return t;
}

void DrawCheckMark(Canvas& canvas, float cx, float cy, float k, float t, const Color& c) {
  canvas.Line(cx - k, cy, cx - k * 0.3f, cy + k * 0.7f, t, c);
  canvas.Line(cx - k * 0.3f, cy + k * 0.7f, cx + k, cy - k * 0.6f, t, c);
}

float HintPainter::Glyph(PadGlyph pad, std::string_view key, float x, float cy, bool draw) const {
  const float s = scale;
  const float d = 26.0f * s;  // Face button diameter.
  const float r = d * 0.5f;
  auto text_w = [&](float size, std::string_view t) { return bold.TextWidth(size, t); };
  auto label_text = [&](float size, float lx, std::string_view t) {
    canvas.Text(bold, size, lx, cy - size * 0.5f, t, WithAlpha(text, alpha));
  };
  if (keyboard) {
    const float ts = 14.0f * s;
    const float w = std::max(d, text_w(ts, key) + 14.0f * s);
    if (draw) {
      const RectF k{x, cy - r, x + w, cy + r};
      canvas.FillRect(k, WithAlpha(text, 0.10f * alpha), 5.0f * s);
      canvas.StrokeRect(k, WithAlpha(text, 0.45f * alpha), 5.0f * s, 1.25f * s);
      label_text(ts, x + (w - text_w(ts, key)) * 0.5f, key);
    }
    return w;
  }
  const bool ps = labels == ButtonLabels::kPlayStation;
  auto shoulder = [&](std::string_view label) {
    const float ts = 13.0f * s;
    const float w = std::max(d * 1.5f, text_w(ts, label) + 14.0f * s);
    if (draw) {
      const RectF k{x, cy - r * 0.85f, x + w, cy + r * 0.85f};
      canvas.FillRect(k, WithAlpha(text, 0.12f * alpha), r * 0.6f);
      canvas.StrokeRect(k, WithAlpha(text, 0.5f * alpha), r * 0.6f, 1.25f * s);
      label_text(ts, x + (w - text_w(ts, label)) * 0.5f, label);
    }
    return w;
  };
  auto face = [&](int which) {  // 0 a/cross, 1 b/circle, 2 x/square, 3 y/triangle
    if (!draw) return d;
    const float cx = x + r;
    if (!ps) {
      static const Color kColors[4] = {Color::Rgba(92, 176, 54), Color::Rgba(214, 58, 46),
                                       Color::Rgba(40, 112, 214), Color::Rgba(236, 178, 22)};
      static const char* kLetters[4] = {"A", "B", "X", "Y"};
      canvas.FillCircle(cx, cy, r, WithAlpha(kColors[which], alpha));
      const float ts = 16.0f * s;
      canvas.Text(bold, ts, cx - text_w(ts, kLetters[which]) * 0.5f, cy - ts * 0.5f, kLetters[which],
                  WithAlpha(Color::Rgba(255, 255, 255), alpha));
      return d;
    }
    static const Color kColors[4] = {Color::Rgba(124, 168, 228), Color::Rgba(232, 96, 92),
                                     Color::Rgba(222, 142, 200), Color::Rgba(64, 196, 164)};
    canvas.FillCircle(cx, cy, r, WithAlpha(Color::Rgba(28, 28, 32), alpha));
    canvas.StrokeCircle(cx, cy, r - 0.75f * s, 1.5f * s, WithAlpha(text, 0.35f * alpha));
    const Color c = WithAlpha(kColors[which], alpha);
    const float k = r * 0.42f, t = 2.2f * s;
    switch (which) {
      case 0:
        canvas.Line(cx - k, cy - k, cx + k, cy + k, t, c);
        canvas.Line(cx - k, cy + k, cx + k, cy - k, t, c);
        break;
      case 1: canvas.StrokeCircle(cx, cy, k, t, c); break;
      case 2: canvas.StrokeRect({cx - k * 0.9f, cy - k * 0.9f, cx + k * 0.9f, cy + k * 0.9f}, c, 0, t); break;
      default: {
        const Vec2 p0{cx, cy - k * 1.05f}, p1{cx + k * 1.05f, cy + k * 0.75f}, p2{cx - k * 1.05f, cy + k * 0.75f};
        canvas.Line(p0.x, p0.y, p1.x, p1.y, t, c);
        canvas.Line(p1.x, p1.y, p2.x, p2.y, t, c);
        canvas.Line(p2.x, p2.y, p0.x, p0.y, t, c);
      }
    }
    return d;
  };
  switch (pad) {
    case PadGlyph::kA: return face(0);
    case PadGlyph::kB: return face(1);
    case PadGlyph::kX: return face(2);
    case PadGlyph::kY: return face(3);
    case PadGlyph::kLB: return shoulder(ps ? "L1" : "LB");
    case PadGlyph::kRB: return shoulder(ps ? "R1" : "RB");
    case PadGlyph::kBack: return shoulder(ps ? "SELECT" : "BACK");
    case PadGlyph::kStart: return shoulder(ps ? "OPTIONS" : "START");
    case PadGlyph::kDpadHorizontal:
      // The d-pad with its left and right arms lit.
      if (draw) {
        const float cx = x + r, w = r * 0.36f;
        canvas.FillRect({cx - w, cy - r, cx + w, cy + r}, WithAlpha(text, 0.3f * alpha), w * 0.5f);
        canvas.FillRect({cx - r, cy - w, cx + r, cy + w}, WithAlpha(text, 0.85f * alpha), w * 0.5f);
      }
      return d;
    case PadGlyph::kDpad:
      if (draw) {
        const float cx = x + r, w = r * 0.36f;
        canvas.FillRect({cx - w, cy - r, cx + w, cy + r}, WithAlpha(text, 0.8f * alpha), w * 0.5f);
        canvas.FillRect({cx - r, cy - w, cx + r, cy + w}, WithAlpha(text, 0.8f * alpha), w * 0.5f);
      }
      return d;
  }
  return d;
}

float HintPainter::Hint(PadGlyph pad, std::string_view key, std::string_view label, float x, float cy,
                        bool draw) const {
  const float gw = Glyph(pad, key, x, cy, draw);
  const float ts = 15.0f * scale;
  if (draw) canvas.Text(regular, ts, x + gw + 8.0f * scale, cy - ts * 0.5f, label, WithAlpha(subtext, alpha));
  return gw + 8.0f * scale + regular.TextWidth(ts, label);
}

}  // namespace recomp::achievements::ui
