// The achievement library's icons through the UI's IconCache providers, and
// the library switched on from OverlayOptions.

#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "recomp/achievements/library.h"
#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/icon_cache.h"
#include "recomp/achievements/ui/overlay.h"
#include "test.h"

using namespace recomp::achievements;
using namespace recomp::achievements::ui;
namespace fs = std::filesystem;

namespace {

fs::path FreshDir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "recomp_ach_ui_tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

nlohmann::json ReadJson(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return nlohmann::json::parse(in, nullptr, false);
}

AchievementSet OneSet() {
  AchievementSet set;
  set.id = "s";
  set.title = "Icon Game";
  Achievement a;
  a.local_id = "a";
  a.name = "Painted";
  a.icon = "gen:pixels";
  a.locked_icon = "gen:png";
  set.achievements = {a};
  return set;
}

// 3x2 with distinct, partly transparent pixels.
Image Pattern() {
  Image img;
  img.width = 3;
  img.height = 2;
  for (int i = 0; i < 6; ++i) img.rgba.insert(img.rgba.end(), {uint8_t(i * 40), uint8_t(255 - i), 7, uint8_t(i * 50)});
  return img;
}

}  // namespace

TEST(library_icons_round_trip_through_png) {
  const fs::path root = FreshDir("library_icons") / "library";
  const Image pattern = Pattern();
  const std::vector<uint8_t> png = EncodePng(pattern.width, pattern.height, pattern.rgba);
  Image decoded;
  CHECK(DecodeImage(png, decoded));
  CHECK(decoded.width == 3 && decoded.height == 2 && decoded.rgba == pattern.rgba);

  IconCache icons;
  icons.AddProvider("gen", [&](const IconRequest& r, IconData& out) {
    if (r.body == "pixels") out.image = pattern;
    else out.bytes = png;
    return IconResult::kOk;
  });
  System sys;
  sys.AddSet(OneSet());
  LibraryOptions o;
  o.game_id = "icon_game";
  o.root = root;
  o.wine_host = false;
  o.icon_source = LibraryIconSourceFor(icons);
  {
    Library lib(sys, o);
    lib.Flush();
    CHECK(lib.stats().icon_writes == 1);  // Both refs give the same pixels: one file.
  }
  const fs::path dir = root / "icon_game";
  const auto doc = ReadJson(dir / "sets" / "s.achievements.json");
  const std::string icon = doc["achievements"][0].value("icon", "");
  CHECK(!icon.empty());
  CHECK(icon == doc["achievements"][0].value("locked_icon", ""));
  std::vector<uint8_t> bytes;
  CHECK(ReadFileBytes(dir / "sets" / icon, bytes));
  CHECK(bytes == png);  // PNG bytes are copied, RGBA is encoded the same way.
  Image back;
  CHECK(DecodeImage(bytes, back));
  CHECK(back.width == 3 && back.height == 2 && back.rgba == pattern.rgba);
}

TEST(library_from_overlay_options) {
  const fs::path tmp = FreshDir("library_overlay");
  const Image pattern = Pattern();
  const auto png = EncodePng(pattern.width, pattern.height, pattern.rgba);
  {
    std::ofstream out(tmp / "icon.png", std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  }
  System sys;
  sys.AddSet(OneSet());
  {
    OverlayOptions off;
    Overlay plain(sys, off);
    CHECK(plain.library() == nullptr);
  }
  OverlayOptions options;
  options.global_preferences = false;
  options.fallback_icon = (tmp / "icon.png").string();
  options.library.game_id = "overlay_game";
  options.library.title = "Overlay Game";
  options.library.root = tmp / "library";
  options.library.wine_host = false;
  {
    Overlay overlay(sys, options);
    overlay.icons().AddProvider("gen", [&](const IconRequest&, IconData& out) {
      out.image = pattern;
      return IconResult::kOk;
    });
    CHECK(overlay.library() != nullptr);
    sys.Unlock("s:a");
    overlay.library()->Flush();
  }
  const fs::path dir = tmp / "library" / "overlay_game";
  const auto game = ReadJson(dir / "game.json");
  CHECK(game.value("title", "") == "Overlay Game");
  const std::string icon = game.value("icon", "");
  CHECK(!icon.empty());
  std::vector<uint8_t> bytes;
  CHECK(ReadFileBytes(dir / icon, bytes) && bytes == png);
  const auto set = ReadJson(dir / "sets" / "s.achievements.json");
  CHECK(!set["achievements"][0].value("icon", "").empty());
  CHECK(ReadJson(dir / "progress" / "default.json")["achievements"]["s:a"]["unlocked"] == true);
}
