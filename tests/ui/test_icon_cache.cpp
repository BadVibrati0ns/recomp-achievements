#include <filesystem>
#include <fstream>

#include "recomp/achievements/ui/icon_cache.h"
#include "test.h"

using namespace recomp::achievements::ui;

namespace {

struct FakeTextures {
  int created = 0, destroyed = 0;
  int last_w = 0, last_h = 0;
  uintptr_t next = 1;
  void Install(IconCache& c) {
    c.SetTextureCallbacks(
        [this](int w, int h, const uint8_t*) {
          ++created;
          last_w = w;
          last_h = h;
          return reinterpret_cast<void*>(next++);
        },
        [this](void*) { ++destroyed; });
  }
};

Image Solid(int w, int h) {
  Image img;
  img.width = w;
  img.height = h;
  img.rgba.assign(static_cast<size_t>(w) * h * 4, 255);
  return img;
}

}  // namespace

TEST(icon_split_ref) {
  std::string_view s, b;
  SplitIconRef("xdbf:12", s, b);
  CHECK(s == "xdbf" && b == "12");
  SplitIconRef("trophy:TROP001.PNG", s, b);
  CHECK(s == "trophy" && b == "TROP001.PNG");
  SplitIconRef("ra:123456", s, b);
  CHECK(s == "ra" && b == "123456");
  SplitIconRef("C:/icons/a.png", s, b);
  CHECK(s.empty() && b == "C:/icons/a.png");
  SplitIconRef("icons/a.png", s, b);
  CHECK(s.empty() && b == "icons/a.png");
  SplitIconRef("file:icons/a.png", s, b);
  CHECK(s.empty() && b == "icons/a.png");
  SplitIconRef("weird dir:x.png", s, b);
  CHECK(s.empty());
}

TEST(icon_provider_and_cache) {
  IconCache cache;
  FakeTextures tex;
  tex.Install(cache);
  int asked = 0;
  std::string last_set;
  cache.AddProvider("xdbf", [&](const IconRequest& r, IconData& out) {
    ++asked;
    last_set = std::string(r.set_id);
    if (r.body == "404") return IconResult::kMissing;
    out.image = Solid(64, 32);
    return IconResult::kOk;
  });
  void* t = cache.Get("xdbf:7", "game");
  CHECK(t != nullptr);
  CHECK_EQ(tex.created, 1);
  CHECK_EQ(tex.last_w, 64);
  CHECK_EQ(tex.last_h, 32);
  CHECK(last_set == "game");
  CHECK(cache.Get("xdbf:7", "game") == t);
  CHECK_EQ(asked, 1);
  CHECK(cache.Get("xdbf:7", "other") != t);  // Keyed by set too.

  CHECK(cache.Get("xdbf:404", "game") == nullptr);
  CHECK(cache.Get("xdbf:404", "game") == nullptr);
  CHECK_EQ(asked, 3);  // The miss is cached.
  CHECK(cache.Get("unknown:1") == nullptr);
  CHECK(cache.Get("") == nullptr);

  cache.Clear();
  CHECK_EQ(tex.destroyed, 2);
  CHECK_EQ(cache.size(), 0u);
}

TEST(icon_pending_is_retried) {
  IconCache cache;
  FakeTextures tex;
  tex.Install(cache);
  cache.SetRetrySeconds(0.0);
  int calls = 0;
  cache.AddProvider("ra", [&](const IconRequest&, IconData& out) {
    if (++calls < 3) return IconResult::kPending;
    out.image = Solid(4, 4);
    return IconResult::kOk;
  });
  CHECK(cache.Get("ra:1") == nullptr);
  CHECK(cache.Get("ra:1") == nullptr);
  CHECK(cache.Get("ra:1") != nullptr);
  CHECK_EQ(calls, 3);

  cache.SetRetrySeconds(3600.0);
  int slow = 0;
  cache.AddProvider("slow", [&](const IconRequest&, IconData&) {
    ++slow;
    return IconResult::kPending;
  });
  cache.Get("slow:1");
  cache.Get("slow:1");
  CHECK_EQ(slow, 1);  // Not asked again before the retry time.
}

TEST(icon_files_go_through_decoder) {
  const auto dir = std::filesystem::temp_directory_path() / "recomp_ach_ui_tests" / "icons";
  std::filesystem::create_directories(dir / "icons");
  std::ofstream(dir / "icons" / "a.png", std::ios::binary) << "FAKEPNG";

  IconCache cache;
  FakeTextures tex;
  tex.Install(cache);
  CHECK(cache.has_decoder());  // The built-in one, which rejects fake PNGs.
  CHECK(cache.Get("icons/a.png", "s", dir) == nullptr);

  std::string seen;
  cache.SetDecoder([&](std::span<const uint8_t> bytes, Image& out) {
    seen.assign(bytes.begin(), bytes.end());
    out = Solid(2, 3);
    return true;
  });
  CHECK(cache.Get("icons/a.png", "s", dir) != nullptr);  // Earlier miss was forgotten.
  CHECK(seen == "FAKEPNG");
  CHECK_EQ(tex.last_h, 3);
  CHECK(cache.Get("icons/missing.png", "s", dir) == nullptr);

  // Providers can hand over encoded bytes for the same decoder.
  cache.AddProvider("trophy", [](const IconRequest&, IconData& out) {
    out.bytes = {'P', 'N', 'G'};
    return IconResult::kOk;
  });
  CHECK(cache.Get("trophy:TROP000.PNG") != nullptr);
  CHECK(seen == "PNG");
}

TEST(icon_without_texture_callback) {
  IconCache cache;
  cache.AddProvider("img", [](const IconRequest&, IconData& out) {
    out.image = Solid(1, 1);
    return IconResult::kOk;
  });
  CHECK(cache.Get("img:1") == nullptr);
  CHECK_EQ(cache.size(), 0u);  // Not remembered as a failure.
  Image img;
  CHECK(cache.Load("img:1", {}, {}, img) == IconResult::kOk);
  CHECK_EQ(img.width, 1);
  FakeTextures tex;
  tex.Install(cache);
  CHECK(cache.Get("img:1") != nullptr);
}

TEST(builtin_png_decoder) {
  // A 1x1 red PNG.
  const std::vector<uint8_t> png = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53,
      0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
      0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
      0x44, 0xAE, 0x42, 0x60, 0x82};
  Image img;
  CHECK(DecodeImage(png, img));
  CHECK_EQ(img.width, 1);
  CHECK_EQ(img.height, 1);
  CHECK(img.rgba.size() == 4u && img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[3] == 255);
  Image bad;
  CHECK(!DecodeImage(std::vector<uint8_t>{'F', 'A', 'K', 'E'}, bad));
}

TEST(fallback_icon_for_achievements_without_one) {
  IconCache cache;
  FakeTextures tex;
  tex.Install(cache);
  cache.AddProvider("game", [](const IconRequest&, IconData& out) {
    out.image = Solid(4, 4);
    return IconResult::kOk;
  });
  cache.AddProvider("own", [](const IconRequest&, IconData& out) {
    out.image = Solid(2, 2);
    return IconResult::kOk;
  });
  CHECK(cache.GetOrFallback("") == nullptr);  // No fallback set yet.

  cache.SetFallbackIcon("game:icon");
  void* fallback = cache.GetOrFallback("");
  CHECK(fallback != nullptr);
  CHECK(cache.GetOrFallback("missing:1") == fallback);  // Unloadable icons fall back too.
  void* own = cache.GetOrFallback("own:1");
  CHECK(own != nullptr && own != fallback);  // An achievement's own icon wins.

  cache.SetFallbackIcon("");
  CHECK(cache.GetOrFallback("") == nullptr);
}
