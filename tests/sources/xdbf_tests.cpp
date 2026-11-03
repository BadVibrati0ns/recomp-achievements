#include <cstring>

#include "recomp/achievements/sources/xdbf.h"
#include "recomp/achievements/system.h"
#include "test_util.h"

using namespace recomp::achievements;
using namespace recomp::achievements::xbox;

namespace {

const std::vector<uint8_t> kFakePng = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 1, 2, 3};

test::XdbfBuilder BaseBuilder() {
  test::XdbfBuilder b;
  b.achievements = {
      {1, 10, 11, 12, 101, 20, 0x9},   // visible
      {2, 20, 21, 22, 102, 50, 0x1},   // secret (bit 3 clear)
  };
  b.AddString(1, 10, "First Steps");
  b.AddString(1, 11, "You took your first steps.");
  b.AddString(1, 12, "Take your first steps.");
  b.AddString(1, 20, "Hidden One");
  b.AddString(1, 21, "Found it.");
  b.AddString(1, 22, "Found it.");
  b.AddString(1, 0x8000, "Test Game");
  b.AddString(3, 10, "Erste Schritte");
  b.AddString(3, 0x8000, "Testspiel");
  b.images.push_back({101, kFakePng});
  b.images.push_back({0x8000, {9, 9}});
  b.with_xthd = true;
  b.title_id = 0x454108E6;
  return b;
}

XdbfDatabase Parse(const std::vector<uint8_t>& bytes) {
  XdbfDatabase db;
  std::string error;
  CHECK(db.Load(std::span<const uint8_t>(bytes), &error));
  CHECK(error.empty());
  return db;
}

}  // namespace

TEST(XdbfParsesAchievements) {
  XdbfDatabase db = Parse(BaseBuilder().Build());
  CHECK(db.valid());
  auto list = db.achievements();
  CHECK_EQ(list.size(), size_t(2));
  if (list.size() != 2) return;
  CHECK_EQ(list[0].id, 1);
  CHECK_EQ(list[0].label_id, 10);
  CHECK_EQ(list[0].image_id, 101u);
  CHECK_EQ(list[0].gamerscore, 20);
  CHECK(!list[0].secret());
  CHECK(list[1].secret());
  CHECK_EQ(db.GetString(10), "First Steps");
  CHECK_EQ(db.image(101).size(), kFakePng.size());
  CHECK(db.image(999).empty());
}

TEST(XdbfTitleInfo) {
  auto b = BaseBuilder();
  b.version[0] = 1, b.version[1] = 0, b.version[2] = 607, b.version[3] = 0;
  XdbfDatabase db = Parse(b.Build());
  auto info = db.title_info();
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->title_id, 0x454108E6u);
  CHECK_EQ(info->version_string(), "1.0.607.0");
  CHECK_EQ(db.title(), "Test Game");
  CHECK_EQ(db.title(Language::kGerman), "Testspiel");
}

TEST(XdbfLanguageFallback) {
  auto b = BaseBuilder();
  XdbfDatabase db = Parse(b.Build());
  CHECK_EQ(db.GetString(10, Language::kGerman), "Erste Schritte");
  // German table lacks string 11: falls back to English.
  CHECK_EQ(db.GetString(11, Language::kGerman), "You took your first steps.");
  // No French table at all.
  CHECK_EQ(db.GetString(10, Language::kFrench), "First Steps");
  CHECK(!db.FindString(Language::kFrench, 10).has_value());
  CHECK(db.HasLanguage(Language::kGerman));
  CHECK_EQ(db.languages().size(), size_t(2));

  // Without English, the XSTC default language is next, then any table.
  test::XdbfBuilder jp;
  jp.default_language = 2;
  jp.AddString(4, 1, "Bonjour");
  jp.AddString(2, 1, "Konnichiwa");
  XdbfDatabase jdb = Parse(jp.Build());
  CHECK(jdb.default_language() == Language::kJapanese);
  CHECK_EQ(jdb.GetString(1, Language::kGerman), "Konnichiwa");
  test::XdbfBuilder only_fr;
  only_fr.with_xstc = false;
  only_fr.AddString(4, 1, "Bonjour");
  XdbfDatabase fdb = Parse(only_fr.Build());
  CHECK(fdb.default_language() == Language::kEnglish);
  CHECK_EQ(fdb.GetString(1, Language::kGerman), "Bonjour");
  CHECK_EQ(fdb.GetString(2), "");
}

TEST(XdbfRejectsBadInput) {
  XdbfDatabase db;
  std::string error;
  std::vector<uint8_t> junk(64, 0);
  CHECK(!db.Load(std::span<const uint8_t>(junk), &error));
  CHECK(!error.empty());
  CHECK(!db.valid());
  CHECK(db.achievements().empty());

  auto bytes = BaseBuilder().Build();
  std::vector<uint8_t> header_only(bytes.begin(), bytes.begin() + 30);
  CHECK(!db.Load(std::span<const uint8_t>(header_only)));

  // Cut the content area: entries pointing past the end are dropped.
  std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 4);
  CHECK(db.Load(std::span<const uint8_t>(cut)));
  CHECK(db.image(0x8000).empty());
  CHECK_EQ(db.achievements().size(), size_t(2));
}

TEST(XdbfTitleUpdateMerge) {
  test::XdbfBuilder tu;
  tu.achievements = {
      {2, 20, 21, 22, 102, 75, 0x9},   // changed gamerscore, now visible
      {3, 30, 31, 32, 103, 10, 0x9},   // new
  };
  tu.AddString(1, 20, "Not Hidden Anymore");
  tu.AddString(1, 21, "Found it.");
  tu.AddString(1, 30, "DLC");
  tu.images.push_back({101, {7}});
  tu.with_xthd = true;
  tu.title_id = 0x454108E6;
  tu.version[2] = 2;

  XdbfTitle title;
  title.SetBase(Parse(BaseBuilder().Build()));
  title.SetTitleUpdate(Parse(tu.Build()));
  auto list = title.achievements();
  CHECK_EQ(list.size(), size_t(3));
  if (list.size() != 3) return;
  CHECK_EQ(list[0].id, 1);
  CHECK_EQ(list[1].id, 2);
  CHECK_EQ(list[1].gamerscore, 75);
  CHECK_EQ(list[2].id, 3);
  CHECK_EQ(title.GetString(20), "Not Hidden Anymore");
  CHECK_EQ(title.GetString(10), "First Steps");  // only in base
  // The base's German beats the TU's English for a German player.
  CHECK_EQ(title.GetString(10, Language::kGerman), "Erste Schritte");
  CHECK_EQ(title.image(101).size(), size_t(1));  // TU image wins
  CHECK_EQ(title.title_info()->build, 2);
  CHECK_EQ(title.title(), "Test Game");  // from base
}

TEST(XdbfToAchievementSet) {
  XdbfTitle title;
  title.SetBase(Parse(BaseBuilder().Build()));
  XdbfSetOptions options;
  options.set_id = "game";
  options.category = "Base game";
  AchievementSet set = title.ToAchievementSet(options);
  CHECK_EQ(set.id, "game");
  CHECK(set.platform == Platform::kXbox360);
  CHECK_EQ(set.title, "Test Game");
  CHECK_EQ(set.category, "Base game");
  CHECK_EQ(set.source.value("title_id", ""), "454108E6");
  CHECK_EQ(set.achievements.size(), size_t(2));
  if (set.achievements.size() != 2) return;
  const Achievement& a = set.achievements[0];
  CHECK_EQ(a.local_id, "1");
  CHECK_EQ(a.platform_id, 1u);
  CHECK_EQ(a.name, "First Steps");
  CHECK_EQ(a.description, "You took your first steps.");
  CHECK_EQ(a.locked_description, "Take your first steps.");
  CHECK_EQ(a.points, 20u);
  CHECK(!a.secret);
  CHECK_EQ(a.icon, "xdbf:101");
  CHECK_EQ(a.display_order, 0u);
  CHECK(set.achievements[1].secret);
  CHECK_EQ(set.achievements[1].locked_description, "");  // same as description

  CHECK_EQ(title.ResolveIcon(a.icon).size(), kFakePng.size());
  CHECK_EQ(title.ResolveIcon("xdbf:0x65").size(), kFakePng.size());
  CHECK(title.ResolveIcon("xdbf:").empty());
  CHECK(title.ResolveIcon("xdbf:12x").empty());
  CHECK(title.ResolveIcon("trophy:TROP000.PNG").empty());
  CHECK(ParseIconRef("xdbf:32768").value_or(0) == 0x8000);
}

TEST(XdbfFindTitleUpdateSpa) {
  test::TempDir dir;
  auto make = [](uint32_t tid, uint16_t build) {
    auto b = BaseBuilder();
    b.title_id = tid;
    b.version[2] = build;
    return b.Build();
  };
  const auto tree = dir.path / "content" / "0000000000000000" / "454108E6";
  test::WriteFile(tree / "00000002" / "AAAA" / "spa.bin", make(0x454108E6, 556));
  test::WriteFile(tree / "00000002" / "BBBB" / "spa.bin", make(0x454108E6, 607));
  test::WriteFile(tree / "000B0000" / "CCCC" / "spa.bin", make(0x454108E6, 600));
  test::WriteFile(tree / "00000002" / "DDDD" / "spa.bin", make(0x12345678, 999));  // other title
  test::WriteFile(tree / "00000002" / "EEEE" / "spa.bin", {1, 2, 3});              // junk

  auto best = FindTitleUpdateSpa({dir.path / "missing", dir.path / "content"}, 0x454108E6);
  CHECK_EQ(best.parent_path().filename().string(), "BBBB");
  CHECK(FindTitleUpdateSpa({dir.path / "content"}, 0x0BADF00D).empty());

  // A loose spa.bin in a root counts too.
  test::WriteFile(dir.path / "loose" / "spa.bin", make(0x0BADF00D, 1));
  CHECK_EQ(FindTitleUpdateSpa({dir.path / "loose"}, 0x0BADF00D), dir.path / "loose" / "spa.bin");
}

TEST(XdbfSetUnlocksThroughSystem) {
  XdbfTitle title;
  title.SetBase(Parse(BaseBuilder().Build()));
  System system;
  XdbfSetOptions options;
  options.set_id = "x360";
  system.AddSet(title.ToAchievementSet(options));
  const Achievement* a = system.FindByPlatformId("x360", 2);
  CHECK(a != nullptr);
  if (!a) return;
  CHECK_EQ(a->key, "x360:2");
  CHECK(!system.IsUnlocked("x360", 2));
  CHECK(system.Unlock("x360", 2));
  CHECK(!system.Unlock("x360", 2));
  CHECK(system.IsUnlocked("x360", 2));
  CHECK_EQ(system.GetTotals("x360").unlocked_points, 50u);
}

// Optional check against a real spa.bin. 77 = skipped.
int RunRealSpaTest(const char* path) {
  if (!path || !*path || !std::filesystem::is_regular_file(path)) {
    std::printf("real spa.bin test skipped (set RECOMP_ACH_TEST_SPA)\n");
    return 77;
  }
  XdbfDatabase db;
  std::string error;
  if (!db.LoadFile(path, &error)) {
    std::printf("FAIL: %s\n", error.c_str());
    return 1;
  }
  XdbfTitle title;
  title.SetTitleUpdate(std::move(db));
  XdbfSetOptions options;
  options.set_id = "real";
  AchievementSet set = title.ToAchievementSet(options);
  auto info = title.title_info();
  size_t named = 0, with_png = 0, secret = 0;
  uint32_t points = 0;
  for (const Achievement& a : set.achievements) {
    named += !a.name.empty();
    secret += a.secret;
    points += a.points;
    auto png = title.ResolveIcon(a.icon);
    with_png += png.size() > 8 && std::memcmp(png.data(), "\x89PNG", 4) == 0;
  }
  std::printf("%s: title \"%s\" %08X v%s, %zu achievements (%zu named, %zu with PNG icons, %zu "
              "secret), %u gamerscore, languages %zu\n",
              path, set.title.c_str(), info ? info->title_id : 0,
              info ? info->version_string().c_str() : "?", set.achievements.size(), named, with_png,
              secret, points, title.title_update().languages().size());
  const bool ok = info && !set.title.empty() && !set.achievements.empty() &&
                  named == set.achievements.size() && with_png > 0 && points > 0 &&
                  !title.image(kTitleStringId).empty();
  // When the file sits in a content tree, also run the TU search over it.
  for (auto p = std::filesystem::path(path).parent_path(); info && p.has_relative_path(); p = p.parent_path()) {
    if (p.filename() == "0000000000000000") {
      std::printf("best spa.bin under %s: %s\n", p.parent_path().string().c_str(),
                  FindTitleUpdateSpa({p.parent_path()}, info->title_id).string().c_str());
      break;
    }
  }
  std::printf("%s\n", ok ? "OK" : "FAIL");
  return ok ? 0 : 1;
}
