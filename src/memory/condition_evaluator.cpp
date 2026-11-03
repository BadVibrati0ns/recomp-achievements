#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "recomp/achievements/memory_triggers.h"
#include "rc_error.h"
#include "rc_runtime_types.h"

namespace recomp::achievements {
namespace {

constexpr uint32_t kMeasuredUnknown = 0xFFFFFFFFu;  // rcheevos' RC_MEASURED_UNKNOWN.

uint32_t RcPeek(uint32_t address, uint32_t num_bytes, void* ud) {
  return static_cast<const GuestMemory*>(ud)->Peek(address, num_bytes);
}

std::string ErrorMessage(int code) {
  if (code == RC_INVALID_LUA_OPERAND) return "Lua operands are not supported";
  return rc_error_str(code);
}

}  // namespace

struct ConditionEvaluator::Impl {
  struct Entry {
    std::string key;
    std::string condition;
    // rcheevos parses into one caller-owned block; max_align_t keeps it aligned.
    std::vector<std::max_align_t> buffer;
    rc_trigger_t* trigger = nullptr;
    uint8_t last_state = RC_TRIGGER_STATE_WAITING;
    uint32_t last_measured = kMeasuredUnknown;
  };
  std::vector<Entry> entries;

  Entry* Find(std::string_view key) {
    for (Entry& e : entries)
      if (e.key == key) return &e;
    return nullptr;
  }
};

ConditionEvaluator::ConditionEvaluator() : impl_(std::make_unique<Impl>()) {}
ConditionEvaluator::~ConditionEvaluator() = default;
ConditionEvaluator::ConditionEvaluator(ConditionEvaluator&&) noexcept = default;
ConditionEvaluator& ConditionEvaluator::operator=(ConditionEvaluator&&) noexcept = default;

int ConditionEvaluator::Validate(std::string_view condition, std::string* message) {
  const std::string text(condition);
  const int size = rc_trigger_size(text.c_str());
  if (size >= 0) return 0;
  if (message) *message = ErrorMessage(size);
  return size;
}

bool ConditionEvaluator::Add(std::string key, std::string_view condition, ConditionError* error,
                             bool require_false_first) {
  Impl::Entry entry;
  entry.key = std::move(key);
  entry.condition = std::string(condition);
  int size = entry.condition.empty() ? RC_INVALID_STATE : rc_trigger_size(entry.condition.c_str());
  if (size >= 0) {
    entry.buffer.resize((static_cast<size_t>(size) + sizeof(std::max_align_t) - 1) /
                            sizeof(std::max_align_t) +
                        1);
    entry.trigger = rc_parse_trigger(entry.buffer.data(), entry.condition.c_str(), nullptr, 0);
    if (!entry.trigger) size = RC_INVALID_STATE;
  }
  if (size < 0 || !entry.trigger) {
    if (error) {
      error->key = entry.key;
      error->condition = entry.condition;
      error->code = size;
      error->message = entry.condition.empty() ? "empty condition" : ErrorMessage(size);
    }
    return false;
  }
  if (!require_false_first) entry.trigger->state = RC_TRIGGER_STATE_ACTIVE;
  entry.last_state = entry.trigger->state;

  if (Impl::Entry* existing = impl_->Find(entry.key)) {
    *existing = std::move(entry);
  } else {
    impl_->entries.push_back(std::move(entry));
  }
  return true;
}

void ConditionEvaluator::Remove(std::string_view key) {
  auto& entries = impl_->entries;
  entries.erase(std::remove_if(entries.begin(), entries.end(),
                               [&](const Impl::Entry& e) { return e.key == key; }),
                entries.end());
}

void ConditionEvaluator::Clear() { impl_->entries.clear(); }

bool ConditionEvaluator::Contains(std::string_view key) const {
  return impl_->Find(key) != nullptr;
}

size_t ConditionEvaluator::size() const { return impl_->entries.size(); }

void ConditionEvaluator::Reset(std::string_view key) {
  if (Impl::Entry* e = impl_->Find(key)) {
    rc_reset_trigger(e->trigger);
    e->last_state = e->trigger->state;
    e->last_measured = kMeasuredUnknown;
  }
}

void ConditionEvaluator::ResetAll() {
  for (Impl::Entry& e : impl_->entries) {
    rc_reset_trigger(e.trigger);
    e.last_state = e.trigger->state;
    e.last_measured = kMeasuredUnknown;
  }
}

void ConditionEvaluator::Update(const GuestMemory& memory, const EventFn& fn) {
  void* ud = const_cast<GuestMemory*>(&memory);
  for (Impl::Entry& e : impl_->entries) {
    rc_trigger_t* t = e.trigger;
    if (t->state == RC_TRIGGER_STATE_TRIGGERED) continue;

    const int result = rc_evaluate_trigger(t, RcPeek, ud, nullptr);
    const uint8_t previous = e.last_state;
    e.last_state = t->state;
    if (!fn) {
      e.last_measured = t->measured_value;
      continue;
    }

    if (t->measured_target != 0 && t->measured_value != kMeasuredUnknown &&
        t->measured_value != e.last_measured) {
      fn(Event{EventKind::kMeasured, e.key, std::min(t->measured_value, t->measured_target),
               t->measured_target, t->measured_as_percent != 0});
    }
    e.last_measured = t->measured_value;

    if (result == RC_TRIGGER_STATE_RESET) fn(Event{EventKind::kReset, e.key});
    if (result == RC_TRIGGER_STATE_TRIGGERED) {
      if (previous == RC_TRIGGER_STATE_PRIMED) fn(Event{EventKind::kUnprimed, e.key});
      fn(Event{EventKind::kTriggered, e.key});
      continue;
    }
    if (t->state == RC_TRIGGER_STATE_PRIMED && previous != RC_TRIGGER_STATE_PRIMED) {
      fn(Event{EventKind::kPrimed, e.key});
    } else if (t->state != RC_TRIGGER_STATE_PRIMED && previous == RC_TRIGGER_STATE_PRIMED) {
      fn(Event{EventKind::kUnprimed, e.key});
    }
  }
}

TriggerState ConditionEvaluator::state(std::string_view key) const {
  const Impl::Entry* e = impl_->Find(key);
  return e ? static_cast<TriggerState>(e->trigger->state) : TriggerState::kInactive;
}

uint32_t ConditionEvaluator::measured_value(std::string_view key) const {
  const Impl::Entry* e = impl_->Find(key);
  if (!e || e->trigger->measured_value == kMeasuredUnknown) return 0;
  return e->trigger->measured_value;
}

uint32_t ConditionEvaluator::measured_target(std::string_view key) const {
  const Impl::Entry* e = impl_->Find(key);
  return e ? e->trigger->measured_target : 0;
}

bool ConditionEvaluator::measured_as_percent(std::string_view key) const {
  const Impl::Entry* e = impl_->Find(key);
  return e && e->trigger->measured_as_percent != 0;
}

std::string_view ConditionEvaluator::condition(std::string_view key) const {
  const Impl::Entry* e = impl_->Find(key);
  return e ? std::string_view(e->condition) : std::string_view();
}

std::vector<std::string> ConditionEvaluator::keys() const {
  std::vector<std::string> out;
  out.reserve(impl_->entries.size());
  for (const Impl::Entry& e : impl_->entries) out.push_back(e.key);
  return out;
}

}  // namespace recomp::achievements
