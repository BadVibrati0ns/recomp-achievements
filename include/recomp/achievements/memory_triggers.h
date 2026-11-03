#pragma once

// "memory" triggers: unlock conditions written in RetroAchievements' condition
// syntax (trigger.params.condition), evaluated against the recomp's guest
// memory with rcheevos. Nothing here talks to RetroAchievements; only the
// condition parser and evaluator of rcheevos are built.
//
//   recomp::achievements::MemoryTriggers triggers(system,
//       recomp::achievements::GuestMemory::N64Recomp(rdram));
//   // once per frame, on the thread that owns guest memory:
//   triggers.Update();
//
// See docs/memory-triggers.md for the syntax and address maps.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/guest_memory.h"

namespace recomp::achievements {

class System;

// rcheevos' trigger states.
enum class TriggerState : uint8_t {
  kInactive,
  kWaiting,  // Must be false for one frame before it can fire.
  kActive,
  kPaused,  // A PauseIf condition is true.
  kReset,   // Returned for one frame when hit counts were reset.
  kTriggered,
  kPrimed,  // Every condition but the Trigger ones is true.
  kDisabled,
};

struct ConditionError {
  std::string key;  // Achievement key (or the key given to ConditionEvaluator::Add).
  std::string condition;
  int code = 0;  // rcheevos error code (negative).
  std::string message;
};

// The System-free part: a keyed list of parsed conditions, evaluated once per
// call to Update(). MemoryTriggers builds on it; use it directly for
// conditions that aren't achievements (e.g. a game-defined trigger type).
class ConditionEvaluator {
 public:
  enum class EventKind {
    kTriggered,  // The condition fired; it is now inactive until Reset().
    kPrimed,     // Entered the primed state (a "challenge" indicator).
    kUnprimed,   // Left the primed state (sent before kTriggered too).
    kReset,      // Hit counts were reset by a ResetIf.
    kMeasured,   // The Measured value changed.
  };
  struct Event {
    EventKind kind;
    const std::string& key;
    uint32_t value = 0;   // kMeasured: current measured value.
    uint32_t target = 0;  // kMeasured: the Measured target.
    bool as_percent = false;
  };
  using EventFn = std::function<void(const Event&)>;

  ConditionEvaluator();
  ~ConditionEvaluator();
  ConditionEvaluator(ConditionEvaluator&&) noexcept;
  ConditionEvaluator& operator=(ConditionEvaluator&&) noexcept;

  // Parses and adds (or replaces) a condition. With require_false_first the
  // condition starts kWaiting, as on RetroAchievements, so one that is
  // already true when it is added doesn't fire until it has been false.
  bool Add(std::string key, std::string_view condition, ConditionError* error = nullptr,
           bool require_false_first = true);
  // Parses without adding; returns an rcheevos error code or 0.
  static int Validate(std::string_view condition, std::string* message = nullptr);
  void Remove(std::string_view key);
  void Clear();
  bool Contains(std::string_view key) const;
  size_t size() const;
  // Clears hit counts and re-arms (kWaiting), including triggered ones.
  void Reset(std::string_view key);
  void ResetAll();

  // Evaluates every condition once against memory. Calls fn for state changes.
  void Update(const GuestMemory& memory, const EventFn& fn = nullptr);

  TriggerState state(std::string_view key) const;
  // Measured value / target; target 0 means the condition has no Measured.
  // The value is 0 until the first Update().
  uint32_t measured_value(std::string_view key) const;
  uint32_t measured_target(std::string_view key) const;
  bool measured_as_percent(std::string_view key) const;  // Measured% (G:)
  // The condition text a key was added with, or empty.
  std::string_view condition(std::string_view key) const;
  std::vector<std::string> keys() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Registers every locked achievement whose trigger type is "memory" from a
// System and unlocks it when its condition fires. Measured values go to
// System::RecordValue with the Measured target as the bar's total (0-100 of
// 100 for Measured%). Re-syncs when System::generation() changes. Call
// everything from one thread; the System may unlock from any thread.
class MemoryTriggers {
 public:
  struct Options {
    bool require_false_first = true;
    bool record_progress = true;
  };
  using ErrorFn = std::function<void(const ConditionError&)>;
  using PrimedFn = std::function<void(std::string_view key, bool primed)>;

  MemoryTriggers(System& system, GuestMemory memory);
  MemoryTriggers(System& system, GuestMemory memory, Options options);
  ~MemoryTriggers();
  MemoryTriggers(const MemoryTriggers&) = delete;
  MemoryTriggers& operator=(const MemoryTriggers&) = delete;

  void SetMemory(GuestMemory memory);
  const GuestMemory& memory() const { return memory_; }
  // Called once per parse error found while syncing (each error once).
  void SetErrorCallback(ErrorFn fn) { on_error_ = std::move(fn); }
  // Primed/unprimed notifications for an on-screen challenge indicator.
  void SetPrimedCallback(PrimedFn fn) { on_primed_ = std::move(fn); }

  // Re-reads the System's pending "memory" achievements now. Update() does
  // this by itself whenever System::generation() changes.
  void Sync();
  // Clears every condition's hit counts and re-arms it, then syncs. Update()
  // does this by itself after a progress reset, reload or profile switch.
  void ResetConditions();
  // Evaluates every active condition once. Call once per guest frame.
  void Update();

  // Achievements whose conditions failed to parse, from the last Sync().
  const std::vector<ConditionError>& errors() const { return errors_; }
  size_t active_count() const { return evaluator_.size(); }
  const ConditionEvaluator& evaluator() const { return evaluator_; }

 private:
  uint64_t SetsSignature() const;
  void DropUnlocked();

  System& system_;
  GuestMemory memory_;
  Options options_;
  ConditionEvaluator evaluator_;
  std::vector<ConditionError> errors_;
  std::vector<std::string> reported_errors_;  // "key\ncondition" already sent to on_error_.
  ErrorFn on_error_;
  PrimedFn on_primed_;
  uint64_t sets_signature_ = 0;
  uint64_t generation_ = 0;
  bool synced_ = false;
  // Unlocks reported by the System's listener, which may run on any thread.
  struct UnlockQueue;
  std::shared_ptr<UnlockQueue> unlocked_;
  int listener_id_ = 0;
};

}  // namespace recomp::achievements
