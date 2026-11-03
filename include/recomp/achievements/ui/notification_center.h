#pragma once

// Renderer-agnostic popup queues. Events can arrive from any thread (System
// listeners fire on the game thread); Update() and Active() belong to the
// render thread. Nothing here depends on ImGui or SDL, so a game with its own
// renderer can draw Active() itself.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/system.h"
#include "recomp/achievements/types.h"
#include "recomp/achievements/ui/style.h"

namespace recomp::achievements::ui {

enum class NotificationKind { kUnlock, kProgress };

// A popup's content, copied out of the event so it outlives the set.
struct Notification {
  NotificationKind kind = NotificationKind::kUnlock;
  uint64_t serial = 0;  // Unique per center, in arrival order.
  std::string key;
  std::string set_id;
  std::string set_title;
  Platform platform = Platform::kCustom;
  std::string name;
  std::string description;
  std::string icon;  // Icon ref (locked_icon for locked progress popups).
  std::filesystem::path icon_base_dir;
  uint32_t points = 0;
  Tier tier = Tier::kNone;
  bool secret = false;
  bool replay = false;  // TestPopup().
  // Progress popups.
  std::string item;
  uint32_t current = 0;
  uint32_t total = 0;

  std::shared_ptr<const Style> style;  // Resolved when queued.

  const PopupStyle& popup() const {
    return kind == NotificationKind::kUnlock ? style->unlock : style->progress;
  }
};

enum class Phase { kIn, kHold, kOut };

// One visible popup with its animation state, recomputed by Update().
struct ActivePopup {
  Notification n;
  Phase phase = Phase::kIn;
  float elapsed = 0.0f;  // Seconds since it appeared.
  float phase_t = 0.0f;  // 0..1 within the phase.
  // Eased values for the style's animation:
  float alpha = 0.0f;   // 0..1 opacity.
  float slide = 0.0f;   // 0..1 fraction of slide_distance away from rest (in or out).
  float expand = 1.0f;  // 0..1 card width for kExpand / scale for kPop.
  // Smoothed stack position (0 = nearest the anchor); eases toward its slot.
  float stack_pos = 0.0f;
  int slot = 0;
};

// Formats the popup's lines with its style's text formats.
struct PopupStrings {
  std::string title, name, points, description;
};
PopupStrings FormatPopup(const Notification& n);

class NotificationCenter {
 public:
  // Resolves the style for a set; default: built-in theme by platform. set
  // is null for notifications pushed by hand.
  using StyleResolver = std::function<std::shared_ptr<const Style>(
      std::string_view set_id, Platform platform, const AchievementSet* set)>;
  // Called from Update() when a popup appears. sound is the style's ref for it.
  struct SoundRequest {
    NotificationKind kind;
    Tier tier;
    std::string sound;
    float volume;
    Notification notification;
  };
  using SoundHandler = std::function<void(const SoundRequest&)>;

  NotificationCenter();
  ~NotificationCenter();
  NotificationCenter(const NotificationCenter&) = delete;
  NotificationCenter& operator=(const NotificationCenter&) = delete;

  void SetStyleResolver(StyleResolver resolver);
  // Uses manager.Resolve(); manager must outlive the center.
  void UseStyles(const StyleManager& manager);
  void SetSoundHandler(SoundHandler handler);

  // Subscribes to a System's unlock and progress listeners. The System must
  // outlive the center or Detach() must be called first.
  void Attach(System& system);
  void Detach();

  // Feeding events directly (also what Attach() does).
  void OnUnlock(const UnlockEvent& event);
  void OnProgress(const ProgressEvent& event);
  void Push(Notification n);  // n.style may be null: resolved here.

  // Advances time; promotes queued popups and fires their sounds.
  void Update(float dt);
  // Copy of the visible popups, unlock popups first.
  std::vector<ActivePopup> Active() const;
  bool Busy() const;  // Anything visible or waiting: keep repainting.

  // One popup on screen at a time, for games with a single popup slot:
  // unlocks and progress share it, and a waiting unlock goes before waiting
  // progress. Each kind's max_visible still applies when off (the default).
  void SetSingleSlot(bool single);
  bool single_slot() const;

  // Keeps popups waiting (e.g. while a loading screen or the pause menu is up).
  void SetPaused(bool paused);
  bool paused() const;
  void SetEnabled(NotificationKind kind, bool enabled);
  void Clear();

  struct Stats {
    size_t queued_unlocks = 0, queued_progress = 0;
    size_t visible = 0;
    uint64_t dropped = 0;  // Discarded because a queue was full.
  };
  Stats stats() const;

 private:
  struct Queue {
    std::deque<Notification> waiting;
    std::vector<ActivePopup> active;
  };
  std::shared_ptr<const Style> ResolveStyle(std::string_view set_id, Platform platform,
                                            const AchievementSet* set) const;
  void Expire(Queue& q, float dt);
  void Admit(Queue& q, size_t limit, std::vector<SoundRequest>& sounds);
  static void AnimateQueue(Queue& q, float dt);
  static void Animate(ActivePopup& p, float dt);

  mutable std::mutex mutex_;
  StyleResolver resolver_;
  SoundHandler sound_handler_;
  Queue unlocks_, progress_;
  bool paused_ = false;
  bool single_slot_ = false;
  bool unlock_enabled_ = true, progress_enabled_ = true;
  uint64_t next_serial_ = 1;
  uint64_t dropped_ = 0;
  std::vector<Notification> resound_;  // Coalesced steps that still get a tick.
  // Set by Attach(); a callback so the center itself never links against System.
  std::function<void()> detach_;
};

}  // namespace recomp::achievements::ui
