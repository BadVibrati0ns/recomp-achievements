#include "recomp/achievements/ui/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#define STB_TRUETYPE_IMPLEMENTATION
// Static, so a game's own stb_truetype (or ImGui's) never clashes with ours.
#define STBTT_STATIC
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include <stb/stb_truetype.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#if !RECOMP_ACH_NO_EMBEDDED_FONTS
namespace recomp::achievements::ui::fonts {
// Generated at build time from assets/fonts (cmake/embed_file.cmake).
extern const unsigned char kDejaVuSans[];
extern const size_t kDejaVuSansSize;
extern const unsigned char kDejaVuSansBold[];
extern const size_t kDejaVuSansBoldSize;
}  // namespace recomp::achievements::ui::fonts
#endif

namespace recomp::achievements::ui {

namespace {

// Next code point of s at i; invalid bytes come back as U+FFFD.
uint32_t NextCodePoint(std::string_view s, size_t& i) {
  const auto b0 = static_cast<unsigned char>(s[i++]);
  if (b0 < 0x80) return b0;
  int extra = 0;
  uint32_t cp = 0;
  if ((b0 & 0xE0) == 0xC0) extra = 1, cp = b0 & 0x1F;
  else if ((b0 & 0xF0) == 0xE0) extra = 2, cp = b0 & 0x0F;
  else if ((b0 & 0xF8) == 0xF0) extra = 3, cp = b0 & 0x07;
  else return 0xFFFD;
  for (int k = 0; k < extra; ++k) {
    if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
  }
  return cp;
}

uint32_t Load(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
void Store(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// Each 8-bit channel of s times k/255, rounded.
uint32_t ScalePremul(uint32_t s, int k) {
  if (k >= 255) return s;
  if (k <= 0) return 0;
  uint32_t rb = (s & 0x00FF00FFu) * static_cast<uint32_t>(k) + 0x00800080u;
  rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
  uint32_t ag = ((s >> 8) & 0x00FF00FFu) * static_cast<uint32_t>(k) + 0x00800080u;
  ag = ((ag + ((ag >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
  return rb | (ag << 8);
}

int CoverageByte(float c) {
  if (c <= 0.0f) return 0;
  if (c >= 1.0f) return 255;
  return static_cast<int>(c * 255.0f + 0.5f);
}

float Clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

// Signed distance to a rounded rect (centre c, half size h) with per-quadrant radii.
float RoundedRectDistance(float px, float py, float cx, float cy, float hw, float hh, const float r[4]) {
  const float dx = px - cx, dy = py - cy;
  const float rad = dy < 0 ? (dx < 0 ? r[0] : r[1]) : (dx < 0 ? r[2] : r[3]);
  const float qx = std::fabs(dx) - (hw - rad), qy = std::fabs(dy) - (hh - rad);
  const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
  const float outside = (ox > 0.0f || oy > 0.0f) ? std::sqrt(ox * ox + oy * oy) : 0.0f;
  return outside + std::min(std::max(qx, qy), 0.0f) - rad;
}

void Radii(RectF r, float radius, int corners, float out[4]) {
  const float rad = std::clamp(radius, 0.0f, std::max(0.0f, std::min(r.width(), r.height()) * 0.5f));
  out[0] = corners & kCornerTopLeft ? rad : 0.0f;
  out[1] = corners & kCornerTopRight ? rad : 0.0f;
  out[2] = corners & kCornerBottomLeft ? rad : 0.0f;
  out[3] = corners & kCornerBottomRight ? rad : 0.0f;
}

// Straight alpha from premultiplied, via a reciprocal table.
struct Unpremul {
  uint32_t recip[256];
  Unpremul() {
    recip[0] = 0;
    for (int a = 1; a < 256; ++a) recip[a] = static_cast<uint32_t>((255u << 16) / a);
  }
};

}  // namespace

PixelRect Union(const PixelRect& a, const PixelRect& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

PixelRect Intersect(const PixelRect& a, const PixelRect& b) {
  PixelRect r{std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
  if (r.empty()) return {};
  return r;
}

// ---- Font ---------------------------------------------------------------------

struct Font::Impl {
  struct Glyph {
    int index = 0;
    float advance = 0;  // Font units.
  };
  struct Bitmap {
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<uint8_t> px;
  };

  std::vector<uint8_t> data;
  stbtt_fontinfo info{};
  float units_ascent = 0, units_height = 1;
  std::shared_ptr<Font> fallback;
  bool synthetic_bold = false;
  // The built-in fonts are shared by every overlay, so the caches are locked.
  mutable std::mutex mutex;
  mutable std::unordered_map<uint32_t, Glyph> glyphs;
  mutable std::unordered_map<uint64_t, Bitmap> bitmaps;

  const Glyph& Find(uint32_t cp) const {
    auto it = glyphs.find(cp);
    if (it != glyphs.end()) return it->second;
    Glyph g;
    g.index = stbtt_FindGlyphIndex(&info, static_cast<int>(cp));
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, g.index, &adv, &lsb);
    g.advance = static_cast<float>(adv);
    return glyphs.emplace(cp, g).first->second;
  }
  float Scale(float size) const { return size / units_height; }
  float Bolden(float size) const { return synthetic_bold ? std::max(1.0f, std::round(size / 18.0f)) : 0.0f; }

  // Size in quarter pixels, subpixel x shift in quarters.
  const Bitmap& Raster(int glyph, int size_q, int sub) const {
    const uint64_t key = (static_cast<uint64_t>(size_q) << 34) | (static_cast<uint64_t>(sub) << 32) |
                         static_cast<uint32_t>(glyph);
    auto it = bitmaps.find(key);
    if (it != bitmaps.end()) return it->second;
    if (bitmaps.size() > 8192) bitmaps.clear();  // Bounded; animations can sweep many sizes.
    const float scale = Scale(size_q * 0.25f);
    const float shift = sub * 0.25f;
    Bitmap b;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&info, glyph, scale, scale, shift, 0.0f, &x0, &y0, &x1, &y1);
    b.x0 = x0;
    b.y0 = y0;
    b.w = std::max(0, x1 - x0);
    b.h = std::max(0, y1 - y0);
    if (b.w > 0 && b.h > 0) {
      b.px.assign(static_cast<size_t>(b.w) * b.h, 0);
      stbtt_MakeGlyphBitmapSubpixel(&info, b.px.data(), b.w, b.h, b.w, scale, scale, shift, 0.0f, glyph);
      const int grow = static_cast<int>(Bolden(size_q * 0.25f));
      if (grow > 0) {
        // Smear right by `grow` pixels: a cheap, readable faux bold.
        Bitmap wide = b;
        wide.w = b.w + grow;
        wide.px.assign(static_cast<size_t>(wide.w) * b.h, 0);
        for (int y = 0; y < b.h; ++y)
          for (int x = 0; x < wide.w; ++x) {
            int v = 0;
            for (int k = 0; k <= grow; ++k) {
              const int sx = x - k;
              if (sx >= 0 && sx < b.w) v = std::max<int>(v, b.px[y * b.w + sx]);
            }
            wide.px[y * wide.w + x] = static_cast<uint8_t>(v);
          }
        b = std::move(wide);
      }
    }
    return bitmaps.emplace(key, std::move(b)).first->second;
  }
};

Font::Font() : impl_(std::make_unique<Impl>()) {}
Font::~Font() = default;

std::shared_ptr<Font> Font::FromTtf(std::span<const uint8_t> ttf, int index) {
  if (ttf.empty()) return nullptr;
  std::shared_ptr<Font> f(new Font());
  Impl& m = *f->impl_;
  m.data.assign(ttf.begin(), ttf.end());
  const int offset = stbtt_GetFontOffsetForIndex(m.data.data(), index);
  if (offset < 0 || !stbtt_InitFont(&m.info, m.data.data(), offset)) return nullptr;
  int ascent = 0, descent = 0, gap = 0;
  stbtt_GetFontVMetrics(&m.info, &ascent, &descent, &gap);
  m.units_ascent = static_cast<float>(ascent);
  m.units_height = static_cast<float>(std::max(1, ascent - descent));
  return f;
}

std::shared_ptr<Font> Font::Builtin(bool bold) {
#if RECOMP_ACH_NO_EMBEDDED_FONTS
  (void)bold;
  return nullptr;
#else
  static const std::shared_ptr<Font> regular =
      FromTtf(std::span<const uint8_t>(fonts::kDejaVuSans, fonts::kDejaVuSansSize));
  static const std::shared_ptr<Font> heavy =
      FromTtf(std::span<const uint8_t>(fonts::kDejaVuSansBold, fonts::kDejaVuSansBoldSize));
  return bold ? heavy : regular;
#endif
}

float Font::Ascent(float size) const { return impl_->units_ascent * impl_->Scale(size); }

float Font::TextWidth(float size, std::string_view s) const {
  std::lock_guard lock(impl_->mutex);
  float units = 0, fallback_px = 0;
  int glyphs = 0;
  for (size_t i = 0; i < s.size();) {
    const uint32_t cp = NextCodePoint(s, i);
    const auto& g = impl_->Find(cp);
    if (g.index == 0 && impl_->fallback && impl_->fallback->HasGlyph(cp)) {
      char buf[4];
      size_t n = 0;
      // Re-encode the one code point for the fallback font.
      if (cp < 0x80) buf[n++] = static_cast<char>(cp);
      else if (cp < 0x800) buf[n++] = static_cast<char>(0xC0 | (cp >> 6)), buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
      else if (cp < 0x10000)
        buf[n++] = static_cast<char>(0xE0 | (cp >> 12)), buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
        buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
      else
        buf[n++] = static_cast<char>(0xF0 | (cp >> 18)), buf[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
        buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
      fallback_px += impl_->fallback->TextWidth(size, std::string_view(buf, n));
      continue;
    }
    units += g.advance;
    ++glyphs;
  }
  return units * impl_->Scale(size) + fallback_px + glyphs * impl_->Bolden(size);
}

bool Font::HasGlyph(uint32_t cp) const {
  std::lock_guard lock(impl_->mutex);
  return impl_->Find(cp).index != 0;
}
void Font::SetFallback(std::shared_ptr<Font> fallback) {
  impl_->fallback = fallback.get() == this ? nullptr : std::move(fallback);
}
void Font::SetSyntheticBold(bool on) {
  std::lock_guard lock(impl_->mutex);
  impl_->synthetic_bold = on;
  impl_->bitmaps.clear();
}
size_t Font::cached_glyphs() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->bitmaps.size();
}

// ---- Images ---------------------------------------------------------------------

CanvasImage CanvasImage::From(Image image) {
  CanvasImage out;
  out.image = std::move(image);
  const Image* src = &out.image;
  while (src->width > 1 || src->height > 1) {
    Image half;
    half.width = std::max(1, src->width / 2);
    half.height = std::max(1, src->height / 2);
    half.rgba.resize(static_cast<size_t>(half.width) * half.height * 4);
    for (int y = 0; y < half.height; ++y) {
      for (int x = 0; x < half.width; ++x) {
        // Alpha-weighted 2x2 average so transparent texels don't darken edges.
        int sum[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
          const int sx = std::min(src->width - 1, x * 2 + (k & 1));
          const int sy = std::min(src->height - 1, y * 2 + (k >> 1));
          const uint8_t* p = &src->rgba[(static_cast<size_t>(sy) * src->width + sx) * 4];
          sum[0] += p[0] * p[3];
          sum[1] += p[1] * p[3];
          sum[2] += p[2] * p[3];
          sum[3] += p[3];
        }
        uint8_t* d = &half.rgba[(static_cast<size_t>(y) * half.width + x) * 4];
        d[3] = static_cast<uint8_t>((sum[3] + 2) / 4);
        for (int c = 0; c < 3; ++c) d[c] = sum[3] ? static_cast<uint8_t>((sum[c] + sum[3] / 2) / sum[3]) : 0;
      }
    }
    out.mips.push_back(std::move(half));
    src = &out.mips.back();
  }
  return out;
}

void UseImageTextures(IconCache& cache) {
  cache.SetTextureCallbacks(
      [](int w, int h, const uint8_t* rgba) -> void* {
        if (w <= 0 || h <= 0 || !rgba) return nullptr;
        Image img;
        img.width = w;
        img.height = h;
        img.rgba.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
        return new CanvasImage(CanvasImage::From(std::move(img)));
      },
      [](void* texture) { delete static_cast<CanvasImage*>(texture); });
}

// ---- Canvas -------------------------------------------------------------------

void Canvas::Resize(int width, int height) {
  width_ = std::max(0, width);
  height_ = std::max(0, height);
  const size_t bytes = static_cast<size_t>(width_) * height_ * 4;
  premul_.width = straight_.width = width_;
  premul_.height = straight_.height = height_;
  premul_.rgba.assign(bytes, 0);
  straight_.rgba.assign(bytes, 0);
  stale_ = {};
  dirty_ = {0, 0, width_, height_};
  clips_.clear();
}

const Image& Canvas::image() const {
  if (stale_.empty()) return straight_;
  static const Unpremul lut;
  for (int y = stale_.y0; y < stale_.y1; ++y) {
    const size_t row = (static_cast<size_t>(y) * width_ + stale_.x0) * 4;
    const uint8_t* s = premul_.rgba.data() + row;
    uint8_t* d = straight_.rgba.data() + row;
    // UI pixels come in long runs of one colour; convert each run once.
    uint32_t last_in = 0, last_out = 0;
    for (int x = stale_.x0; x < stale_.x1; ++x, s += 4, d += 4) {
      const uint32_t v = Load(s);
      const uint32_t a = v >> 24;
      if (a == 255 || a == 0) {
        Store(d, v);
        continue;
      }
      if (v != last_in) {
        const uint32_t k = lut.recip[a];
        auto c = [&](int shift) { return std::min<uint32_t>(255, (((v >> shift) & 0xFF) * k + 32768) >> 16) << shift; };
        last_in = v;
        last_out = c(0) | c(8) | c(16) | (a << 24);
      }
      Store(d, last_out);
    }
  }
  stale_ = {};
  return straight_;
}

void Canvas::Touch(PixelRect r) {
  if (r.empty()) return;
  dirty_ = Union(dirty_, r);
  stale_ = Union(stale_, r);
}

void Canvas::Clear() { Clear({0, 0, width_, height_}); }

void Canvas::Clear(PixelRect r) {
  r = Intersect(r, {0, 0, width_, height_});
  if (r.empty()) return;
  for (int y = r.y0; y < r.y1; ++y)
    std::memset(premul_.rgba.data() + (static_cast<size_t>(y) * width_ + r.x0) * 4, 0, static_cast<size_t>(r.width()) * 4);
  Touch(r);
}

void Canvas::Fill(PixelRect r, const Color& c) {
  r = Intersect(r, clip());
  if (r.empty()) return;
  const uint32_t v = ToPremul(c, 1.0f).px;
  for (int y = r.y0; y < r.y1; ++y) {
    uint8_t* row = premul_.rgba.data() + (static_cast<size_t>(y) * width_ + r.x0) * 4;
    for (int x = 0; x < r.width(); ++x) Store(row + x * 4, v);
  }
  Touch(r);
}

PixelRect Canvas::clip() const { return clips_.empty() ? PixelRect{0, 0, width_, height_} : clips_.back(); }
void Canvas::PushClip(PixelRect r) { clips_.push_back(Intersect(r, clip())); }
void Canvas::PushClip(RectF r) {
  PushClip(PixelRect{static_cast<int>(std::lround(r.x0)), static_cast<int>(std::lround(r.y0)),
                     static_cast<int>(std::lround(r.x1)), static_cast<int>(std::lround(r.y1))});
}
void Canvas::PopClip() {
  if (!clips_.empty()) clips_.pop_back();
}

PixelRect Canvas::Bounds(float x0, float y0, float x1, float y1) const {
  PixelRect r{static_cast<int>(std::floor(x0)), static_cast<int>(std::floor(y0)), static_cast<int>(std::ceil(x1)),
              static_cast<int>(std::ceil(y1))};
  return Intersect(r, clip());
}

Canvas::Premul Canvas::ToPremul(const Color& c, float coverage) {
  const int a = CoverageByte(Clamp01(c.a) * coverage);
  auto ch = [&](float v) { return static_cast<uint32_t>(Clamp01(v) * a + 0.5f); };
  return {ch(c.r) | (ch(c.g) << 8) | (ch(c.b) << 16) | (static_cast<uint32_t>(a) << 24), a};
}

void Canvas::BlendPremul(uint32_t* d, uint32_t s) const {
  const uint32_t sa = s >> 24;
  if (sa == 0) return;
  if (sa == 255) {
    *d = s;
    return;
  }
  *d = s + ScalePremul(*d, static_cast<int>(255 - sa));
}

void Canvas::Span(int y, int x0, int x1, const Color& c, float coverage) {
  if (x1 <= x0) return;
  const Premul s = ToPremul(c, coverage);
  if (s.a == 0) return;
  uint8_t* row = premul_.rgba.data() + (static_cast<size_t>(y) * width_) * 4;
  for (int x = x0; x < x1; ++x) {
    uint32_t v = Load(row + x * 4);
    BlendPremul(&v, s.px);
    Store(row + x * 4, v);
  }
}

template <typename Coverage>
void Canvas::FillShape(PixelRect box, const Color& c, Coverage&& coverage) {
  if (box.empty()) return;
  const Premul full = ToPremul(c, 1.0f);
  if (full.a == 0) return;
  for (int y = box.y0; y < box.y1; ++y) {
    uint8_t* row = premul_.rgba.data() + (static_cast<size_t>(y) * width_) * 4;
    const float py = y + 0.5f;
    for (int x = box.x0; x < box.x1; ++x) {
      const int k = CoverageByte(coverage(x + 0.5f, py));
      if (!k) continue;
      uint32_t v = Load(row + x * 4);
      BlendPremul(&v, ScalePremul(full.px, k));
      Store(row + x * 4, v);
    }
  }
  Touch(box);
}

void Canvas::FillRect(RectF r, const Color& c, float radius, int corners) {
  if (r.width() <= 0 || r.height() <= 0 || c.a <= 0) return;
  const PixelRect box = Bounds(r.x0, r.y0, r.x1, r.y1);
  if (box.empty()) return;
  float rad[4];
  Radii(r, radius, corners, rad);
  const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f;
  const float hw = r.width() * 0.5f, hh = r.height() * 0.5f;
  const Premul full = ToPremul(c, 1.0f);
  if (full.a == 0) return;
  for (int y = box.y0; y < box.y1; ++y) {
    const float py = y + 0.5f;
    // Away from the left/right ends only the top/bottom edges matter, so the
    // middle of each row is one constant-coverage span.
    const float row_cov = Clamp01(0.5f - (std::fabs(py - cy) - hh));
    if (row_cov <= 0.0f) continue;
    const float rl = py < cy ? rad[0] : rad[2], rr = py < cy ? rad[1] : rad[3];
    const int mid0 = std::clamp(static_cast<int>(std::ceil(r.x0 + rl + 1.0f)), box.x0, box.x1);
    const int mid1 = std::clamp(static_cast<int>(std::floor(r.x1 - rr - 1.0f)), mid0, box.x1);
    uint8_t* row = premul_.rgba.data() + (static_cast<size_t>(y) * width_) * 4;
    auto edge = [&](int x) {
      const int k = CoverageByte(Clamp01(0.5f - RoundedRectDistance(x + 0.5f, py, cx, cy, hw, hh, rad)));
      if (!k) return;
      uint32_t v = Load(row + x * 4);
      BlendPremul(&v, ScalePremul(full.px, k));
      Store(row + x * 4, v);
    };
    for (int x = box.x0; x < mid0; ++x) edge(x);
    const uint32_t s = ScalePremul(full.px, CoverageByte(row_cov));
    if ((s >> 24) == 255) {
      for (int x = mid0; x < mid1; ++x) Store(row + x * 4, s);
    } else {
      // Blending one colour over runs of one colour: reuse the last result.
      uint32_t last_in = 0, last_out = s;
      for (int x = mid0; x < mid1; ++x) {
        const uint32_t d = Load(row + x * 4);
        if (d != last_in) {
          last_in = d;
          last_out = d;
          BlendPremul(&last_out, s);
        }
        Store(row + x * 4, last_out);
      }
    }
    for (int x = mid1; x < box.x1; ++x) edge(x);
  }
  Touch(box);
}

void Canvas::StrokeRect(RectF r, const Color& c, float radius, float t) {
  if (r.width() <= 0 || r.height() <= 0 || t <= 0 || c.a <= 0) return;
  const PixelRect box = Bounds(r.x0, r.y0, r.x1, r.y1);
  if (box.empty()) return;
  float rad[4];
  Radii(r, radius, kCornersAll, rad);
  const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f;
  const float hw = r.width() * 0.5f, hh = r.height() * 0.5f;
  const float band = std::max(rad[0], t) + 2.0f;
  auto cov = [&](float px, float py) {
    const float d = RoundedRectDistance(px, py, cx, cy, hw, hh, rad);
    return Clamp01(0.5f - (std::fabs(d + t * 0.5f) - t * 0.5f));
  };
  // Rows clear of the top and bottom bands only touch the left and right columns.
  const int top = std::clamp(static_cast<int>(std::ceil(r.y0 + band)), box.y0, box.y1);
  const int bottom = std::clamp(static_cast<int>(std::floor(r.y1 - band)), top, box.y1);
  FillShape({box.x0, box.y0, box.x1, top}, c, cov);
  FillShape({box.x0, bottom, box.x1, box.y1}, c, cov);
  const int left = std::clamp(static_cast<int>(std::ceil(r.x0 + band)), box.x0, box.x1);
  const int right = std::clamp(static_cast<int>(std::floor(r.x1 - band)), left, box.x1);
  FillShape({box.x0, top, left, bottom}, c, cov);
  FillShape({right, top, box.x1, bottom}, c, cov);
}

void Canvas::FillCircle(float cx, float cy, float radius, const Color& c) {
  if (radius <= 0 || c.a <= 0) return;
  FillShape(Bounds(cx - radius, cy - radius, cx + radius, cy + radius), c, [&](float px, float py) {
    return Clamp01(0.5f - (std::hypot(px - cx, py - cy) - radius));
  });
}

void Canvas::StrokeCircle(float cx, float cy, float radius, float t, const Color& c) {
  if (radius <= 0 || t <= 0 || c.a <= 0) return;
  const float o = radius + t * 0.5f + 1.0f;
  FillShape(Bounds(cx - o, cy - o, cx + o, cy + o), c, [&](float px, float py) {
    return Clamp01(0.5f - (std::fabs(std::hypot(px - cx, py - cy) - radius) - t * 0.5f));
  });
}

void Canvas::Line(float x0, float y0, float x1, float y1, float t, const Color& c) {
  if (t <= 0 || c.a <= 0) return;
  const float h = t * 0.5f + 1.0f;
  const float dx = x1 - x0, dy = y1 - y0;
  const float len2 = dx * dx + dy * dy;
  FillShape(Bounds(std::min(x0, x1) - h, std::min(y0, y1) - h, std::max(x0, x1) + h, std::max(y0, y1) + h), c,
            [&](float px, float py) {
              const float u = len2 > 0 ? std::clamp(((px - x0) * dx + (py - y0) * dy) / len2, 0.0f, 1.0f) : 0.0f;
              return Clamp01(0.5f - (std::hypot(px - (x0 + u * dx), py - (y0 + u * dy)) - t * 0.5f));
            });
}

void Canvas::FillTriangle(Vec2 a, Vec2 b, Vec2 c, const Color& col) {
  if (col.a <= 0) return;
  float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
  if (std::fabs(area) < 1e-6f) return;
  if (area < 0) std::swap(b, c);
  struct Edge {
    float nx, ny, d;
  };
  auto edge = [](Vec2 p, Vec2 q) {
    const float ex = q.x - p.x, ey = q.y - p.y, l = std::hypot(ex, ey);
    // Inward normal for counter-clockwise (in y-down space) winding.
    return Edge{-ey / l, ex / l, (-ey * p.x + ex * p.y) / l};
  };
  const Edge e[3] = {edge(a, b), edge(b, c), edge(c, a)};
  FillShape(Bounds(std::min({a.x, b.x, c.x}) - 1, std::min({a.y, b.y, c.y}) - 1, std::max({a.x, b.x, c.x}) + 1,
                   std::max({a.y, b.y, c.y}) + 1),
            col, [&](float px, float py) {
              float m = 1e9f;
              for (const Edge& k : e) m = std::min(m, k.nx * px + k.ny * py - k.d);
              return Clamp01(m + 0.5f);
            });
}

void Canvas::ProgressBar(RectF r, float fraction, const Color& track, const Color& fill) {
  const float rad = r.height() * 0.5f;
  FillRect(r, track, rad);
  fraction = Clamp01(fraction);
  if (fraction <= 0.0f) return;
  // The fill is the track's shape cut at the fraction, so short fills keep round ends.
  const float fx = r.x0 + r.width() * fraction;
  float rads[4];
  Radii(r, rad, kCornersAll, rads);
  const float cx = (r.x0 + r.x1) * 0.5f, cy = (r.y0 + r.y1) * 0.5f;
  FillShape(Bounds(r.x0, r.y0, fx, r.y1), fill, [&](float px, float py) {
    return Clamp01(0.5f - RoundedRectDistance(px, py, cx, cy, r.width() * 0.5f, r.height() * 0.5f, rads)) *
           Clamp01(fx - px + 0.5f);
  });
}

void Canvas::Blit(const Image& img, RectF dst, const Color& tint, float radius) {
  if (img.width <= 0 || img.height <= 0 || dst.width() <= 0 || dst.height() <= 0 || tint.a <= 0) return;
  if (img.rgba.size() < static_cast<size_t>(img.width) * img.height * 4) return;
  const PixelRect box = Bounds(dst.x0, dst.y0, dst.x1, dst.y1);
  if (box.empty()) return;
  float rad[4];
  Radii(dst, radius, kCornersAll, rad);
  const float cx = (dst.x0 + dst.x1) * 0.5f, cy = (dst.y0 + dst.y1) * 0.5f;
  const float hw = dst.width() * 0.5f, hh = dst.height() * 0.5f;
  const float sx = img.width / dst.width(), sy = img.height / dst.height();
  const int tr = CoverageByte(Clamp01(tint.r)), tg = CoverageByte(Clamp01(tint.g)), tb = CoverageByte(Clamp01(tint.b));
  const int ta = CoverageByte(Clamp01(tint.a));
  auto texel = [&](int x, int y, int out[4]) {
    x = std::clamp(x, 0, img.width - 1);
    y = std::clamp(y, 0, img.height - 1);
    const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
    out[0] = p[0] * p[3];
    out[1] = p[1] * p[3];
    out[2] = p[2] * p[3];
    out[3] = p[3] * 255;
  };
  for (int y = box.y0; y < box.y1; ++y) {
    uint8_t* row = premul_.rgba.data() + (static_cast<size_t>(y) * width_) * 4;
    const float py = y + 0.5f;
    const float v = (py - dst.y0) * sy - 0.5f;
    const int v0 = static_cast<int>(std::floor(v));
    const float fv = v - v0;
    for (int x = box.x0; x < box.x1; ++x) {
      const float px = x + 0.5f;
      const float cov = Clamp01(0.5f - RoundedRectDistance(px, py, cx, cy, hw, hh, rad));
      if (cov <= 0.0f) continue;
      const float u = (px - dst.x0) * sx - 0.5f;
      const int u0 = static_cast<int>(std::floor(u));
      const float fu = u - u0;
      int t00[4], t10[4], t01[4], t11[4];
      texel(u0, v0, t00);
      texel(u0 + 1, v0, t10);
      texel(u0, v0 + 1, t01);
      texel(u0 + 1, v0 + 1, t11);
      // Premultiplied bilinear sample (0..65025 per channel).
      float s[4];
      for (int k = 0; k < 4; ++k)
        s[k] = (t00[k] * (1 - fu) + t10[k] * fu) * (1 - fv) + (t01[k] * (1 - fu) + t11[k] * fu) * fv;
      const float m = cov / (255.0f * 255.0f);
      auto chan = [&](float val, int tint_k) {
        return static_cast<uint32_t>(std::clamp(val * m * tint_k * (ta / 255.0f) + 0.5f, 0.0f, 255.0f));
      };
      const uint32_t a = static_cast<uint32_t>(std::clamp(s[3] * m * ta + 0.5f, 0.0f, 255.0f));
      if (!a) continue;
      const uint32_t px32 = std::min(a, chan(s[0], tr)) | (std::min(a, chan(s[1], tg)) << 8) |
                            (std::min(a, chan(s[2], tb)) << 16) | (a << 24);
      uint32_t d = Load(row + x * 4);
      BlendPremul(&d, px32);
      Store(row + x * 4, d);
    }
  }
  Touch(box);
}

void Canvas::DrawImage(const Image& image, RectF dst, const Color& tint, float radius) {
  Blit(image, dst, tint, radius);
}

void Canvas::DrawImage(const CanvasImage& ci, RectF dst, const Color& tint, float radius) {
  // The smallest level still at least as large as the destination.
  const Image* img = &ci.image;
  for (const Image& m : ci.mips) {
    if (m.width < dst.width() || m.height < dst.height()) break;
    img = &m;
  }
  Blit(*img, dst, tint, radius);
}

float Canvas::Text(const Font& font, float size, float x, float y, std::string_view s, const Color& c) {
  if (s.empty() || size <= 0.0f) return 0.0f;
  const Font::Impl& f = *font.impl_;
  std::lock_guard lock(f.mutex);
  const int size_q = std::max(1, static_cast<int>(std::lround(size * 4.0f)));
  const float scale = f.Scale(size);
  const float bolden = f.Bolden(size);
  const int baseline = static_cast<int>(std::lround(y + font.Ascent(size)));
  const Premul full = ToPremul(c, 1.0f);
  const PixelRect cl = clip();
  float pen = x;
  PixelRect touched;
  for (size_t i = 0; i < s.size();) {
    const size_t start = i;
    const uint32_t cp = NextCodePoint(s, i);
    const auto& g = f.Find(cp);
    if (g.index == 0 && f.fallback && f.fallback->HasGlyph(cp)) {
      pen += Text(*f.fallback, size, pen, y, s.substr(start, i - start), c);
      continue;
    }
    const float advance = g.advance * scale + bolden;
    if (full.a && cp != ' ') {
      const int ix = static_cast<int>(std::floor(pen));
      const int sub = std::clamp(static_cast<int>((pen - ix) * 4.0f), 0, 3);
      const auto& b = f.Raster(g.index, size_q, sub);
      const PixelRect gr = Intersect({ix + b.x0, baseline + b.y0, ix + b.x0 + b.w, baseline + b.y0 + b.h}, cl);
      for (int py = gr.y0; py < gr.y1; ++py) {
        uint8_t* row = premul_.rgba.data() + (static_cast<size_t>(py) * width_) * 4;
        const uint8_t* src = b.px.data() + static_cast<size_t>(py - baseline - b.y0) * b.w;
        for (int px = gr.x0; px < gr.x1; ++px) {
          const int k = src[px - ix - b.x0];
          if (!k) continue;
          uint32_t v = Load(row + px * 4);
          BlendPremul(&v, ScalePremul(full.px, k));
          Store(row + px * 4, v);
        }
      }
      touched = Union(touched, gr);
    }
    pen += advance;
  }
  Touch(touched);
  return pen - x;
}

std::vector<std::string> Canvas::WrapText(const Font& font, float size, const std::string& text, float max_w,
                                          int max_lines) {
  return ui::WrapText(FontMeasurer(&font, &font), false, size, text, max_w, max_lines);
}

std::string Canvas::EllipsizeText(const Font& font, float size, std::string text, float max_w) {
  return Ellipsize(FontMeasurer(&font, &font), false, size, std::move(text), max_w);
}

// ---- CanvasPainter ----------------------------------------------------------------

float CanvasPainter::TextWidth(bool bold, float size, std::string_view text) const {
  const Font* f = bold ? bold_ : regular_;
  return f ? f->TextWidth(size, text) : 0.0f;
}

void CanvasPainter::FillRect(Vec2 min, Vec2 max, const Color& c, float rounding, int corners) {
  canvas_.FillRect({min.x, min.y, max.x, max.y}, c, rounding, corners);
}

void CanvasPainter::StrokeRect(Vec2 min, Vec2 max, const Color& c, float rounding, float thickness) {
  canvas_.StrokeRect({min.x, min.y, max.x, max.y}, c, rounding, thickness);
}

void CanvasPainter::FillCircle(Vec2 center, float radius, const Color& c, int) {
  canvas_.FillCircle(center.x, center.y, radius, c);
}

void CanvasPainter::StrokeCircle(Vec2 center, float radius, const Color& c, int, float thickness) {
  canvas_.StrokeCircle(center.x, center.y, radius, thickness, c);
}

void CanvasPainter::DrawImage(void* texture, Vec2 min, Vec2 max, const Color& tint, float rounding) {
  if (texture) canvas_.DrawImage(*static_cast<const CanvasImage*>(texture), {min.x, min.y, max.x, max.y}, tint, rounding);
}

void CanvasPainter::Text(bool bold, float size, Vec2 pos, const Color& c, std::string_view text) {
  const Font* f = bold ? bold_ : regular_;
  if (f) canvas_.Text(*f, size, pos.x, pos.y, text, c);
}

void CanvasPainter::PushClip(Vec2 min, Vec2 max) { canvas_.PushClip(RectF{min.x, min.y, max.x, max.y}); }
void CanvasPainter::PopClip() { canvas_.PopClip(); }

}  // namespace recomp::achievements::ui
