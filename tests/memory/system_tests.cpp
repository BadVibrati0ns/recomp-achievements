// MemoryTriggers driving a real System.

#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "recomp/achievements/memory_triggers.h"
#include "recomp/achievements/system.h"
#include "test_util.h"

using namespace recomp::achievements;

namespace {

Achievement MemoryAchievement(std::string id, std::string condition) {
  Achievement a;
  a.local_id = std::move(id);
  a.name = a.local_id;
  a.trigger.type = "memory";
  a.trigger.params = {{"condition", std::move(condition)}};
  return a;
}

AchievementSet MakeSet(std::string id, std::vector<Achievement> achievements) {
  AchievementSet set;
  set.id = std::move(id);
  set.title = set.id;
  set.platform = Platform::kRetroAchievements;
  set.achievements = std::move(achievements);
  return set;
}

struct Ram {
  std::array<uint8_t, 256> bytes{};
  GuestMemory memory() const {
    GuestMemory m;
    m.AddRegion(0, bytes.size(), bytes.data());
    return m;
  }
};

}  // namespace

MEM_TEST(system_unlock) {
  System system;
  Achievement manual;
  manual.local_id = "manual";
  system.AddSet(MakeSet("game", {MemoryAchievement("a", "0xH0010=3"), manual}));
  Ram ram;
  MemoryTriggers triggers(system, ram.memory());
  std::vector<std::string> unlocked;
  system.AddUnlockListener([&](const UnlockEvent& e) {
    unlocked.push_back(e.view.achievement ? e.view.achievement->key : "?");
  });

  triggers.Update();
  CHECK(triggers.active_count() == 1);  // Only the "memory" achievement.
  CHECK(!system.IsUnlocked("game:a"));
  ram.bytes[0x10] = 3;
  triggers.Update();
  CHECK(system.IsUnlocked("game:a"));
  CHECK(unlocked.size() == 1 && unlocked[0] == "game:a");
  CHECK(triggers.active_count() == 0);
  triggers.Update();
  CHECK(unlocked.size() == 1);
}

MEM_TEST(system_parse_error) {
  System system;
  system.AddSet(MakeSet("game", {MemoryAchievement("good", "0xH0010=1"),
                                 MemoryAchievement("bad", "0xH0010=="),
                                 MemoryAchievement("none", "")}));
  Ram ram;
  MemoryTriggers triggers(system, ram.memory());
  std::vector<ConditionError> reported;
  triggers.SetErrorCallback([&](const ConditionError& e) { reported.push_back(e); });
  triggers.Update();
  system.Reset();  // Bumps generation(): a second sync.
  triggers.Update();
  triggers.Sync();
  CHECK(triggers.active_count() == 1);
  CHECK(triggers.errors().size() == 2);
  CHECK(triggers.errors()[0].key == "game:bad");
  CHECK(triggers.errors()[1].key == "game:none");
  CHECK(reported.size() == 2);  // Each error once, despite three syncs.
}

namespace {
AchievementView ViewOf(const System& system, std::string_view key) {
  for (const AchievementView& v : system.Snapshot())
    if (v.achievement && v.achievement->key == key) return v;
  return {};
}
}  // namespace

MEM_TEST(system_progress) {
  System system;
  // No thresholds: the bar's total comes from the Measured target.
  system.AddSet(MakeSet("game", {MemoryAchievement("collect", "M:0xH0020>=5"),
                                 MemoryAchievement("pct", "G:0xH0021>=200")}));
  Ram ram;
  MemoryTriggers triggers(system, ram.memory());
  triggers.Sync();  // Registers the totals before any value is measured.
  CHECK(ViewOf(system, "game:collect").progress_total == 5);
  CHECK(ViewOf(system, "game:pct").progress_total == 100);

  ram.bytes[0x20] = 2;
  ram.bytes[0x21] = 50;
  triggers.Update();
  AchievementView collect = ViewOf(system, "game:collect");
  AchievementView pct = ViewOf(system, "game:pct");
  CHECK(collect.progress_current == 2 && collect.progress_total == 5);
  CHECK(pct.progress_current == 25 && pct.progress_total == 100);  // 50 of 200.
  ram.bytes[0x20] = 4;
  triggers.Update();
  CHECK(ViewOf(system, "game:collect").progress_current == 4);
  ram.bytes[0x20] = 5;
  triggers.Update();
  CHECK(system.IsUnlocked("game:collect"));

  // Reset clears the System's totals; the triggers put them back.
  system.Reset();
  triggers.Update();
  CHECK(ViewOf(system, "game:collect").progress_total == 5);
  CHECK(ViewOf(system, "game:pct").progress_current == 25);
}

MEM_TEST(system_resync) {
  System system;
  Ram ram;
  MemoryTriggers triggers(system, ram.memory());
  triggers.Update();
  CHECK(triggers.active_count() == 0);

  // A set added later is picked up on the next frame.
  system.AddSet(MakeSet("game", {MemoryAchievement("a", "0xH0010=1"),
                                 MemoryAchievement("b", "0xH0011=1"),
                                 MemoryAchievement("hits", "0xH0013=1.3.")}));
  triggers.Update();
  CHECK(triggers.active_count() == 3);

  ram.bytes[0x10] = 1;
  ram.bytes[0x13] = 1;
  triggers.Update();  // a fires; hits: 1
  CHECK(system.IsUnlocked("game:a"));
  CHECK(triggers.active_count() == 2);
  triggers.Update();  // hits: 2

  // Adding another set keeps hit counts.
  system.AddSet(MakeSet("other", {MemoryAchievement("x", "0xH0014=1")}));
  triggers.Update();  // hits: 3
  CHECK(system.IsUnlocked("game:hits"));
  CHECK(triggers.active_count() == 2);  // b, other:x

  // System::Reset() alone re-arms unlocked achievements and clears hit counts.
  system.Reset();
  triggers.Update();  // Re-synced; a waits for false first; hits: 1
  CHECK(triggers.active_count() == 4);
  CHECK(!system.IsUnlocked("game:a"));
  ram.bytes[0x13] = 0;
  ram.bytes[0x10] = 0;
  triggers.Update();
  ram.bytes[0x13] = 1;
  triggers.Update();  // hits: 2
  system.Reset(std::vector<std::string>{"game:a"});
  ram.bytes[0x10] = 1;
  triggers.Update();  // Cleared again; hits: 1
  triggers.Update();  // hits: 2
  CHECK(!system.IsUnlocked("game:hits"));
  triggers.Update();  // hits: 3
  CHECK(system.IsUnlocked("game:hits"));
  // a was re-armed by the reset too, so it waits for a false frame again.
  CHECK(!system.IsUnlocked("game:a"));
  CHECK(triggers.evaluator().state("game:a") == TriggerState::kWaiting);

  // An achievement unlocked by game code leaves on the next frame.
  system.Unlock("game:b");
  triggers.Update();
  CHECK(!triggers.evaluator().Contains("game:b"));
  CHECK(triggers.active_count() == 2);  // a, other:x

  // Replacing the set with an edited condition re-parses it.
  system.AddSet(MakeSet("game", {MemoryAchievement("c", "0xH0012=1")}));
  triggers.Update();
  CHECK(triggers.active_count() == 2);  // c, other:x
  CHECK(!triggers.evaluator().Contains("game:a"));
  CHECK(triggers.evaluator().condition("game:c") == "0xH0012=1");

  system.RemoveSet("game");
  system.RemoveSet("other");
  triggers.Update();
  CHECK(triggers.active_count() == 0);
}

MEM_TEST(system_imported_set) {
  // A set as tools/ra_import.py writes it, with one memory trigger added.
  System system;
  std::string error;
  CHECK(system.LoadSetFile(RECOMP_ACH_MEMORY_TEST_DATA "/ra-1234.achievements.json", &error));
  if (!error.empty()) std::fprintf(stderr, "%s\n", error.c_str());
  const AchievementSet* set = system.FindSet("ra-1234");
  CHECK(set && set->platform == Platform::kRetroAchievements && set->achievements.size() == 4);
  CHECK(set && set->credits.size() == 4 && set->credits.back().name == "RetroAchievements");
  const Achievement* ten = system.Find("ra-1234:90003");
  CHECK(ten && ten->platform_id == 90003 && ten->icon == "ra:301003");
  CHECK(ten && ten->credits.size() == 1 && ten->credits[0].role == "author");

  Ram ram;
  MemoryTriggers triggers(system, ram.memory());
  triggers.Update();
  CHECK(triggers.active_count() == 1);  // The other three are manual.
  ram.bytes[0x40] = 6;
  triggers.Update();
  auto progress = system.GetProgress("ra-1234:90003");
  CHECK(progress && progress->best_value == 6);
  ram.bytes[0x40] = 10;
  triggers.Update();
  CHECK(system.IsUnlocked("ra-1234:90003"));
}
