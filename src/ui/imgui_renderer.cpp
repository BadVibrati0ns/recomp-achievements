#include "recomp/achievements/ui/imgui_renderer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace recomp::achievements::ui {

namespace {

ImU32 Col(const Color& c, float alpha) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a * std::clamp(alpha, 0.0f, 1.0f)));
}

float TextWidth(ImFont* font, float size, const std::string& s) {
  return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s.c_str(), s.c_str() + s.size()).x;
}

// Drops the last UTF-8 code point.
void PopCodePoint(std::string& s) {
  while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
  if (!s.empty()) s.pop_back();
}

std::string Ellipsize(ImFont* font, float size, std::string s, float max_w) {
  if (TextWidth(font, size, s) <= max_w) return s;
  while (!s.empty() && TextWidth(font, size, s + "...") > max_w) PopCodePoint(s);
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s + "...";
}

// Greedy word wrap into at most max_lines; the last line is ellipsised.
std::vector<std::string> Wrap(ImFont* font, float size, const std::string& text, float max_w, int max_lines) {
  std::vector<std::string> lines;
  if (text.empty() || max_lines <= 0) return lines;
  size_t pos = 0;
  while (pos < text.size() && static_cast<int>(lines.size()) < max_lines) {
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size()) break;
    const bool last = static_cast<int>(lines.size()) == max_lines - 1;
    if (last) {
      lines.push_back(Ellipsize(font, size, text.substr(pos), max_w));
      break;
    }
    size_t end = pos, best = std::string::npos;
    while (end <= text.size()) {
      const size_t sp = text.find(' ', end);
      const size_t cut = sp == std::string::npos ? text.size() : sp;
      if (TextWidth(font, size, text.substr(pos, cut - pos)) > max_w) break;
      best = cut;
      if (sp == std::string::npos) break;
      end = sp + 1;
    }
    if (best == std::string::npos) {
      // One word wider than the line: cut it by characters.
      std::string w = text.substr(pos);
      size_t n = w.size();
      while (n > 1 && TextWidth(font, size, w.substr(0, n)) > max_w) {
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

Rows Measure(const ActivePopup& a, float s, ImFont* reg, ImFont* bold) {
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
      r.pill_w = TextWidth(bold, r.points_size, r.text.points) + 20.0f * s;
      right -= r.pill_w + 10.0f * s;
    } else {
      r.inline_points_w = TextWidth(bold, r.points_size, r.text.points);
    }
  }
  r.text_w = std::max(40.0f * s, right - r.text_x);
  r.bar = p.lines.bar && a.n.kind == NotificationKind::kProgress && a.n.total > 0;
  r.bar_h = 6.0f * s;
  r.description = Wrap(reg, r.desc_size, r.text.description, r.text_w, p.description_lines);

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

float ScaleFor(const Style& st, ImVec2 size) {
  return std::clamp(size.y / std::max(1.0f, st.reference_height), st.min_scale, st.max_scale) * st.scale;
}

}  // namespace

// The trophy drawn when an achievement has no icon and no fallback icon.
void DrawTrophy(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  dl->AddRectFilled(ImVec2(c.x - 0.42f * r, c.y - 0.5f * r), ImVec2(c.x + 0.42f * r, c.y + 0.1f * r), col,
                    0.4f * r, ImDrawFlags_RoundCornersBottom);
  dl->AddCircle(ImVec2(c.x - 0.45f * r, c.y - 0.25f * r), 0.18f * r, col, 12, 0.08f * r);
  dl->AddCircle(ImVec2(c.x + 0.45f * r, c.y - 0.25f * r), 0.18f * r, col, 12, 0.08f * r);
  dl->AddRectFilled(ImVec2(c.x - 0.08f * r, c.y + 0.1f * r), ImVec2(c.x + 0.08f * r, c.y + 0.32f * r), col);
  dl->AddRectFilled(ImVec2(c.x - 0.3f * r, c.y + 0.32f * r), ImVec2(c.x + 0.3f * r, c.y + 0.46f * r), col,
                    0.05f * r);
}

ImGuiRenderer::ImGuiRenderer(NotificationCenter& center, IconCache* icons) : center_(center), icons_(icons) {}

void ImGuiRenderer::SetCustomDraw(DrawCallback callback) { custom_ = std::move(callback); }

void ImGuiRenderer::SetFonts(ImFont* regular, ImFont* bold) {
  font_ = regular;
  font_bold_ = bold;
}

void ImGuiRenderer::RegisterFont(std::string name, ImFont* font) { fonts_[std::move(name)] = font; }

ImFont* ImGuiRenderer::FontFor(const Style& style, bool bold) const {
  const std::string& name = bold ? style.font_bold : style.font;
  if (!name.empty()) {
    if (auto it = fonts_.find(name); it != fonts_.end() && it->second) return it->second;
  }
  if (bold && font_bold_) return font_bold_;
  if (font_) return font_;
  return ImGui::GetFont();
}

float ImGuiRenderer::MeasureHeight(const ActivePopup& popup, float s) const {
  return Measure(popup, s, FontFor(*popup.n.style, false), FontFor(*popup.n.style, true)).height;
}

PopupLayout ImGuiRenderer::Layout(const ActivePopup& a, ImVec2 origin, ImVec2 size) const {
  const float s = ScaleFor(*a.n.style, size);
  const float step = MeasureHeight(a, s) + a.n.popup().spacing * s;
  return Place(a, origin, size, step * a.stack_pos);
}

std::vector<PopupLayout> ImGuiRenderer::LayoutAll(const std::vector<ActivePopup>& popups, ImVec2 origin,
                                                  ImVec2 size) const {
  // Popups at the same anchor and offset share one stack, so an unlock and a
  // progress popup never cover each other. Active() lists unlocks first.
  struct Key {
    Anchor anchor;
    float x, y;
  };
  std::vector<Key> keys;
  std::vector<float> stack_px;  // Running offset per key.
  std::vector<PopupLayout> out;
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
    const float s = ScaleFor(*a.n.style, size);
    const float step = MeasureHeight(a, s) + p.spacing * s;
    // stack_pos lags slot while gliding into a freed space.
    out.push_back(Place(a, origin, size, stack_px[k] + (a.stack_pos - static_cast<float>(a.slot)) * step));
    stack_px[k] += step;
  }
  return out;
}

PopupLayout ImGuiRenderer::Place(const ActivePopup& a, ImVec2 origin, ImVec2 size, float stack_px) const {
  const Style& st = *a.n.style;
  const PopupStyle& p = a.n.popup();
  PopupLayout L;
  L.scale = ScaleFor(st, size);
  L.alpha = a.alpha;
  L.font = FontFor(st, false);
  L.font_bold = FontFor(st, true);
  const float s = L.scale;
  const float w = p.width * s;
  const float h = Measure(a, s, L.font, L.font_bold).height;
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
  L.rest_min = ImVec2(x, y);
  L.rest_max = ImVec2(x + w, y + h);

  // Slides travel toward the nearest edge.
  const float d = p.slide_distance * s * a.slide;
  float dx = 0, dy = 0;
  if (row == 0) dy = -d;
  else if (row == 2) dy = d;
  else if (col == 0) dx = -d;
  else if (col == 2) dx = d;
  else dy = d;
  L.min = ImVec2(x + dx, y + dy);
  L.max = ImVec2(x + w + dx, y + h + dy);

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
    const ImVec2 c((L.min.x + L.max.x) * 0.5f, (L.min.y + L.max.y) * 0.5f);
    L.min = ImVec2(c.x - w * 0.5f * k, c.y - h * 0.5f * k);
    L.max = ImVec2(c.x + w * 0.5f * k, c.y + h * 0.5f * k);
    L.rest_min = L.min;
    L.rest_max = L.max;
    L.scale *= k;
  }
  return L;
}

void ImGuiRenderer::DrawDefault(ImDrawList* dl, const ActivePopup& a, const PopupLayout& L) const {
  const Style& st = *a.n.style;
  const PopupStyle& p = a.n.popup();
  const PopupColors& c = p.colors;
  const float s = L.scale;
  const float alpha = L.alpha;
  if (alpha <= 0.0f || L.max.x - L.min.x < 1.0f) return;
  ImFont* reg = L.font ? L.font : ImGui::GetFont();
  ImFont* bold = L.font_bold ? L.font_bold : reg;
  const Rows r = Measure(a, s, reg, bold);
  const float rounding = std::min(p.rounding * s, (L.max.y - L.min.y) * 0.5f);
  const Color& tier_col = p.tier_colors ? st.TierColor(a.n.tier, c.points) : c.points;

  if (p.shadow && c.shadow.a > 0.0f) {
    dl->AddRectFilled(ImVec2(L.min.x - 2.0f * s, L.min.y + 4.0f * s), ImVec2(L.max.x + 2.0f * s, L.max.y + 6.0f * s),
                      Col(c.shadow, alpha), rounding + 2.0f * s);
  }
  dl->AddRectFilled(L.min, L.max, Col(c.background, alpha), rounding);
  if (p.border_thickness > 0.0f && c.border.a > 0.0f)
    dl->AddRect(L.min, L.max, Col(c.border, alpha), rounding, 0, p.border_thickness * s);

  dl->PushClipRect(L.min, L.max, true);
  const float pad = p.padding * s;
  const float cy = (L.min.y + L.max.y) * 0.5f;

  // Icon: pinned to the card's current left edge so it leads an expanding card.
  if (r.icon > 0) {
    const ImVec2 i0(L.min.x + pad, cy - r.icon * 0.5f);
    const ImVec2 i1(i0.x + r.icon, i0.y + r.icon);
    const float ir = p.icon_shape == IconShape::kCircle ? r.icon * 0.5f : p.icon_rounding * s;
    if (L.icon) {
      dl->AddImageRounded(ToImTextureID(L.icon), i0, i1, ImVec2(0, 0), ImVec2(1, 1),
                          Col(Color{1, 1, 1, 1}, alpha), ir);
    } else {
      const Color& fill = a.n.tier != Tier::kNone && p.tier_colors ? tier_col : c.accent;
      const ImVec2 ic((i0.x + i1.x) * 0.5f, (i0.y + i1.y) * 0.5f);
      if (p.icon_shape == IconShape::kCircle) dl->AddCircleFilled(ic, r.icon * 0.5f, Col(fill, alpha), 32);
      else dl->AddRectFilled(i0, i1, Col(fill, alpha), ir);
      if (!p.badge.empty()) {
        const float bs = r.icon * 0.45f;
        const ImVec2 ext = bold->CalcTextSizeA(bs, FLT_MAX, 0.0f, p.badge.c_str());
        dl->AddText(bold, bs, ImVec2(ic.x - ext.x * 0.5f, ic.y - ext.y * 0.5f), Col(c.name, alpha),
                    p.badge.c_str());
      } else {
        DrawTrophy(dl, ic, r.icon * 0.5f, Col(c.name, alpha * 0.9f));
      }
    }
    if (c.icon_border.a > 0.0f) {
      if (p.icon_shape == IconShape::kCircle)
        dl->AddCircle(ImVec2((i0.x + i1.x) * 0.5f, cy), r.icon * 0.5f, Col(c.icon_border, alpha), 32, 1.5f * s);
      else
        dl->AddRect(i0, i1, Col(c.icon_border, alpha), ir, 0, 1.5f * s);
    }
  }

  // Text lays out against the rest rectangle and is clipped by the current
  // one; while a card expands it rides the moving edge and fades in late.
  const bool expanding = p.animation == Animation::kExpand && a.expand < 1.0f;
  const float tx = (expanding ? L.min.x : L.rest_min.x) + r.text_x;
  const float text_alpha = expanding ? alpha * std::clamp((a.expand - 0.3f) / 0.7f, 0.0f, 1.0f) : alpha;
  const float tw = r.text_w;
  const ImU32 points_col = Col(tier_col, text_alpha);
  if (r.pill_w > 0) {
    const float ph = r.points_size + 9.0f * s;
    const float px1 = L.rest_max.x - pad - 4.0f * s;
    const ImVec2 p0(px1 - r.pill_w, cy - ph * 0.5f), p1(px1, cy + ph * 0.5f);
    Color fill = tier_col;
    fill.a *= 40.0f / 255.0f;
    Color edge = tier_col;
    edge.a *= 120.0f / 255.0f;
    dl->AddRectFilled(p0, p1, Col(fill, text_alpha), 6.0f * s);
    dl->AddRect(p0, p1, Col(edge, text_alpha), 6.0f * s, 0, 1.0f * s);
    const ImVec2 ext = bold->CalcTextSizeA(r.points_size, FLT_MAX, 0.0f, r.text.points.c_str());
    dl->AddText(bold, r.points_size, ImVec2(p0.x + (r.pill_w - ext.x) * 0.5f, cy - ext.y * 0.5f), points_col,
                r.text.points.c_str());
  }

  float y = cy - r.content_h * 0.5f;
  bool first = true;
  auto row = [&](ImFont* font, float size, const std::string& text, const Color& color) {
    if (!first) y += r.gap;
    float w = tw;
    if (first && r.inline_points_w > 0) {
      // Points share the first row, right-aligned.
      dl->AddText(bold, r.points_size, ImVec2(tx + tw - r.inline_points_w, y + (size - r.points_size) * 0.5f),
                  points_col, r.text.points.c_str());
      w = tw - r.inline_points_w - 8.0f * s;
    }
    if (!text.empty()) {
      const std::string t = Ellipsize(font, size, text, std::max(1.0f, w));
      dl->AddText(font, size, ImVec2(tx, y), Col(color, text_alpha), t.c_str());
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
    dl->AddRectFilled(ImVec2(tx, by), ImVec2(tx + tw, by + r.bar_h), Col(c.bar_track, text_alpha), r.bar_h * 0.5f);
    if (frac > 0.0f)
      dl->AddRectFilled(ImVec2(tx, by), ImVec2(tx + tw * frac, by + r.bar_h), Col(c.bar_fill, text_alpha),
                        r.bar_h * 0.5f);
    y += r.bar_h + 4.0f * s;
    first = false;
  };
  const bool any_head = !r.text.title.empty() || !r.text.name.empty();
  if (!r.text.title.empty()) {
    row(bold, r.title_size, r.text.title, c.title);
    bar();
  }
  if (!r.text.name.empty()) {
    row(bold, r.name_size, r.text.name, c.name);
    bar();
  }
  if (!any_head && r.inline_points_w > 0) row(bold, r.points_size, std::string(), c.points);
  bar();
  for (const auto& line : r.description) {
    if (!first) y += r.gap;
    dl->AddText(reg, r.desc_size, ImVec2(tx, y), Col(c.description, text_alpha), line.c_str());
    y += r.desc_size;
    first = false;
  }
  dl->PopClipRect();
}

void ImGuiRenderer::Draw(bool advance) {
  const ImGuiIO& io = ImGui::GetIO();
  if (advance) center_.Update(io.DeltaTime);
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  Draw(ImGui::GetForegroundDrawList(), vp->Pos, vp->Size, 0.0f);
}

void ImGuiRenderer::Draw(ImDrawList* dl, ImVec2 origin, ImVec2 size, float dt) {
  if (dt > 0.0f) center_.Update(dt);
  if (!dl) return;
  const auto active = center_.Active();
  const auto layouts = LayoutAll(active, origin, size);
  for (size_t i = 0; i < active.size(); ++i) {
    const ActivePopup& a = active[i];
    PopupLayout L = layouts[i];
    if (icons_ && a.n.popup().lines.icon)
      L.icon = icons_->GetOrFallback(a.n.icon, a.n.set_id, a.n.icon_base_dir);
    if (custom_ && custom_(dl, a, L)) continue;
    DrawDefault(dl, a, L);
  }
}

}  // namespace recomp::achievements::ui
