#include "recomp/achievements/ui/notification_center.h"

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace recomp::achievements::ui {

namespace {

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float EaseOutCubic(float t) { return 1.0f - std::pow(1.0f - t, 3.0f); }
float EaseOutBack(float t) {
  constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
  return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

bool Blank(const std::string& s) {
  return std::all_of(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\n'; });
}

}  // namespace

PopupStrings FormatPopup(const Notification& n) {
  PopupStrings out;
  if (!n.style) return out;
  const PopupStyle& p = n.popup();
  const Style& s = *n.style;
  std::map<std::string, std::string, std::less<>> v;
  v["name"] = n.name;
  v["description"] = n.description;
  v["points"] = n.points ? std::to_string(n.points) : std::string();
  v["tier"] = s.TierName(n.tier);
  v["set"] = n.set_title;
  v["item"] = n.item;
  v["current"] = std::to_string(n.current);
  v["total"] = std::to_string(n.total);
  v["percent"] = n.total ? std::to_string(std::min<uint32_t>(100, n.current * 100 / n.total)) : "0";
  if (n.kind == NotificationKind::kUnlock || n.total == 0) {
    // No step counts for unlocks; drops "[{current}/{total}]" groups.
    v["current"].clear();
    v["total"].clear();
    v["percent"].clear();
  }
  auto line = [&](bool shown, const std::string& fmt) {
    if (!shown) return std::string();
    std::string r = FormatText(fmt, v);
    return Blank(r) ? std::string() : r;
  };
  out.title = line(p.lines.title, p.text.title);
  if (p.uppercase_title) {
    for (char& c : out.title)
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  out.name = line(p.lines.name, p.text.name);
  out.points = line(p.lines.points, p.text.points);
  out.description = line(p.lines.description, p.text.description);
  return out;
}

NotificationCenter::NotificationCenter() = default;

NotificationCenter::~NotificationCenter() { Detach(); }

void NotificationCenter::Detach() {
  std::function<void()> detach;
  {
    std::lock_guard lock(mutex_);
    detach.swap(detach_);
  }
  if (detach) detach();  // Unlocked: System may be mid-callback into us.
}

void NotificationCenter::SetStyleResolver(StyleResolver resolver) {
  std::lock_guard lock(mutex_);
  resolver_ = std::move(resolver);
}

void NotificationCenter::UseStyles(const StyleManager& manager) {
  SetStyleResolver([&manager](std::string_view set_id, Platform platform, const AchievementSet* set) {
    return set ? manager.Resolve(set) : manager.Resolve(set_id, platform);
  });
}

void NotificationCenter::SetSoundHandler(SoundHandler handler) {
  std::lock_guard lock(mutex_);
  sound_handler_ = std::move(handler);
}

std::shared_ptr<const Style> NotificationCenter::ResolveStyle(std::string_view set_id, Platform platform,
                                                              const AchievementSet* set) const {
  StyleResolver resolver;
  {
    std::lock_guard lock(mutex_);
    resolver = resolver_;
  }
  if (resolver) {
    if (auto s = resolver(set_id, platform, set)) return s;
  }
  return std::make_shared<Style>(BuiltinTheme(DefaultThemeFor(platform)));
}

void NotificationCenter::OnUnlock(const UnlockEvent& event) {
  const Achievement* a = event.view.achievement;
  if (!a) return;
  const AchievementSet* set = event.view.set;
  Notification n;
  n.kind = NotificationKind::kUnlock;
  n.key = a->key;
  n.set_id = a->set_id;
  if (set) {
    n.set_title = set->title;
    n.platform = set->platform;
    n.icon_base_dir = set->base_dir;
  }
  n.name = a->name;
  n.description = a->description;
  n.icon = a->icon;
  n.points = a->points;
  n.tier = a->tier;
  n.secret = a->secret;
  n.replay = event.replay;
  n.style = ResolveStyle(n.set_id, n.platform, set);
  Push(std::move(n));
}

void NotificationCenter::OnProgress(const ProgressEvent& event) {
  const Achievement* a = event.view.achievement;
  if (!a) return;
  const AchievementSet* set = event.view.set;
  Notification n;
  n.kind = NotificationKind::kProgress;
  n.key = a->key;
  n.set_id = a->set_id;
  if (set) {
    n.set_title = set->title;
    n.platform = set->platform;
    n.icon_base_dir = set->base_dir;
  }
  n.style = ResolveStyle(n.set_id, n.platform, set);
  n.points = a->points;
  n.tier = a->tier;
  n.secret = a->secret;
  n.item = event.item;
  n.current = event.view.progress_current;
  n.total = event.view.progress_total;
  if (a->secret && !event.view.progress.unlocked) {
    // Don't spoil a secret through its progress popup.
    n.name = n.style->secret_name;
    n.description = !a->locked_description.empty() ? a->locked_description : n.style->secret_description;
    n.icon = a->locked_icon;
  } else {
    n.name = a->name;
    n.description = a->description;
    n.icon = a->icon;
  }
  Push(std::move(n));
}

void NotificationCenter::Push(Notification n) {
  if (!n.style) n.style = ResolveStyle(n.set_id, n.platform, nullptr);
  const Style& style = *n.style;
  std::lock_guard lock(mutex_);
  n.serial = next_serial_++;

  if (n.kind == NotificationKind::kUnlock) {
    if (!unlock_enabled_) return;
    // A waiting step popup for this achievement is stale once it unlocks.
    auto& w = progress_.waiting;
    w.erase(std::remove_if(w.begin(), w.end(), [&](const Notification& p) { return p.key == n.key; }),
            w.end());
    unlocks_.waiting.push_back(std::move(n));
    while (unlocks_.waiting.size() > static_cast<size_t>(std::max(1, style.max_queued))) {
      unlocks_.waiting.pop_front();
      ++dropped_;
    }
    return;
  }

  if (!progress_enabled_ || !style.progress_enabled) return;
  if (style.progress_skip_complete && n.total > 0 && n.current >= n.total) return;
  if (style.progress_coalesce && !n.key.empty()) {
    for (auto& a : progress_.active) {
      if (a.n.key == n.key && a.phase != Phase::kOut) {
        // Update in place and hold again, rather than queueing another card.
        const float in = a.n.popup().timing.in;
        a.n = std::move(n);
        a.elapsed = std::min(a.elapsed, in);
        resound_.push_back(a.n);
        return;
      }
    }
    for (auto& w : progress_.waiting) {
      if (w.key == n.key) {
        w = std::move(n);
        return;
      }
    }
  }
  progress_.waiting.push_back(std::move(n));
  while (progress_.waiting.size() > static_cast<size_t>(std::max(1, style.progress_max_queued))) {
    progress_.waiting.pop_front();
    ++dropped_;
  }
}

void NotificationCenter::Animate(ActivePopup& p, float dt) {
  const PopupStyle& ps = p.n.popup();
  const Timing& t = ps.timing;
  const float e = p.elapsed;
  if (e < t.in) {
    p.phase = Phase::kIn;
    p.phase_t = t.in > 0.0f ? Clamp01(e / t.in) : 1.0f;
  } else if (e < t.in + t.hold) {
    p.phase = Phase::kHold;
    p.phase_t = t.hold > 0.0f ? Clamp01((e - t.in) / t.hold) : 1.0f;
  } else {
    p.phase = Phase::kOut;
    p.phase_t = t.out > 0.0f ? Clamp01((e - t.in - t.hold) / t.out) : 1.0f;
  }

  const float k = p.phase_t;
  p.alpha = 1.0f;
  p.slide = 0.0f;
  p.expand = 1.0f;
  switch (p.phase) {
    case Phase::kIn:
      switch (ps.animation) {
        case Animation::kNone: break;
        case Animation::kFade: p.alpha = k; break;
        case Animation::kSlide:
          p.alpha = Clamp01(k * 1.5f);
          p.slide = 1.0f - EaseOutCubic(k);
          break;
        case Animation::kExpand:
          // The icon shows at once; the card grows out of it.
          p.alpha = Clamp01(k * 3.0f);
          p.expand = EaseOutCubic(k);
          break;
        case Animation::kPop:
          p.alpha = Clamp01(k * 2.0f);
          p.expand = std::max(0.0f, EaseOutBack(k));
          break;
      }
      break;
    case Phase::kHold: break;
    case Phase::kOut:
      switch (ps.animation) {
        case Animation::kNone: p.alpha = k >= 1.0f ? 0.0f : 1.0f; break;
        case Animation::kFade: p.alpha = 1.0f - k; break;
        case Animation::kSlide:
          p.alpha = 1.0f - k;
          p.slide = k * k;
          break;
        case Animation::kExpand:
          p.expand = 1.0f - EaseOutCubic(std::min(1.0f, k * 1.4f));
          p.alpha = k < 0.6f ? 1.0f : Clamp01(1.0f - (k - 0.6f) / 0.4f);
          break;
        case Animation::kPop:
          p.expand = 1.0f - 0.2f * k;
          p.alpha = 1.0f - k;
          break;
      }
      break;
  }

  // Stacked popups glide into the gap left by one that finished.
  const float target = static_cast<float>(p.slot);
  const float rate = 1.0f - std::exp(-dt * 12.0f);  // Frame-rate independent.
  p.stack_pos += (target - p.stack_pos) * rate;
  if (std::abs(p.stack_pos - target) < 0.001f) p.stack_pos = target;
}

void NotificationCenter::Expire(Queue& q, float dt) {
  for (auto& a : q.active) a.elapsed += dt;
  q.active.erase(std::remove_if(q.active.begin(), q.active.end(),
                                [](const ActivePopup& a) { return a.elapsed >= a.n.popup().timing.total(); }),
                 q.active.end());
}

void NotificationCenter::Admit(Queue& q, size_t limit, std::vector<SoundRequest>& sounds) {
  while (!paused_ && !q.waiting.empty() &&
         q.active.size() < std::min(limit, static_cast<size_t>(std::max(1, q.waiting.front().popup().max_visible)))) {
    ActivePopup a;
    a.n = std::move(q.waiting.front());
    q.waiting.pop_front();
    a.slot = static_cast<int>(q.active.size());
    a.stack_pos = static_cast<float>(a.slot);
    const Style& s = *a.n.style;
    if (s.sound.enabled && s.sound.volume > 0.0f) {
      const bool unlock = a.n.kind == NotificationKind::kUnlock;
      sounds.push_back({a.n.kind, a.n.tier, unlock ? s.UnlockSound(a.n.tier) : s.sound.progress,
                        s.sound.volume, a.n});
    }
    q.active.push_back(std::move(a));
  }
}

void NotificationCenter::AnimateQueue(Queue& q, float dt) {
  for (size_t i = 0; i < q.active.size(); ++i) {
    q.active[i].slot = static_cast<int>(i);
    Animate(q.active[i], dt);
  }
}

void NotificationCenter::Update(float dt) {
  if (!(dt >= 0.0f)) dt = 0.0f;  // Also rejects NaN.
  dt = std::min(dt, 1.0f);       // A long hitch (loading) shouldn't skip a popup.
  std::vector<SoundRequest> sounds;
  SoundHandler handler;
  {
    std::lock_guard lock(mutex_);
    Expire(unlocks_, dt);
    Expire(progress_, dt);
    if (single_slot_) {
      // One popup on screen at a time; a waiting unlock goes before progress.
      if (progress_.active.empty()) Admit(unlocks_, 1, sounds);
      if (unlocks_.active.empty() && unlocks_.waiting.empty()) Admit(progress_, 1, sounds);
    } else {
      Admit(unlocks_, SIZE_MAX, sounds);
      Admit(progress_, SIZE_MAX, sounds);
    }
    AnimateQueue(unlocks_, dt);
    AnimateQueue(progress_, dt);
    for (auto& n : resound_) {
      const Style& s = *n.style;
      if (s.sound.enabled && s.sound.volume > 0.0f)
        sounds.push_back({n.kind, n.tier, s.sound.progress, s.sound.volume, std::move(n)});
    }
    resound_.clear();
    handler = sound_handler_;
  }
  // Outside the lock: the handler may be slow or call back into us.
  if (handler) {
    for (const auto& s : sounds) handler(s);
  }
}

std::vector<ActivePopup> NotificationCenter::Active() const {
  std::lock_guard lock(mutex_);
  std::vector<ActivePopup> out;
  out.reserve(unlocks_.active.size() + progress_.active.size());
  out.insert(out.end(), unlocks_.active.begin(), unlocks_.active.end());
  out.insert(out.end(), progress_.active.begin(), progress_.active.end());
  return out;
}

bool NotificationCenter::Busy() const {
  std::lock_guard lock(mutex_);
  return !unlocks_.active.empty() || !unlocks_.waiting.empty() || !progress_.active.empty() ||
         !progress_.waiting.empty() || !resound_.empty();
}

void NotificationCenter::SetSingleSlot(bool single) {
  std::lock_guard lock(mutex_);
  single_slot_ = single;
}

bool NotificationCenter::single_slot() const {
  std::lock_guard lock(mutex_);
  return single_slot_;
}

void NotificationCenter::SetPaused(bool paused) {
  std::lock_guard lock(mutex_);
  paused_ = paused;
}

bool NotificationCenter::paused() const {
  std::lock_guard lock(mutex_);
  return paused_;
}

void NotificationCenter::SetEnabled(NotificationKind kind, bool enabled) {
  std::lock_guard lock(mutex_);
  if (kind == NotificationKind::kUnlock) {
    unlock_enabled_ = enabled;
  } else {
    progress_enabled_ = enabled;
    if (!enabled) progress_.waiting.clear();
  }
}

void NotificationCenter::Clear() {
  std::lock_guard lock(mutex_);
  unlocks_ = {};
  progress_ = {};
  resound_.clear();
}

NotificationCenter::Stats NotificationCenter::stats() const {
  std::lock_guard lock(mutex_);
  Stats s;
  s.queued_unlocks = unlocks_.waiting.size();
  s.queued_progress = progress_.waiting.size();
  s.visible = unlocks_.active.size() + progress_.active.size();
  s.dropped = dropped_;
  return s;
}

}  // namespace recomp::achievements::ui
