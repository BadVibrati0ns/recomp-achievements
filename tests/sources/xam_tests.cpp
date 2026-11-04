#include <cstring>

#include "recomp/achievements/sources/xam.h"
#include "recomp/achievements/sources/xdbf.h"
#include "recomp/achievements/system.h"
#include "test_util.h"

using namespace recomp::achievements;
using namespace recomp::achievements::xbox;

namespace {

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  out.insert(out.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)});
}

uint32_t Get32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void AddXboxSet(System& system) {
  AchievementSet set;
  set.id = "x360";
  set.platform = Platform::kXbox360;
  for (uint32_t id : {1u, 2u, 3u}) {
    Achievement a;
    a.platform_id = id;
    a.local_id = std::to_string(id);
    a.name = "A" + std::to_string(id);
    a.points = 10;
    set.achievements.push_back(a);
  }
  system.AddSet(set);
}

// An enumerator's output: `ids` XACHIEVEMENT_DETAILS with base flags 0x8.
std::vector<uint8_t> Details(std::initializer_list<uint32_t> ids) {
  std::vector<uint8_t> out;
  for (uint32_t id : ids) {
    Put32(out, id);
    for (int i = 0; i < 7; ++i) Put32(out, 0);
    Put32(out, 0x8);
  }
  return out;
}

}  // namespace

TEST(XamWriteAchievementsUnlocks) {
  System system;
  AddXboxSet(system);
  XamAchievements xam(system, "x360");
  std::vector<uint32_t> unknown;
  xam.SetUnknownHandler([&](uint32_t, uint32_t id) { unknown.push_back(id); });

  std::vector<uint8_t> entries;
  Put32(entries, 0); Put32(entries, 2);   // user 0, achievement 2
  Put32(entries, 1); Put32(entries, 99);  // unknown id
  Put32(entries, 3); Put32(entries, 2);   // already unlocked by then
  CHECK_EQ(xam.WriteAchievements(entries.data(), 3), 1u);
  CHECK(system.IsUnlocked("x360", 2));
  CHECK(xam.IsUnlocked(2));
  CHECK(!xam.IsUnlocked(1));
  CHECK_EQ(unknown.size(), 1u);
  CHECK(xam.WriteAchievements(nullptr, 5) == 0);
}

TEST(XamWriteAchievementsMessage) {
  System system;
  AddXboxSet(system);
  XamAchievements xam(system, "x360");
  std::vector<uint8_t> entries;
  Put32(entries, 0); Put32(entries, 1);
  Put32(entries, 0); Put32(entries, 3);
  std::vector<uint8_t> message;
  Put32(message, 2);
  Put32(message, 0x40001000);  // guest address of `entries`
  const auto translate = [&](uint32_t address) -> const uint8_t* {
    return address == 0x40001000 ? entries.data() : nullptr;
  };
  CHECK_EQ(xam.WriteAchievementsMessage(message.data(), translate), 2u);
  CHECK(system.IsUnlocked("x360", 1));
  CHECK(system.IsUnlocked("x360", 3));

  std::vector<uint8_t> unmapped;
  Put32(unmapped, 1);
  Put32(unmapped, 0x50000000);
  CHECK_EQ(xam.WriteAchievementsMessage(unmapped.data(), translate), 0u);
}

TEST(XamApplyUnlockStateMarksDetails) {
  System system;
  AddXboxSet(system);
  XamAchievements xam(system, "x360");
  system.Unlock("x360", 3);
  auto details = Details({1, 3, 42});
  CHECK_EQ(xam.ApplyUnlockState(details.data(), 3), 1u);
  const uint8_t* d1 = details.data();
  const uint8_t* d3 = details.data() + kAchievementDetailsSize;
  const uint8_t* d42 = details.data() + 2 * kAchievementDetailsSize;
  CHECK_EQ(Get32(d1 + 32), 0x8u);
  CHECK_EQ(Get32(d3 + 32), 0x8u | kAchievementFlagAchieved);
  const uint64_t filetime = (uint64_t(Get32(d3 + 24)) << 32) | Get32(d3 + 28);
  const uint64_t unlock = system.GetProgress("x360:3")->unlock_time;
  CHECK_EQ(filetime, UnixToFileTime(unlock));
  CHECK_EQ(Get32(d42 + 32), 0x8u);
}

TEST(XamFileTime) {
  CHECK_EQ(UnixToFileTime(0), 116444736000000000ull);
}

TEST(XamShowHandler) {
  System system;
  XamAchievements xam(system, "x360");
  xam.ShowAchievementsUI(0, 0x415607F0);  // no handler: nothing happens
  uint32_t seen = 0;
  xam.SetShowHandler([&](uint32_t, uint32_t title_id) { seen = title_id; });
  xam.ShowAchievementsUI(0, 0x415607F0);
  CHECK_EQ(seen, 0x415607F0u);
}

namespace {

// A title with online-only achievements 29, 30 and 48, from a synthetic XDBF.
AchievementSet SyntheticTitle() {
  test::XdbfBuilder b;
  uint16_t sid = 100;
  for (uint16_t id : {1, 2, 29, 30, 48}) {
    b.achievements.push_back({id, sid, uint16_t(sid + 1), uint16_t(sid + 1), 0, 10, 0x9});
    b.AddString(1, sid, "Achievement " + std::to_string(id));
    b.AddString(1, uint16_t(sid + 1), "Do thing " + std::to_string(id));
    sid = uint16_t(sid + 2);
  }
  b.AddString(1, 0x8000, "Synthetic Title");
  const auto bytes = b.Build();
  XdbfDatabase db;
  CHECK(db.Load(std::span<const uint8_t>(bytes)));
  XdbfTitle title;
  title.SetBase(std::move(db));
  XdbfSetOptions options;
  options.set_id = "title";
  return title.ToAchievementSet(options);
}

}  // namespace

TEST(XamFindsSplitSets) {
  System system;
  AchievementSet title = SyntheticTitle();
  CHECK_EQ(title.achievements.size(), size_t(5));
  const std::vector<uint32_t> online_ids = {29, 30, 48};
  AchievementSet online = SplitSet(title, online_ids, "title_online", "Online", false);
  CHECK_EQ(title.achievements.size(), size_t(2));
  CHECK_EQ(online.achievements.size(), size_t(3));
  CHECK(online.platform == Platform::kXbox360);
  system.AddSet(title);
  system.AddSet(online);

  XamAchievements xam(system, "title");
  xam.AddSetId("title_online");
  xam.AddSetId("title_online");  // Once is enough.
  CHECK_EQ(xam.extra_set_ids().size(), size_t(1));
  CHECK_EQ(xam.SetFor(1), std::string("title"));
  CHECK_EQ(xam.SetFor(48), std::string("title_online"));
  CHECK(xam.SetFor(7).empty());
  std::vector<uint32_t> unknown;
  xam.SetUnknownHandler([&](uint32_t, uint32_t id) { unknown.push_back(id); });

  // The pack starts off: the game's write is known, but nothing unlocks.
  std::vector<uint8_t> entries;
  Put32(entries, 0); Put32(entries, 29);
  Put32(entries, 0); Put32(entries, 1);
  Put32(entries, 0); Put32(entries, 7);
  CHECK_EQ(xam.WriteAchievements(entries.data(), 3), 1u);
  CHECK_EQ(unknown, std::vector<uint32_t>{7});
  CHECK(!system.IsUnlocked("title_online", 29));
  CHECK(system.IsUnlocked("title", 1));

  // Turned on: the same write unlocks it in the split set.
  system.SetSetEnabled("title_online", true);
  CHECK_EQ(xam.WriteAchievements(entries.data(), 3), 1u);
  CHECK(system.IsUnlocked("title_online", 29));
  CHECK(xam.IsUnlocked(29));
  auto details = Details({1, 29, 30});
  CHECK_EQ(xam.ApplyUnlockState(details.data(), 3), 2u);
  CHECK_EQ(Get32(details.data() + kAchievementDetailsSize + 32), 0x8u | kAchievementFlagAchieved);

  // Off again: kept, but the game reads it as locked.
  system.SetSetEnabled("title_online", false);
  CHECK(system.IsUnlocked("title_online", 29));
  CHECK(!xam.IsUnlocked(29));
  details = Details({1, 29});
  CHECK_EQ(xam.ApplyUnlockState(details.data(), 2), 1u);
  CHECK_EQ(Get32(details.data() + kAchievementDetailsSize + 32), 0x8u);
}
