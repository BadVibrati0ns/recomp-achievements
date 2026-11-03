#pragma once

// Framework-agnostic backend for a PS3 recomp's sceNpTrophy HLE functions.
// Plain host structs; the HLE module copies them into guest structures
// (byte-swapping, truncating strings to the SCE_NP_TROPHY_*_MAX_SIZE limits).
// Unlock state lives in System, so saves, popups and the UI are shared with
// every other set. See docs/ps3.md.

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include "recomp/achievements/sources/trophy.h"

namespace recomp::achievements {
class System;
}

namespace recomp::achievements::trophy {

// Results of the context calls. The HLE module maps these to its
// SCE_NP_TROPHY_ERROR_* constants (names given per value).
enum class Result : int32_t {
  kOk = 0,
  kNotRegistered,         // CONTEXT_NOT_REGISTERED
  kInvalidArgument,       // INVALID_ARGUMENT
  kInvalidTrophyId,       // INVALID_TROPHY_ID
  kAlreadyUnlocked,       // ALREADY_UNLOCKED
  kCannotUnlockPlatinum,  // CANNOT_UNLOCK_PLATINUM
  kExceedsMax,            // EXCEEDS_MAX (more trophies than a flag array holds)
  kLoadFailed,            // INVALID_FORMAT / CONF_DOES_NOT_EXIST
};
const char* ResultName(Result result);

// SceNpTrophyFlagArray: SCE_NP_TROPHY_FLAG_SETSIZE (128) bits in 32-bit
// words, bit (id & 31) of word (id >> 5). Host-endian; swap when storing.
inline constexpr uint32_t kFlagSetSize = 128;
struct FlagArray {
  std::array<uint32_t, kFlagSetSize / 32> flag_bits{};
  bool test(uint32_t id) const { return id < kFlagSetSize && (flag_bits[id >> 5] >> (id & 31)) & 1; }
  void set(uint32_t id) {
    if (id < kFlagSetSize) flag_bits[id >> 5] |= 1u << (id & 31);
  }
};

// SceNpTrophyGameDetails / SceNpTrophyGameData.
struct GameDetails {
  uint32_t num_trophies = 0, num_platinum = 0, num_gold = 0, num_silver = 0, num_bronze = 0;
  std::string title;        // title-name
  std::string description;  // title-detail
};
struct GameData {
  uint32_t unlocked_trophies = 0, unlocked_platinum = 0, unlocked_gold = 0, unlocked_silver = 0,
           unlocked_bronze = 0;
};

// SceNpTrophyDetails / SceNpTrophyData.
struct TrophyDetails {
  int32_t trophy_id = kInvalidTrophyId;
  Grade grade = Grade::kUnknown;
  std::string name;
  std::string description;
  bool hidden = false;
};
struct TrophyData {
  uint64_t timestamp = 0;  // CellRtcTick (microseconds since 0001-01-01); 0 while locked.
  int32_t trophy_id = kInvalidTrophyId;
  bool unlocked = false;
};

// CellRtcTick for a Unix time in seconds.
uint64_t UnixToRtcTick(uint64_t unix_seconds);

class TrophyContext {
 public:
  // set_id is the AchievementSet id the trophies are registered under.
  TrophyContext(System& system, std::string set_id);
  TrophyContext(const TrophyContext&) = delete;
  TrophyContext& operator=(const TrophyContext&) = delete;

  struct RegisterOptions {
    int language = 1;  // CELL_SYSUTIL_LANG_*; -1 = the set's default text.
    std::string category;
    std::string title;
  };
  // sceNpTrophyRegisterContext: loads TROPHY.TRP and adds its set to System
  // (replacing an earlier registration of the same set id).
  Result Register(const std::filesystem::path& trp_path, const RegisterOptions& options,
                  std::string* error = nullptr);
  Result Register(const std::filesystem::path& trp_path, std::string* error = nullptr) {
    return Register(trp_path, RegisterOptions{}, error);
  }
  Result Register(TrophyFile file, const RegisterOptions& options);
  bool registered() const;

  // sceNpTrophyUnlockTrophy. platinum_id receives the platinum trophy this
  // unlock completed, or kInvalidTrophyId. Unlocking the platinum directly is
  // refused (kCannotUnlockPlatinum); it unlocks once every trophy whose pid
  // points at it is unlocked.
  Result UnlockTrophy(int32_t trophy_id, int32_t* platinum_id = nullptr);

  // sceNpTrophyGetTrophyUnlockState. count receives the number of trophies
  // in the set (not the number unlocked), as the real call does.
  Result GetTrophyUnlockState(FlagArray* flags, uint32_t* count) const;

  // sceNpTrophyGetGameInfo. Either pointer may be null.
  Result GetGameInfo(GameDetails* details, GameData* data) const;

  // sceNpTrophyGetTrophyInfo. A locked hidden trophy reports hidden = true
  // and empty name/description, as the system software does, unless
  // reveal_hidden is set.
  Result GetTrophyInfo(int32_t trophy_id, TrophyDetails* details, TrophyData* data,
                       bool reveal_hidden = false) const;

  // sceNpTrophyGetGameIcon / sceNpTrophyGetTrophyIcon: PNG bytes, empty when
  // missing. The span stays valid until the next Register.
  std::span<const uint8_t> GetGameIcon() const;
  std::span<const uint8_t> GetTrophyIcon(int32_t trophy_id) const;

  // Unlocks any platinum whose trophies are all unlocked (e.g. after a save
  // from before a set gained its platinum). Returns how many were unlocked.
  uint32_t SyncPlatinum();

  const TrophyFile& file() const { return file_; }
  const std::string& set_id() const { return set_id_; }

 private:
  std::string Key(int32_t id) const;
  bool IsUnlockedLocked(int32_t id) const;
  int32_t CompletePlatinumLocked(int32_t platinum_id);

  System& system_;
  std::string set_id_;
  TrophyFile file_;
  bool registered_ = false;
  // Recursive: System listeners run on the unlocking thread and may call back in.
  mutable std::recursive_mutex mutex_;
};

}  // namespace recomp::achievements::trophy
