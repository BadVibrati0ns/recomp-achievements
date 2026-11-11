#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/notification_center.h"
#include "test.h"

using namespace recomp::achievements;
using namespace recomp::achievements::ui;

namespace {

// A style with round-number timings so phases are easy to check.
std::shared_ptr<Style> TestStyle(int max_visible = 1) {
  auto s = std::make_shared<Style>(BuiltinTheme("minimal"));
  s->unlock.timing = {0.5f, 1.0f, 0.5f};
  s->unlock.max_visible = max_visible;
  s->unlock.animation = Animation::kSlide;
  s->progress.timing = {0.25f, 0.5f, 0.25f};
  s->sound.unlock = "builtin:minimal";
  s->sound.progress = "builtin:tick";
  return s;
}

Notification Unlock(const std::string& key, std::shared_ptr<const Style> style) {
  Notification n;
  n.kind = NotificationKind::kUnlock;
  n.key = key;
  n.name = key;
  n.style = std::move(style);
  return n;
}

Notification Step(const std::string& key, uint32_t cur, uint32_t total, std::shared_ptr<const Style> style) {
  Notification n;
  n.kind = NotificationKind::kProgress;
  n.key = key;
  n.name = key;
  n.item = "item" + std::to_string(cur);
  n.current = cur;
  n.total = total;
  n.style = std::move(style);
  return n;
}

}  // namespace

TEST(center_unlock_lifecycle) {
  NotificationCenter c;
  std::vector<std::string> sounds;
  c.SetSoundHandler([&](const NotificationCenter::SoundRequest& r) { sounds.push_back(r.sound); });
  c.Push(Unlock("a", TestStyle()));
  CHECK(c.Active().empty());  // Promoted on the render thread only.
  CHECK(c.Busy());

  c.Update(0.0f);
  auto act = c.Active();
  CHECK_EQ(act.size(), 1u);
  CHECK(act[0].phase == Phase::kIn);
  CHECK_NEAR(act[0].alpha, 0.0, 1e-6);
  CHECK_NEAR(act[0].slide, 1.0, 1e-6);
  CHECK_EQ(sounds.size(), 1u);
  CHECK(sounds[0] == "builtin:minimal");

  c.Update(0.25f);
  act = c.Active();
  CHECK(act[0].phase == Phase::kIn);
  CHECK_NEAR(act[0].phase_t, 0.5, 1e-5);
  CHECK(act[0].slide > 0.0f && act[0].slide < 0.5f);  // Ease-out: past halfway in.

  c.Update(0.5f);  // 0.75 s: holding.
  act = c.Active();
  CHECK(act[0].phase == Phase::kHold);
  CHECK_NEAR(act[0].alpha, 1.0, 1e-6);
  CHECK_NEAR(act[0].slide, 0.0, 1e-6);

  c.Update(1.0f / 4);  // 1.0 s
  c.Update(0.25f);     // 1.25 s
  c.Update(0.25f);     // 1.5 s: out begins
  c.Update(0.25f);     // 1.75 s
  act = c.Active();
  CHECK_EQ(act.size(), 1u);
  CHECK(act[0].phase == Phase::kOut);
  CHECK_NEAR(act[0].alpha, 0.5, 1e-5);

  c.Update(0.25f);  // 2.0 s: done.
  CHECK(c.Active().empty());
  CHECK(!c.Busy());
  CHECK_EQ(sounds.size(), 1u);
}

TEST(center_queue_one_at_a_time) {
  NotificationCenter c;
  int sounds = 0;
  c.SetSoundHandler([&](const NotificationCenter::SoundRequest&) { ++sounds; });
  auto style = TestStyle();
  for (const char* k : {"a", "b", "c"}) c.Push(Unlock(k, style));
  c.Update(0.0f);
  CHECK_EQ(c.Active().size(), 1u);
  CHECK(c.Active()[0].n.key == "a");
  CHECK_EQ(c.stats().queued_unlocks, 2u);
  for (int i = 0; i < 8; ++i) c.Update(0.25f);  // 2.0 s: "a" ends, "b" starts.
  CHECK_EQ(c.Active().size(), 1u);
  CHECK(c.Active()[0].n.key == "b");
  CHECK_NEAR(c.Active()[0].elapsed, 0.0, 1e-6);
  for (int i = 0; i < 8; ++i) c.Update(0.25f);
  CHECK(c.Active()[0].n.key == "c");
  for (int i = 0; i < 8; ++i) c.Update(0.25f);
  CHECK(c.Active().empty());
  CHECK_EQ(sounds, 3);
}

TEST(center_stacking) {
  NotificationCenter c;
  auto style = TestStyle(3);
  for (const char* k : {"a", "b", "c", "d"}) c.Push(Unlock(k, style));
  c.Update(0.0f);
  auto act = c.Active();
  CHECK_EQ(act.size(), 3u);
  for (int i = 0; i < 3; ++i) {
    CHECK_EQ(act[i].slot, i);
    CHECK_NEAR(act[i].stack_pos, i, 1e-6);
  }
  for (int i = 0; i < 8; ++i) c.Update(0.25f);  // All three end together; "d" takes slot 0.
  act = c.Active();
  CHECK_EQ(act.size(), 1u);
  CHECK(act[0].n.key == "d");
  CHECK_EQ(act[0].slot, 0);
}

TEST(center_stack_glides_down) {
  NotificationCenter c;
  auto style = TestStyle(2);
  c.Push(Unlock("a", style));
  c.Update(0.0f);
  c.Update(1.0f / 8);
  c.Push(Unlock("b", style));  // Arrives later, so it outlives "a".
  c.Update(0.0f);
  CHECK_EQ(c.Active()[1].slot, 1);
  for (int i = 0; i < 14; ++i) c.Update(0.125f);  // "a" at 1.875 s.
  for (int i = 0; i < 13; ++i) c.Update(0.01f);   // ... and gone at 2.0 s.
  auto act = c.Active();
  CHECK_EQ(act.size(), 1u);
  CHECK(act[0].n.key == "b");
  CHECK_EQ(act[0].slot, 0);
  CHECK(act[0].stack_pos < 1.0f);  // Moving toward 0, not snapped.
  c.Update(0.01f);
  const float before = c.Active()[0].stack_pos;
  c.Update(0.1f);
  CHECK(c.Active()[0].stack_pos < before);
}

TEST(center_max_queued_drops_oldest) {
  NotificationCenter c;
  auto style = TestStyle();
  style->max_queued = 2;
  for (const char* k : {"a", "b", "c", "d", "e"}) c.Push(Unlock(k, style));
  auto st = c.stats();
  CHECK_EQ(st.queued_unlocks, 2u);
  CHECK_EQ(st.dropped, 3u);
  c.Update(0.0f);
  CHECK(c.Active()[0].n.key == "d");
}

TEST(center_default_queue_holds_a_whole_list) {
  // An old save unlocking 255 achievements at once: every popup waits its turn.
  NotificationCenter c;
  auto style = std::make_shared<Style>();
  CHECK_EQ(style->max_queued, 255);
  for (int i = 0; i < 300; ++i) c.Push(Unlock(("k" + std::to_string(i)).c_str(), style));
  auto st = c.stats();
  CHECK_EQ(st.queued_unlocks, 255u);
  CHECK_EQ(st.dropped, 45u);
}

TEST(center_progress_coalesces_visible) {
  NotificationCenter c;
  int ticks = 0;
  c.SetSoundHandler([&](const NotificationCenter::SoundRequest& r) {
    if (r.kind == NotificationKind::kProgress) ++ticks;
  });
  auto style = TestStyle();
  c.Push(Step("set:flip", 1, 14, style));
  c.Update(0.0f);
  c.Update(0.5f);  // Holding.
  c.Push(Step("set:flip", 2, 14, style));
  c.Update(0.0f);
  auto act = c.Active();
  CHECK_EQ(act.size(), 1u);
  CHECK_EQ(act[0].n.current, 2u);
  CHECK(act[0].n.item == "item2");
  CHECK_NEAR(act[0].elapsed, 0.25, 1e-6);  // Back to the start of the hold.
  CHECK_EQ(ticks, 2);
}

TEST(center_progress_coalesces_waiting) {
  NotificationCenter c;
  auto style = TestStyle();
  c.Push(Step("x", 1, 5, style));
  c.Push(Step("y", 1, 5, style));
  c.Push(Step("y", 2, 5, style));
  c.Push(Step("y", 3, 5, style));
  CHECK_EQ(c.stats().queued_progress, 2u);
  style->progress_coalesce = false;
  c.Push(Step("y", 4, 5, style));
  CHECK_EQ(c.stats().queued_progress, 3u);
}

TEST(center_progress_skip_complete_and_disable) {
  NotificationCenter c;
  auto style = TestStyle();
  c.Push(Step("x", 5, 5, style));
  CHECK_EQ(c.stats().queued_progress, 0u);
  style->progress_skip_complete = false;
  c.Push(Step("x", 5, 5, style));
  CHECK_EQ(c.stats().queued_progress, 1u);
  c.SetEnabled(NotificationKind::kProgress, false);
  CHECK_EQ(c.stats().queued_progress, 0u);
  c.Push(Step("y", 1, 5, style));
  CHECK_EQ(c.stats().queued_progress, 0u);
}

TEST(center_unlock_supersedes_waiting_progress) {
  NotificationCenter c;
  auto style = TestStyle();
  c.Push(Step("a", 1, 3, style));
  c.Push(Step("a", 2, 3, style));
  c.Push(Unlock("a", style));
  CHECK_EQ(c.stats().queued_progress, 0u);
  CHECK_EQ(c.stats().queued_unlocks, 1u);
}

TEST(center_unlock_and_progress_run_in_parallel) {
  NotificationCenter c;
  auto style = TestStyle();
  c.Push(Unlock("a", style));
  c.Push(Step("b", 1, 3, style));
  c.Update(0.0f);
  auto act = c.Active();
  CHECK_EQ(act.size(), 2u);
  CHECK(act[0].n.kind == NotificationKind::kUnlock);
  CHECK(act[1].n.kind == NotificationKind::kProgress);
  c.Update(1.0f);  // Progress (1.0 s total) is done; unlock (2.0 s) isn't.
  act = c.Active();
  CHECK_EQ(act.size(), 1u);
  CHECK(act[0].n.kind == NotificationKind::kUnlock);
}

TEST(center_pause_holds_queue) {
  NotificationCenter c;
  c.SetPaused(true);
  c.Push(Unlock("a", TestStyle()));
  c.Update(1.0f);
  CHECK(c.Active().empty());
  CHECK(c.Busy());
  c.SetPaused(false);
  c.Update(0.0f);
  CHECK_EQ(c.Active().size(), 1u);
}

TEST(center_long_frame_is_clamped) {
  NotificationCenter c;
  c.Push(Unlock("a", TestStyle()));
  c.Update(0.0f);
  c.Update(10.0f);  // A hitch must not swallow the popup.
  CHECK_EQ(c.Active().size(), 1u);
  c.Update(-1.0f);
  c.Update(std::nanf(""));
  CHECK_EQ(c.Active().size(), 1u);
}

TEST(center_sound_disabled) {
  NotificationCenter c;
  int n = 0;
  c.SetSoundHandler([&](const NotificationCenter::SoundRequest&) { ++n; });
  auto style = TestStyle();
  style->sound.enabled = false;
  c.Push(Unlock("a", style));
  c.Update(0.0f);
  CHECK_EQ(n, 0);
}

TEST(center_tier_sound) {
  NotificationCenter c;
  std::string sound;
  c.SetSoundHandler([&](const NotificationCenter::SoundRequest& r) { sound = r.sound; });
  auto style = std::make_shared<Style>(BuiltinTheme("ps3"));
  auto n = Unlock("a", style);
  n.tier = Tier::kGold;
  c.Push(n);
  c.Update(0.0f);
  CHECK(sound == "builtin:ps3_gold");
}

TEST(center_default_resolver_uses_platform) {
  NotificationCenter c;
  Notification n;
  n.key = "a";
  n.platform = Platform::kPlayStation3;
  c.Push(n);
  c.Update(0.0f);
  CHECK(c.Active()[0].n.style->theme == "ps3");
  c.SetStyleResolver([](std::string_view, Platform, const AchievementSet*) {
    auto s = std::make_shared<Style>(BuiltinTheme("retro"));
    return std::shared_ptr<const Style>(s);
  });
  Notification m;
  m.key = "b";
  m.platform = Platform::kPlayStation3;
  c.Push(m);
  c.Update(0.0f);
  CHECK(c.Active().back().n.key == "b");
  CHECK(c.Active().back().n.style->theme == "retro");
}

TEST(center_events_from_views) {
  AchievementSet set;
  set.id = "s";
  set.title = "Set";
  set.platform = Platform::kXbox360;
  Achievement a;
  a.key = "s:flip";
  a.set_id = "s";
  a.name = "Flip Trick Encyclopedia";
  a.description = "Land every flip trick.";
  a.locked_description = "Land some tricks.";
  a.icon = "xdbf:3";
  a.points = 25;
  Achievement hidden = a;
  hidden.key = "s:hidden";
  hidden.name = "Secret thing";
  hidden.secret = true;
  hidden.locked_icon = "xdbf:99";

  NotificationCenter c;
  ProgressEvent pe;
  pe.view.achievement = &a;
  pe.view.set = &set;
  pe.view.progress_current = 3;
  pe.view.progress_total = 14;
  pe.item = "Kickflip";
  c.OnProgress(pe);
  UnlockEvent ue;
  ue.view.achievement = &hidden;
  ue.view.set = &set;
  ue.replay = true;
  c.OnUnlock(ue);
  pe.view.achievement = &hidden;
  c.OnProgress(pe);

  c.Update(0.0f);
  auto act = c.Active();
  CHECK_EQ(act.size(), 2u);
  CHECK(act[0].n.kind == NotificationKind::kUnlock);
  CHECK(act[0].n.name == "Secret thing");  // Revealed on unlock.
  CHECK(act[0].n.replay);
  CHECK(act[0].n.style->theme == "xbox360");
  CHECK(act[1].n.name == "Flip Trick Encyclopedia");
  CHECK(act[1].n.item == "Kickflip");
  CHECK_EQ(act[1].n.current, 3u);
  CHECK_EQ(act[1].n.total, 14u);
  CHECK(act[1].n.icon == "xdbf:3");

  const auto text = FormatPopup(act[1].n);
  CHECK(text.title == "Kickflip");
  CHECK(text.name == "Flip Trick Encyclopedia");
  CHECK(text.points == "3/14");

  // The hidden one's step is still waiting behind the first; its text is safe.
  for (int i = 0; i < 12; ++i) c.Update(0.25f);
  act = c.Active();
  bool saw = false;
  for (const auto& p : act) {
    if (p.n.kind != NotificationKind::kProgress) continue;
    saw = true;
    CHECK(p.n.name == "Secret achievement");
    CHECK(p.n.description == "Land some tricks.");
    CHECK(p.n.icon == "xdbf:99");
  }
  CHECK(saw);
}

TEST(center_format_lines) {
  auto xbox = std::make_shared<Style>(BuiltinTheme("xbox360"));
  Notification n = Unlock("a", xbox);
  n.name = "Pro";
  n.points = 15;
  CHECK(FormatPopup(n).name == "15G - Pro");
  n.points = 0;
  CHECK(FormatPopup(n).name == "Pro");
  CHECK(FormatPopup(n).description.empty());  // Xbox look has no description line.

  auto ps3 = std::make_shared<Style>(BuiltinTheme("ps3"));
  n.style = ps3;
  n.tier = Tier::kPlatinum;
  CHECK(FormatPopup(n).points == "Platinum");
  CHECK(FormatPopup(n).title == "You have earned a trophy.");

  auto classic = std::make_shared<Style>(BuiltinTheme("classic"));
  n.style = classic;
  n.tier = Tier::kNone;
  n.points = 20;
  CHECK(FormatPopup(n).title == "ACHIEVEMENT UNLOCKED");
  CHECK(FormatPopup(n).points == "+20 G");
  n.points = 0;
  CHECK(FormatPopup(n).points.empty());
}

TEST(center_thread_safety) {
  NotificationCenter c;
  auto style = TestStyle(16);
  style->unlock.timing = {0.0f, 0.01f, 0.0f};
  style->progress.timing = {0.0f, 0.01f, 0.0f};
  style->max_queued = 100000;
  style->progress_max_queued = 100000;
  style->progress_coalesce = false;
  std::atomic<int> sounds{0};
  c.SetSoundHandler([&](const NotificationCenter::SoundRequest&) { ++sounds; });
  constexpr int kThreads = 4, kEach = 500;
  std::atomic<bool> done{false};
  std::thread render([&] {
    while (!done.load() || c.Busy()) c.Update(0.02f);
  });
  std::vector<std::thread> game;
  for (int t = 0; t < kThreads; ++t) {
    game.emplace_back([&, t] {
      for (int i = 0; i < kEach; ++i) {
        const std::string key = std::to_string(t) + ":" + std::to_string(i);
        if (i % 2) c.Push(Unlock(key, style));
        else c.Push(Step(key, 1, 9, style));
      }
    });
  }
  for (auto& g : game) g.join();
  done = true;
  render.join();
  CHECK_EQ(sounds.load(), kThreads * kEach);
  CHECK_EQ(c.stats().dropped, 0u);
}

TEST(center_attached_to_system) {
  System system;
  AchievementSet set;
  set.id = "game";
  set.title = "Game";
  set.platform = Platform::kPlayStation3;
  Achievement a;
  a.local_id = "flip";
  a.name = "Flip Trick Encyclopedia";
  a.tier = Tier::kGold;
  a.trigger.type = "checklist";
  a.trigger.event = "trick";
  a.checklist = {"Kickflip", "Heelflip", "Varial"};
  set.achievements.push_back(a);
  system.AddSet(set);

  NotificationCenter c;
  std::vector<std::string> sounds;
  c.SetSoundHandler([&](const NotificationCenter::SoundRequest& r) { sounds.push_back(r.sound); });
  c.Attach(system);
  system.ReportEvent("trick", "Kickflip");
  c.Update(0.0f);
  auto act = c.Active();
  CHECK_EQ(act.size(), 1u);
  if (!act.empty()) {
    CHECK(act[0].n.kind == NotificationKind::kProgress);
    CHECK(act[0].n.item == "Kickflip");
    CHECK_EQ(act[0].n.current, 1u);
    CHECK_EQ(act[0].n.total, 3u);
    CHECK(act[0].n.style->theme == "ps3");
  }
  system.ReportEvent("trick", "Heelflip");
  system.ReportEvent("trick", "Varial");
  c.Update(0.0f);
  act = c.Active();
  bool unlock = false;
  for (const auto& p : act) unlock |= p.n.kind == NotificationKind::kUnlock;
  CHECK(unlock);
  CHECK(std::find(sounds.begin(), sounds.end(), "builtin:ps3_gold") != sounds.end());
  CHECK(sounds.front() == "builtin:ps3_tick");

  c.Detach();
  c.Clear();
  system.TestPopup();
  c.Update(0.0f);
  CHECK(c.Active().empty());  // Detached: nothing arrives.
}

TEST(center_single_slot_shares_one_popup) {
  NotificationCenter c;
  c.SetSingleSlot(true);
  CHECK(c.single_slot());
  auto style = TestStyle(/*max_visible=*/3);
  c.Push(Step("p", 1, 4, style));
  c.Update(0.0f);
  CHECK_EQ(c.Active().size(), 1u);  // Progress takes the free slot.
  CHECK(c.Active()[0].n.kind == NotificationKind::kProgress);

  // An unlock waits for the progress popup instead of showing beside it...
  c.Push(Unlock("a", style));
  c.Push(Unlock("b", style));
  c.Push(Step("q", 2, 4, style));
  c.Update(0.25f);
  CHECK_EQ(c.Active().size(), 1u);
  CHECK(c.Active()[0].n.key == "p");
  // ...then both unlocks run one at a time, ahead of the waiting progress.
  for (int i = 0; i < 4; ++i) c.Update(0.25f);  // Progress (1.0 s) is done.
  CHECK_EQ(c.Active().size(), 1u);
  CHECK(c.Active()[0].n.key == "a");
  for (int i = 0; i < 8; ++i) c.Update(0.25f);
  CHECK(c.Active()[0].n.key == "b");
  for (int i = 0; i < 8; ++i) c.Update(0.25f);
  CHECK_EQ(c.Active().size(), 1u);
  CHECK(c.Active()[0].n.key == "q");
  for (int i = 0; i < 4; ++i) c.Update(0.25f);
  CHECK(c.Active().empty());
  CHECK(!c.Busy());

  // Off again: the style's max_visible applies.
  c.SetSingleSlot(false);
  for (const char* k : {"x", "y"}) c.Push(Unlock(k, style));
  c.Update(0.0f);
  CHECK_EQ(c.Active().size(), 2u);
}
