#include "recomp/achievements/sources/xam.h"

#include "recomp/achievements/system.h"

namespace recomp::achievements::xbox {

namespace {

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void StoreBE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

}  // namespace

uint64_t UnixToFileTime(uint64_t unix_seconds) {
  constexpr uint64_t kEpochDelta = 11644473600ull;  // 1601-01-01 to 1970-01-01, seconds.
  return (unix_seconds + kEpochDelta) * 10000000ull;
}

XamAchievements::XamAchievements(System& system, std::string set_id)
    : system_(system), set_id_(std::move(set_id)) {}

void XamAchievements::AddSetId(std::string set_id) {
  if (set_id == set_id_) return;
  for (const auto& id : extra_set_ids_)
    if (id == set_id) return;
  extra_set_ids_.push_back(std::move(set_id));
}

std::string XamAchievements::SetFor(uint32_t achievement_id) const {
  if (system_.FindByPlatformId(set_id_, achievement_id)) return set_id_;
  for (const auto& id : extra_set_ids_)
    if (system_.FindByPlatformId(id, achievement_id)) return id;
  return {};
}

uint32_t XamAchievements::WriteAchievements(const uint8_t* entries, uint32_t count) {
  if (!entries) return 0;
  uint32_t unlocked = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = entries + i * kUserAchievementSize;
    const uint32_t user_index = LoadBE32(entry);
    const uint32_t id = LoadBE32(entry + 4);
    const std::string set = SetFor(id);
    if (set.empty()) {
      if (on_unknown_) on_unknown_(user_index, id);
      continue;
    }
    // A set that's off refuses the unlock.
    unlocked += system_.Unlock(set, id) ? 1 : 0;
  }
  return unlocked;
}

uint32_t XamAchievements::WriteAchievementsMessage(
    const uint8_t* message_buffer, const std::function<const uint8_t*(uint32_t)>& translate) {
  if (!message_buffer || !translate) return 0;
  const uint32_t count = LoadBE32(message_buffer);
  const uint32_t address = LoadBE32(message_buffer + 4);
  if (!count || !address) return 0;
  return WriteAchievements(translate(address), count);
}

uint32_t XamAchievements::ApplyUnlockState(uint8_t* details, uint32_t count) const {
  if (!details) return 0;
  uint32_t marked = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* d = details + i * kAchievementDetailsSize;
    const uint32_t id = LoadBE32(d);
    const std::string set = SetFor(id);
    if (set.empty() || !system_.IsSetEnabled(set)) continue;
    const Achievement* a = system_.FindByPlatformId(set, id);
    if (!a) continue;
    const auto progress = system_.GetProgress(a->key);
    if (!progress || !progress->unlocked) continue;
    // Titles test only the flag; a zero time would read as "unknown".
    const uint64_t filetime = UnixToFileTime(progress->unlock_time ? progress->unlock_time : 1);
    StoreBE32(d + 24, uint32_t(filetime >> 32));
    StoreBE32(d + 28, uint32_t(filetime));
    StoreBE32(d + 32, LoadBE32(d + 32) | kAchievementFlagAchieved);
    ++marked;
  }
  return marked;
}

bool XamAchievements::IsUnlocked(uint32_t achievement_id) const {
  const std::string set = SetFor(achievement_id);
  return !set.empty() && system_.IsSetEnabled(set) && system_.IsUnlocked(set, achievement_id);
}

void XamAchievements::ShowAchievementsUI(uint32_t user_index, uint32_t title_id) const {
  if (on_show_) on_show_(user_index, title_id);
}

}  // namespace recomp::achievements::xbox
