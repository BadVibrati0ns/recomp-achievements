#include "recomp/achievements/ui/imgui_renderer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace recomp::achievements::ui {

namespace {

ImU32 Col(const Color& c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.r, c.g, c.b, c.a)); }
ImVec2 V(Vec2 v) { return ImVec2(v.x, v.y); }
Vec2 V(ImVec2 v) { return {v.x, v.y}; }

ImDrawFlags CornerFlags(int corners) {
  if (corners == kCornersAll) return 0;
  ImDrawFlags f = 0;
  if (corners & kCornerTopLeft) f |= ImDrawFlags_RoundCornersTopLeft;
  if (corners & kCornerTopRight) f |= ImDrawFlags_RoundCornersTopRight;
  if (corners & kCornerBottomLeft) f |= ImDrawFlags_RoundCornersBottomLeft;
  if (corners & kCornerBottomRight) f |= ImDrawFlags_RoundCornersBottomRight;
  return f ? f : ImDrawFlags_RoundCornersNone;
}

PopupRect ToRect(const PopupLayout& L) {
  PopupRect r;
  r.min = V(L.min);
  r.max = V(L.max);
  r.rest_min = V(L.rest_min);
  r.rest_max = V(L.rest_max);
  r.scale = L.scale;
  r.alpha = L.alpha;
  r.icon = L.icon;
  return r;
}

// Text widths with the fonts a style picks.
class ImFontMeasurer : public TextMeasurer {
 public:
  ImFontMeasurer(ImFont* regular, ImFont* bold) : regular_(regular), bold_(bold) {}
  float TextWidth(bool bold, float size, std::string_view s) const override {
    ImFont* f = bold ? bold_ : regular_;
    return f->CalcTextSizeA(size, FLT_MAX, 0.0f, s.data(), s.data() + s.size()).x;
  }

 private:
  ImFont* regular_;
  ImFont* bold_;
};

}  // namespace

ImGuiPainter::ImGuiPainter(ImDrawList* draw_list, ImFont* regular, ImFont* bold)
    : dl_(draw_list), regular_(regular ? regular : ImGui::GetFont()), bold_(bold) {}

float ImGuiPainter::TextWidth(bool bold, float size, std::string_view s) const {
  return Font(bold)->CalcTextSizeA(size, FLT_MAX, 0.0f, s.data(), s.data() + s.size()).x;
}

void ImGuiPainter::FillRect(Vec2 min, Vec2 max, const Color& c, float rounding, int corners) {
  dl_->AddRectFilled(V(min), V(max), Col(c), rounding, CornerFlags(corners));
}

void ImGuiPainter::StrokeRect(Vec2 min, Vec2 max, const Color& c, float rounding, float thickness) {
  dl_->AddRect(V(min), V(max), Col(c), rounding, 0, thickness);
}

void ImGuiPainter::FillCircle(Vec2 center, float radius, const Color& c, int segments) {
  dl_->AddCircleFilled(V(center), radius, Col(c), segments);
}

void ImGuiPainter::StrokeCircle(Vec2 center, float radius, const Color& c, int segments, float thickness) {
  dl_->AddCircle(V(center), radius, Col(c), segments, thickness);
}

void ImGuiPainter::DrawImage(void* texture, Vec2 min, Vec2 max, const Color& tint, float rounding) {
  dl_->AddImageRounded(ToImTextureID(texture), V(min), V(max), ImVec2(0, 0), ImVec2(1, 1), Col(tint), rounding);
}

void ImGuiPainter::Text(bool bold, float size, Vec2 pos, const Color& c, std::string_view text) {
  dl_->AddText(Font(bold), size, V(pos), Col(c), text.data(), text.data() + text.size());
}

void ImGuiPainter::PushClip(Vec2 min, Vec2 max) { dl_->PushClipRect(V(min), V(max), true); }
void ImGuiPainter::PopClip() { dl_->PopClipRect(); }

void DrawTrophy(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
  ImGuiPainter painter(dl, nullptr);
  const ImVec4 f = ImGui::ColorConvertU32ToFloat4(col);
  DrawTrophy(painter, V(c), r, Color{f.x, f.y, f.z, f.w});
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
  return MeasurePopupHeight(popup, s, ImFontMeasurer(FontFor(*popup.n.style, false), FontFor(*popup.n.style, true)));
}

PopupLayout ImGuiRenderer::Layout(const ActivePopup& a, ImVec2 origin, ImVec2 size) const {
  const float s = PopupScale(*a.n.style, V(size));
  const float step = MeasureHeight(a, s) + a.n.popup().spacing * s;
  return Place(a, origin, size, step * a.stack_pos);
}

std::vector<PopupLayout> ImGuiRenderer::LayoutAll(const std::vector<ActivePopup>& popups, ImVec2 origin,
                                                  ImVec2 size) const {
  // Same stacking as LayoutPopups(), with each popup measured in its own fonts.
  struct Key {
    Anchor anchor;
    float x, y;
  };
  std::vector<Key> keys;
  std::vector<float> stack_px;
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
    const float s = PopupScale(*a.n.style, V(size));
    const float step = MeasureHeight(a, s) + p.spacing * s;
    out.push_back(Place(a, origin, size, stack_px[k] + (a.stack_pos - static_cast<float>(a.slot)) * step));
    stack_px[k] += step;
  }
  return out;
}

PopupLayout ImGuiRenderer::Place(const ActivePopup& a, ImVec2 origin, ImVec2 size, float stack_px) const {
  PopupLayout L;
  L.font = FontFor(*a.n.style, false);
  L.font_bold = FontFor(*a.n.style, true);
  const PopupRect r = PlacePopup(a, V(origin), V(size), stack_px, ImFontMeasurer(L.font, L.font_bold));
  L.min = V(r.min);
  L.max = V(r.max);
  L.rest_min = V(r.rest_min);
  L.rest_max = V(r.rest_max);
  L.scale = r.scale;
  L.alpha = r.alpha;
  return L;
}

void ImGuiRenderer::DrawDefault(ImDrawList* dl, const ActivePopup& a, const PopupLayout& L) const {
  ImFont* reg = L.font ? L.font : ImGui::GetFont();
  ImGuiPainter painter(dl, reg, L.font_bold ? L.font_bold : reg);
  DrawPopup(painter, a, ToRect(L));
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
