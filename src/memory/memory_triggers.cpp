#include "recomp/achievements/memory_triggers.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "recomp/achievements/system.h"

namespace recomp::achievements {
namespace {

std::string ConditionOf(const Achievement& achievement) {
  const nlohmann::json& params = achievement.trigger.params;
  if (params.is_object()) {
    auto it = params.find("condition");
    if (it != params.end() && it->is_string()) return it->get<std::string>();
  }
  return {};
}

}  // namespace

struct MemoryTriggers::UnlockQueue {
  std::mutex mutex;
  std::vector<std::string> keys;
};

MemoryTriggers::MemoryTriggers(System& system, GuestMemory memory)
    : MemoryTriggers(system, std::move(memory), Options{}) {}

MemoryTriggers::MemoryTriggers(System& system, GuestMemory memory, Options options)
    : system_(system),
      memory_(std::move(memory)),
      options_(options),
      unlocked_(std::make_shared<UnlockQueue>()) {
  // Unlocks don't bump generation(), so manual unlocks arrive this way.
  listener_id_ = system_.AddUnlockListener([queue = unlocked_](const UnlockEvent& event) {
    if (event.replay || !event.view.achievement) return;
    std::lock_guard lock(queue->mutex);
    queue->keys.push_back(event.view.achievement->key);
  });
}

MemoryTriggers::~MemoryTriggers() { system_.RemoveListener(listener_id_); }

void MemoryTriggers::DropUnlocked() {
  std::vector<std::string> keys;
  {
    std::lock_guard lock(unlocked_->mutex);
    keys.swap(unlocked_->keys);
  }
  for (const std::string& key : keys) evaluator_.Remove(key);
}

void MemoryTriggers::SetMemory(GuestMemory memory) { memory_ = std::move(memory); }

uint64_t MemoryTriggers::SetsSignature() const {
  // Tells set changes apart from progress resets when generation() moves:
  // replacing a set allocates a new achievement list.
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  for (const AchievementSet* set : system_.sets()) {
    mix(reinterpret_cast<uintptr_t>(set));
    mix(reinterpret_cast<uintptr_t>(set->achievements.data()));
    mix(set->achievements.size());
  }
  return h;
}

void MemoryTriggers::Sync() {
  std::unordered_map<std::string, std::string> pending;
  system_.ForEachPending("memory", [&](const Achievement& achievement, const Progress&) {
    pending.emplace(achievement.key, ConditionOf(achievement));
  });

  for (const std::string& key : evaluator_.keys()) {
    if (!pending.count(key)) evaluator_.Remove(key);
  }

  std::vector<ConditionError> errors;
  for (const auto& [key, condition] : pending) {
    if (evaluator_.Contains(key) && evaluator_.condition(key) == condition) continue;
    ConditionError error;
    if (evaluator_.Add(key, condition, &error, options_.require_false_first)) {
      // Show the bar (with its real total) before the first measured change.
      const uint32_t target = evaluator_.measured_target(key);
      if (options_.record_progress && target) {
        system_.RecordValue(key, 0, evaluator_.measured_as_percent(key) ? 100.0 : target);
      }
      continue;
    }
    // A bad edit of a working condition disables it rather than keeping stale logic.
    evaluator_.Remove(key);
    errors.push_back(std::move(error));
  }
  std::sort(errors.begin(), errors.end(),
            [](const ConditionError& a, const ConditionError& b) { return a.key < b.key; });

  if (on_error_) {
    for (const ConditionError& error : errors) {
      std::string id = error.key + '\n' + error.condition;
      if (std::find(reported_errors_.begin(), reported_errors_.end(), id) !=
          reported_errors_.end()) {
        continue;
      }
      reported_errors_.push_back(std::move(id));
      on_error_(error);
    }
  }
  errors_ = std::move(errors);
  sets_signature_ = SetsSignature();
  synced_ = true;
}

void MemoryTriggers::ResetConditions() {
  const uint64_t generation = system_.generation();
  evaluator_.ResetAll();
  Sync();
  generation_ = generation;
}

void MemoryTriggers::Update() {
  DropUnlocked();
  const uint64_t generation = system_.generation();
  if (!synced_ || generation != generation_) {
    // Same sets but a new generation: progress was reset or reloaded, or the
    // profile changed, so hit counts from before no longer apply. (A set
    // change in the same frame as a reset is treated as a set change only.)
    const bool sets_changed = !synced_ || SetsSignature() != sets_signature_;
    if (!sets_changed) evaluator_.ResetAll();
    Sync();
    generation_ = generation;
  }
  if (memory_.empty() || evaluator_.size() == 0) return;

  // Collected first: unlock and progress listeners may call back into the
  // System, and the evaluator must not change while it iterates.
  std::vector<std::string> fired;
  struct Value {
    std::string key;
    double value, total;
  };
  std::vector<Value> values;
  std::vector<std::pair<std::string, bool>> primed;
  evaluator_.Update(memory_, [&](const ConditionEvaluator::Event& event) {
    switch (event.kind) {
      case ConditionEvaluator::EventKind::kTriggered:
        fired.push_back(event.key);
        break;
      case ConditionEvaluator::EventKind::kPrimed:
      case ConditionEvaluator::EventKind::kUnprimed:
        primed.emplace_back(event.key, event.kind == ConditionEvaluator::EventKind::kPrimed);
        break;
      case ConditionEvaluator::EventKind::kMeasured: {
        if (event.as_percent && event.target) {
          values.push_back({event.key, std::floor(100.0 * event.value / event.target), 100.0});
        } else {
          values.push_back({event.key, double(event.value), double(event.target)});
        }
        break;
      }
      case ConditionEvaluator::EventKind::kReset:
        break;
    }
  });

  if (options_.record_progress) {
    for (const Value& v : values) system_.RecordValue(v.key, v.value, v.total);
  }
  if (on_primed_) {
    for (const auto& [key, is_primed] : primed) on_primed_(key, is_primed);
  }
  for (const std::string& key : fired) {
    // Removed either way; the next Sync() re-adds it if it is still locked
    // (e.g. after a progress reset).
    system_.Unlock(key);
    evaluator_.Remove(key);
  }
}

}  // namespace recomp::achievements
