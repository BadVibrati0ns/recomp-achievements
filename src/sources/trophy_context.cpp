#include "recomp/achievements/sources/trophy_context.h"

#include "recomp/achievements/system.h"

namespace recomp::achievements::trophy {

const char* ResultName(Result result) {
  switch (result) {
    case Result::kOk: return "ok";
    case Result::kNotRegistered: return "context not registered";
    case Result::kInvalidArgument: return "invalid argument";
    case Result::kInvalidTrophyId: return "invalid trophy id";
    case Result::kAlreadyUnlocked: return "already unlocked";
    case Result::kCannotUnlockPlatinum: return "cannot unlock platinum";
    case Result::kExceedsMax: return "exceeds max";
    case Result::kLoadFailed: return "load failed";
  }
  return "unknown";
}

uint64_t UnixToRtcTick(uint64_t unix_seconds) {
  constexpr uint64_t kEpochOffsetSeconds = 62135596800ull;  // 0001-01-01 to 1970-01-01
  return (unix_seconds + kEpochOffsetSeconds) * 1000000ull;
}

TrophyContext::TrophyContext(System& system, std::string set_id)
    : system_(system), set_id_(std::move(set_id)) {}

Result TrophyContext::Register(const std::filesystem::path& trp_path, const RegisterOptions& options,
                               std::string* error) {
  TrophyFile file;
  if (!file.LoadFile(trp_path, options.language, error)) return Result::kLoadFailed;
  return Register(std::move(file), options);
}

Result TrophyContext::Register(TrophyFile file, const RegisterOptions& options) {
  if (!file.valid()) return Result::kLoadFailed;
  std::lock_guard lock(mutex_);
  file_ = std::move(file);
  TrophyFile::SetOptions set_options;
  set_options.set_id = set_id_;
  set_options.category = options.category;
  set_options.title = options.title;
  system_.AddSet(file_.ToAchievementSet(set_options));
  registered_ = true;
  return Result::kOk;
}

bool TrophyContext::registered() const {
  std::lock_guard lock(mutex_);
  return registered_;
}

// By key, not platform id: System treats platform id 0 as "none", but trophy 0
// is usually the platinum.
std::string TrophyContext::Key(int32_t id) const { return set_id_ + ":" + std::to_string(id); }

bool TrophyContext::IsUnlockedLocked(int32_t id) const {
  return id >= 0 && system_.IsUnlocked(Key(id));
}

int32_t TrophyContext::CompletePlatinumLocked(int32_t platinum_id) {
  const TrophyDef* platinum = file_.config().Find(platinum_id);
  if (!platinum || platinum->grade != Grade::kPlatinum || IsUnlockedLocked(platinum_id)) {
    return kInvalidTrophyId;
  }
  bool any = false;
  for (const TrophyDef& t : file_.config().trophies) {
    if (t.pid != platinum_id || t.id == platinum_id) continue;
    if (!IsUnlockedLocked(t.id)) return kInvalidTrophyId;
    any = true;
  }
  if (!any) return kInvalidTrophyId;
  return system_.Unlock(Key(platinum_id)) ? platinum_id : kInvalidTrophyId;
}

Result TrophyContext::UnlockTrophy(int32_t trophy_id, int32_t* platinum_id) {
  if (platinum_id) *platinum_id = kInvalidTrophyId;
  std::lock_guard lock(mutex_);
  if (!registered_) return Result::kNotRegistered;
  const TrophyDef* def = file_.config().Find(trophy_id);
  if (!def) return Result::kInvalidTrophyId;
  if (def->grade == Grade::kPlatinum) return Result::kCannotUnlockPlatinum;
  if (IsUnlockedLocked(trophy_id)) return Result::kAlreadyUnlocked;
  if (!system_.Unlock(Key(trophy_id))) {
    // Unlocked concurrently through System, or the set was removed.
    return IsUnlockedLocked(trophy_id) ? Result::kAlreadyUnlocked : Result::kInvalidTrophyId;
  }
  if (def->pid >= 0) {
    const int32_t plat = CompletePlatinumLocked(def->pid);
    if (platinum_id) *platinum_id = plat;
  }
  return Result::kOk;
}

Result TrophyContext::GetTrophyUnlockState(FlagArray* flags, uint32_t* count) const {
  if (!flags || !count) return Result::kInvalidArgument;
  std::lock_guard lock(mutex_);
  if (!registered_) return Result::kNotRegistered;
  *flags = FlagArray();
  const auto& trophies = file_.config().trophies;
  *count = uint32_t(trophies.size());
  bool exceeds = trophies.size() > kFlagSetSize;
  for (const TrophyDef& t : trophies) {
    if (uint32_t(t.id) >= kFlagSetSize) {
      exceeds = true;
      continue;
    }
    if (IsUnlockedLocked(t.id)) flags->set(uint32_t(t.id));
  }
  return exceeds ? Result::kExceedsMax : Result::kOk;
}

Result TrophyContext::GetGameInfo(GameDetails* details, GameData* data) const {
  std::lock_guard lock(mutex_);
  if (!registered_) return Result::kNotRegistered;
  GameDetails d;
  GameData g;
  d.title = file_.config().title_name;
  d.description = file_.config().title_detail;
  for (const TrophyDef& t : file_.config().trophies) {
    const bool unlocked = IsUnlockedLocked(t.id);
    ++d.num_trophies;
    g.unlocked_trophies += unlocked;
    switch (t.grade) {
      case Grade::kPlatinum: ++d.num_platinum; g.unlocked_platinum += unlocked; break;
      case Grade::kGold: ++d.num_gold; g.unlocked_gold += unlocked; break;
      case Grade::kSilver: ++d.num_silver; g.unlocked_silver += unlocked; break;
      case Grade::kBronze: ++d.num_bronze; g.unlocked_bronze += unlocked; break;
      default: break;
    }
  }
  if (details) *details = std::move(d);
  if (data) *data = g;
  return Result::kOk;
}

Result TrophyContext::GetTrophyInfo(int32_t trophy_id, TrophyDetails* details, TrophyData* data,
                                    bool reveal_hidden) const {
  std::lock_guard lock(mutex_);
  if (!registered_) return Result::kNotRegistered;
  const TrophyDef* def = file_.config().Find(trophy_id);
  if (!def) return Result::kInvalidTrophyId;
  const bool unlocked = IsUnlockedLocked(trophy_id);
  if (details) {
    TrophyDetails d;
    d.trophy_id = def->id;
    d.grade = def->grade;
    d.hidden = def->hidden;
    if (!def->hidden || unlocked || reveal_hidden) {
      d.name = def->name;
      d.description = def->detail;
    }
    *details = std::move(d);
  }
  if (data) {
    TrophyData t;
    t.trophy_id = def->id;
    t.unlocked = unlocked;
    if (unlocked) {
      if (auto progress = system_.GetProgress(Key(trophy_id)); progress && progress->unlock_time) {
        t.timestamp = UnixToRtcTick(progress->unlock_time);
      }
    }
    *data = t;
  }
  return Result::kOk;
}

std::span<const uint8_t> TrophyContext::GetGameIcon() const {
  std::lock_guard lock(mutex_);
  return file_.game_icon();
}

std::span<const uint8_t> TrophyContext::GetTrophyIcon(int32_t trophy_id) const {
  std::lock_guard lock(mutex_);
  return file_.trophy_icon(trophy_id);
}

uint32_t TrophyContext::SyncPlatinum() {
  std::lock_guard lock(mutex_);
  if (!registered_) return 0;
  uint32_t unlocked = 0;
  for (const TrophyDef& t : file_.config().trophies) {
    if (t.grade == Grade::kPlatinum && CompletePlatinumLocked(t.id) != kInvalidTrophyId) ++unlocked;
  }
  return unlocked;
}

}  // namespace recomp::achievements::trophy
