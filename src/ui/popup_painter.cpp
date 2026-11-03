#include "recomp/achievements/ui/painter.h"

#include <algorithm>
#include <cmath>

namespace recomp::achievements::ui {

namespace {

// Drops the last UTF-8 code point.
void PopCodePoint(std::string& s) {
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  if (!s.empty()) s.pop_back();
}

// What the built-in look draws, measured for one popup.
struct Rows {
  PopupStrings text;
  std::vector<std::string> description;
  float title_size = 0, name_size = 0, points_size = 0, desc_size = 0;
  float icon = 0;        // Icon edge in pixels, 0 = none.
  float text_x = 0;      // Text column start, relative to the card's left.
  float text_w = 0;      // Text column width.
  float pill_w = 0;      // Points pill width (0 = points drawn inline or not at all).
  float inline_points_w = 0;
  bool bar = false;
  float bar_h = 0;
  float content_h = 0;   // Text block height.
  float gap = 0;
  float height = 0;      // Card height.
};

Rows Measure(const ActivePopup& a, float s, const TextMeasurer& m) {
  Rows r;
  const PopupStyle& p = a.n.popup();
  const float fs = s * p.font_scale;
  r.text = FormatPopup(a.n);
  r.title_size = p.title_size * fs;
  r.name_size = p.name_size * fs;
  r.points_size = p.points_size * fs;
  r.desc_size = p.description_size * fs;
  r.gap = 3.0f * s;
  const float pad = p.padding * s;
  const float width = p.width * s;
  r.icon = p.lines.icon ? p.icon_size * s : 0.0f;
  r.text_x = pad + (r.icon > 0 ? r.icon + pad : 0.0f);
  float right = width - pad;
  if (!r.text.points.empty()) {
    if (p.points_pill) {
      r.pill_w = m.TextWidth(true, r.points_size, r.text.points) + 20.0f * s;
      right -= r.pill_w + 10.0f * s;
    } else {
      r.inline_points_w = m.TextWidth(true, r.points_size, r.text.points);
    }
  }
  r.text_w = std::max(40.0f * s, right - r.text_x);
  r.bar = p.lines.bar && a.n.kind == NotificationKind::kProgress && a.n.total > 0;
  r.bar_h = 6.0f * s;
  r.description = WrapText(m, false, r.desc_size, r.text.description, r.text_w, p.description_lines);

  float h = 0;
  int rows = 0;
  auto add = [&](float row_h) {
    h += row_h;
    ++rows;
  };
  // Inline points sit on the first row; give them a row if there's none.
  const bool any_head = !r.text.title.empty() || !r.text.name.empty();
  if (!r.text.title.empty()) add(r.title_size);
  if (!r.text.name.empty()) add(r.name_size);
  if (!any_head && r.inline_points_w > 0) add(r.points_size);
  if (r.bar) add(r.bar_h + 4.0f * s);
  for (size_t i = 0; i < r.description.size(); ++i) add(r.desc_size);
  if (rows > 1) h += r.gap * (rows - 1);
  r.content_h = h;
  r.height = std::max({p.min_height * s, h + pad * 2.0f, r.icon + pad * 2.0f});
  return r;
}

}  // namespace

Color Fade(const Color& c, float alpha) { return {c.r, c.g, c.b, c.a * std::clamp(alpha, 0.0f, 1.0f)}; }

void DrawTrophy(Painter& p, Vec2 c, float r, const Color& col) {
  p.FillRect({c.x - 0.42f * r, c.y - 0.5f * r}, {c.x + 0.42f * r, c.y + 0.1f * r}, col, 0.4f * r, kCornersBottom);
  p.StrokeCircle({c.x - 0.45f * r, c.y - 0.25f * r}, 0.18f * r, col, 12, 0.08f * r);
  p.StrokeCircle({c.x + 0.45f * r, c.y - 0.25f * r}, 0.18f * r, col, 12, 0.08f * r);
  p.FillRect({c.x - 0.08f * r, c.y + 0.1f * r}, {c.x + 0.08f * r, c.y + 0.32f * r}, col);
  p.FillRect({c.x - 0.3f * r, c.y + 0.32f * r}, {c.x + 0.3f * r, c.y + 0.46f * r}, col, 0.05f * r);
}

std::string Ellipsize(const TextMeasurer& m, bool bold, float size, std::string s, float max_w) {
  if (m.TextWidth(bold, size, s) <= max_w) return s;
  while (!s.empty() && m.TextWidth(bold, size, s + "...") > max_w) PopCodePoint(s);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s + "...";
}

std::vector<std::string> WrapText(const TextMeasurer& m, bool bold, float size, const std::string& text,
                                  float max_w, int max_lines) {
  std::vector<std::string> lines;
  if (text.empty() || max_lines <= 0) return lines;
  size_t pos = 0;
  while (pos < text.size() && static_cast<int>(lines.size()) < max_lines) {
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size()) break;
    const bool last = static_cast<int>(lines.size()) == max_lines - 1;
    if (last) {
      lines.push_back(Ellipsize(m, bold, size, text.substr(pos), max_w));
      break;
    }
    size_t end = pos, best = std::string::npos;
    while (end <= text.size()) {
      const size_t sp = text.find(' ', end);
      const size_t cut = sp == std::string::npos ? text.size() : sp;
      if (m.TextWidth(bold, size, std::string_view(text).substr(pos, cut - pos)) > max_w) break;
      best = cut;
      if (sp == std::string::npos) break;
      end = sp + 1;
    }
    if (best == std::string::npos) {
      // One word wider than the line: cut it by characters.
      const std::string_view w = std::string_view(text).substr(pos);
      size_t n = w.size();
      while (n > 1 && m.TextWidth(bold, size, w.substr(0, n)) > max_w) {
        --n;
        while (n > 1 && (static_cast<unsigned char>(w[n]) & 0xC0) == 0x80) --n;
      }
      best = pos + n;
    }
    lines.push_back(text.substr(pos, best - pos));
    pos = best;
  }
  return lines;
}

float PopupScale(const Style& st, Vec2 size) {
  return std::clamp(size.y / std::max(1.0f, st.reference_height), st.min_scale, st.max_scale) * st.scale;
}

float MeasurePopupHeight(const ActivePopup& popup, float s, const TextMeasurer& m) {
  return Measure(popup, s, m).height;
}

std::vector<PopupRect> LayoutPopups(const std::vector<ActivePopup>& popups, Vec2 origin, Vec2 size,
                                    const TextMeasurer& m) {
  // Popups at the same anchor and offset share one stack, so an unlock and a
  // progress popup never cover each other. Active() lists unlocks first.
  struct Key {
    Anchor anchor;
    float x, y;
  };
  std::vector<Key> keys;
  std::vector<float> stack_px;  // Running offset per key.
  std::vector<PopupRect> out;
  out.reserve(popups.size());
  for (const ActivePopup& a : popups) {
    const PopupStyle& p = a.n.popup();
    size_t k = 0;
    while (k < keys.size() &&
           !(keys[k].anchor == p.anchor && keys[k].x == p.offset_x && keys[k].y == p.offset_y))
      ++k;
    if (k == keys.size()) {
      keys.push_back({p.anchor, p.offset_x, p.offset_y});
      stack_px.push_back(0.0f);
    }
    const float s = PopupScale(*a.n.style, size);
    const float step = MeasurePopupHeight(a, s, m) + p.spacing * s;
    // stack_pos lags slot while gliding into a freed space.
    out.push_back(PlacePopup(a, origin, size, stack_px[k] + (a.stack_pos - static_cast<float>(a.slot)) * step, m));
    stack_px[k] += step;
  }
  return out;
}

PopupRect PlacePopup(const ActivePopup& a, Vec2 origin, Vec2 size, float stack_px, const TextMeasurer& m) {
  const Style& st = *a.n.style;
  const PopupStyle& p = a.n.popup();
  PopupRect L;
  L.scale = PopupScale(st, size);
  L.alpha = a.alpha;
  const float s = L.scale;
  const float w = p.width * s;
  const float h = Measure(a, s, m).height;
  const float ox = p.offset_x * s, oy = p.offset_y * s;

  int col = 1, row = 2;  // 0/1/2 = left/centre/right, top/middle/bottom.
  switch (p.anchor) {
    case Anchor::kTopLeft: col = 0, row = 0; break;
    case Anchor::kTopCenter: col = 1, row = 0; break;
    case Anchor::kTopRight: col = 2, row = 0; break;
    case Anchor::kMiddleLeft: col = 0, row = 1; break;
    case Anchor::kCenter: col = 1, row = 1; break;
    case Anchor::kMiddleRight: col = 2, row = 1; break;
    case Anchor::kBottomLeft: col = 0, row = 2; break;
    case Anchor::kBottomCenter: col = 1, row = 2; break;
    case Anchor::kBottomRight: col = 2, row = 2; break;
  }
  float x = col == 0 ? origin.x + ox : col == 1 ? origin.x + (size.x - w) * 0.5f + ox : origin.x + size.x - ox - w;
  float y = row == 0 ? origin.y + oy : row == 1 ? origin.y + (size.y - h) * 0.5f + oy : origin.y + size.y - oy - h;

  // Stack away from the anchored edge.
  y += row == 2 ? -stack_px : stack_px;
  L.rest_min = {x, y};
  L.rest_max = {x + w, y + h};

  // Slides travel toward the nearest edge.
  const float d = p.slide_distance * s * a.slide;
  float dx = 0, dy = 0;
  if (row == 0) dy = -d;
  else if (row == 2) dy = d;
  else if (col == 0) dx = -d;
  else if (col == 2) dx = d;
  else dy = d;
  L.min = {x + dx, y + dy};
  L.max = {x + w + dx, y + h + dy};

  if (p.animation == Animation::kExpand && a.expand < 1.0f) {
    // Grow from the icon-sized card toward the rest width.
    const float collapsed = std::min(w, p.lines.icon ? (p.icon_size + p.padding * 2.0f) * s : h);
    const float cur = collapsed + (w - collapsed) * std::clamp(a.expand, 0.0f, 1.0f);
    if (col == 0) L.max.x = L.min.x + cur;
    else if (col == 2) L.min.x = L.max.x - cur;
    else {
      const float cx = (L.min.x + L.max.x) * 0.5f;
      L.min.x = cx - cur * 0.5f;
      L.max.x = cx + cur * 0.5f;
    }
  } else if (p.animation == Animation::kPop && a.expand != 1.0f) {
    const float k = std::max(0.01f, a.expand);
    const Vec2 c{(L.min.x + L.max.x) * 0.5f, (L.min.y + L.max.y) * 0.5f};
    L.min = {c.x - w * 0.5f * k, c.y - h * 0.5f * k};
    L.max = {c.x + w * 0.5f * k, c.y + h * 0.5f * k};
    L.rest_min = L.min;
    L.rest_max = L.max;
    L.scale *= k;
  }
  return L;
}

void DrawPopup(Painter& dl, const ActivePopup& a, const PopupRect& L) {
  const Style& st = *a.n.style;
  const PopupStyle& p = a.n.popup();
  const PopupColors& c = p.colors;
  const float s = L.scale;
  const float alpha = L.alpha;
  if (alpha <= 0.0f || L.max.x - L.min.x < 1.0f) return;
  const Rows r = Measure(a, s, dl);
  const float rounding = std::min(p.rounding * s, (L.max.y - L.min.y) * 0.5f);
  const Color& tier_col = p.tier_colors ? st.TierColor(a.n.tier, c.points) : c.points;

  if (p.shadow && c.shadow.a > 0.0f) {
    dl.FillRect({L.min.x - 2.0f * s, L.min.y + 4.0f * s}, {L.max.x + 2.0f * s, L.max.y + 6.0f * s},
                Fade(c.shadow, alpha), rounding + 2.0f * s);
  }
  dl.FillRect(L.min, L.max, Fade(c.background, alpha), rounding);
  if (p.border_thickness > 0.0f && c.border.a > 0.0f)
    dl.StrokeRect(L.min, L.max, Fade(c.border, alpha), rounding, p.border_thickness * s);

  dl.PushClip(L.min, L.max);
  const float pad = p.padding * s;
  const float cy = (L.min.y + L.max.y) * 0.5f;

  // Icon: pinned to the card's current left edge so it leads an expanding card.
  if (r.icon > 0) {
    const Vec2 i0{L.min.x + pad, cy - r.icon * 0.5f};
    const Vec2 i1{i0.x + r.icon, i0.y + r.icon};
    const float ir = p.icon_shape == IconShape::kCircle ? r.icon * 0.5f : p.icon_rounding * s;
    if (L.icon) {
      dl.DrawImage(L.icon, i0, i1, Fade(Color{1, 1, 1, 1}, alpha), ir);
    } else {
      const Color& fill = a.n.tier != Tier::kNone && p.tier_colors ? tier_col : c.accent;
      const Vec2 ic{(i0.x + i1.x) * 0.5f, (i0.y + i1.y) * 0.5f};
      if (p.icon_shape == IconShape::kCircle) dl.FillCircle(ic, r.icon * 0.5f, Fade(fill, alpha), 32);
      else dl.FillRect(i0, i1, Fade(fill, alpha), ir);
      if (!p.badge.empty()) {
        const float bs = r.icon * 0.45f;
        const float ext_x = dl.TextWidth(true, bs, p.badge);
        dl.Text(true, bs, {ic.x - ext_x * 0.5f, ic.y - bs * 0.5f}, Fade(c.name, alpha), p.badge);
      } else {
        DrawTrophy(dl, ic, r.icon * 0.5f, Fade(c.name, alpha * 0.9f));
      }
    }
    if (c.icon_border.a > 0.0f) {
      if (p.icon_shape == IconShape::kCircle)
        dl.StrokeCircle({(i0.x + i1.x) * 0.5f, cy}, r.icon * 0.5f, Fade(c.icon_border, alpha), 32, 1.5f * s);
      else
        dl.StrokeRect(i0, i1, Fade(c.icon_border, alpha), ir, 1.5f * s);
    }
  }

  // Text lays out against the rest rectangle and is clipped by the current
  // one; while a card expands it rides the moving edge and fades in late.
  const bool expanding = p.animation == Animation::kExpand && a.expand < 1.0f;
  const float tx = (expanding ? L.min.x : L.rest_min.x) + r.text_x;
  const float text_alpha = expanding ? alpha * std::clamp((a.expand - 0.3f) / 0.7f, 0.0f, 1.0f) : alpha;
  const float tw = r.text_w;
  const Color points_col = Fade(tier_col, text_alpha);
  if (r.pill_w > 0) {
    const float ph = r.points_size + 9.0f * s;
    const float px1 = L.rest_max.x - pad - 4.0f * s;
    const Vec2 p0{px1 - r.pill_w, cy - ph * 0.5f}, p1{px1, cy + ph * 0.5f};
    Color fill = tier_col;
    fill.a *= 40.0f / 255.0f;
    Color edge = tier_col;
    edge.a *= 120.0f / 255.0f;
    dl.FillRect(p0, p1, Fade(fill, text_alpha), 6.0f * s);
    dl.StrokeRect(p0, p1, Fade(edge, text_alpha), 6.0f * s, 1.0f * s);
    const float ext_x = dl.TextWidth(true, r.points_size, r.text.points);
    dl.Text(true, r.points_size, {p0.x + (r.pill_w - ext_x) * 0.5f, cy - r.points_size * 0.5f}, points_col,
            r.text.points);
  }

  float y = cy - r.content_h * 0.5f;
  bool first = true;
  auto row = [&](bool bold, float size, const std::string& text, const Color& color) {
    if (!first) y += r.gap;
    float w = tw;
    if (first && r.inline_points_w > 0) {
      // Points share the first row, right-aligned.
      dl.Text(true, r.points_size, {tx + tw - r.inline_points_w, y + (size - r.points_size) * 0.5f}, points_col,
              r.text.points);
      w = tw - r.inline_points_w - 8.0f * s;
    }
    if (!text.empty()) {
      const std::string t = Ellipsize(dl, bold, size, text, std::max(1.0f, w));
      dl.Text(bold, size, {tx, y}, Fade(color, text_alpha), t);
    }
    y += size;
    first = false;
  };
  bool bar_done = !r.bar;
  auto bar = [&] {
    // Right under the first row: "Kickflip  3/14", bar, "Flip Trick Encyclopedia".
    if (bar_done) return;
    bar_done = true;
    if (!first) y += r.gap;
    const float by = y + 2.0f * s;
    const float frac = std::clamp(static_cast<float>(a.n.current) / static_cast<float>(a.n.total), 0.0f, 1.0f);
    dl.FillRect({tx, by}, {tx + tw, by + r.bar_h}, Fade(c.bar_track, text_alpha), r.bar_h * 0.5f);
    if (frac > 0.0f)
      dl.FillRect({tx, by}, {tx + tw * frac, by + r.bar_h}, Fade(c.bar_fill, text_alpha), r.bar_h * 0.5f);
    y += r.bar_h + 4.0f * s;
    first = false;
  };
  const bool any_head = !r.text.title.empty() || !r.text.name.empty();
  if (!r.text.title.empty()) {
    row(true, r.title_size, r.text.title, c.title);
    bar();
  }
  if (!r.text.name.empty()) {
    row(true, r.name_size, r.text.name, c.name);
    bar();
  }
  if (!any_head && r.inline_points_w > 0) row(true, r.points_size, std::string(), c.points);
  bar();
  for (const auto& line : r.description) {
    if (!first) y += r.gap;
    dl.Text(false, r.desc_size, {tx, y}, Fade(c.description, text_alpha), line);
    y += r.desc_size;
    first = false;
  }
  dl.PopClip();
}

}  // namespace recomp::achievements::ui
