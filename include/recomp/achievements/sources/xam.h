#pragma once

// XAM's achievement calls, on raw guest data. For Xbox 360 recomps that hook
// XAM themselves (host shims over stock exports) instead of patching their
// framework: pass in the guest structures the game hands XAM, and these
// unlock in a System or report its state back. Framework-agnostic: guest
// memory is just bytes in the console's big-endian layout.

#include <cstdint>
#include <functional>
#include <string>

namespace recomp::achievements {
class System;
}

namespace recomp::achievements::xbox {

// XGI message the game sends for XGIUserWriteAchievements. Its 8-byte buffer
// is {u32 count, u32 guest pointer to `count` XUSER_ACHIEVEMENT entries}.
inline constexpr uint32_t kXgiUserWriteAchievements = 0x000B0008;
// XUSER_ACHIEVEMENT: {u32 user_index, u32 achievement_id}.
inline constexpr uint32_t kUserAchievementSize = 8;
// XACHIEVEMENT_DETAILS: {u32 id, u32 label, u32 description, u32 unachieved,
// u32 image_id, u32 gamerscore, FILETIME achieved (high, low), u32 flags}.
inline constexpr uint32_t kAchievementDetailsSize = 36;
// XACHIEVEMENT_DETAILS_ACHIEVED: set in flags once an achievement is earned.
inline constexpr uint32_t kAchievementFlagAchieved = 0x00020000;

// Unix seconds to a Windows FILETIME (100 ns ticks since 1601).
uint64_t UnixToFileTime(uint64_t unix_seconds);

class XamAchievements {
 public:
  using UnknownHandler = std::function<void(uint32_t user_index, uint32_t achievement_id)>;
  using ShowHandler = std::function<void(uint32_t user_index, uint32_t title_id)>;

  // The System must outlive this. Unlocks go to the set with this id, by
  // XDBF achievement id (its platform_id).
  XamAchievements(System& system, std::string set_id);

  // XGIUserWriteAchievements: `entries` points at `count` guest
  // XUSER_ACHIEVEMENT structs. Every user index unlocks the one local
  // profile. Returns how many were newly unlocked.
  uint32_t WriteAchievements(const uint8_t* entries, uint32_t count);
  // Same, from the XGI message buffer; `translate` turns a guest address
  // into a host pointer (null when unmapped).
  uint32_t WriteAchievementsMessage(const uint8_t* message_buffer,
                                    const std::function<const uint8_t*(uint32_t guest_address)>& translate);

  // After the stock enumerator has written `count` XACHIEVEMENT_DETAILS to
  // `details` (XamEnumerate on an achievement enumerator), marks the ones the
  // System has unlocked: sets kAchievementFlagAchieved and the unlock time.
  // Returns how many were marked.
  uint32_t ApplyUnlockState(uint8_t* details, uint32_t count) const;

  bool IsUnlocked(uint32_t achievement_id) const;

  // XamShowAchievementsUI / XamShowAchievementsUIEx / XamShowAchievementDetailsUI.
  void ShowAchievementsUI(uint32_t user_index, uint32_t title_id) const;

  void SetUnknownHandler(UnknownHandler handler) { on_unknown_ = std::move(handler); }
  void SetShowHandler(ShowHandler handler) { on_show_ = std::move(handler); }
  const std::string& set_id() const { return set_id_; }

 private:
  System& system_;
  std::string set_id_;
  UnknownHandler on_unknown_;
  ShowHandler on_show_;
};

}  // namespace recomp::achievements::xbox
