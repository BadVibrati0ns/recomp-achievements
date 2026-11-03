#pragma once

// Plays popup sounds through SDL3 audio streams. Built only into
// recomp::achievements_ui; without RECOMP_ACH_SDL3_TARGET every call is a
// no-op and available() is false, so games can use it unconditionally.

#include <memory>
#include <string_view>

#include "recomp/achievements/ui/notification_center.h"
#include "recomp/achievements/ui/sounds.h"

namespace recomp::achievements::ui {

class SoundPlayer {
 public:
  SoundPlayer();
  ~SoundPlayer();
  SoundPlayer(const SoundPlayer&) = delete;
  SoundPlayer& operator=(const SoundPlayer&) = delete;

  // True when built with SDL3 (playback may still fail without a device).
  static bool available();

  // The bank has SDL_LoadWAV installed when SDL3 is present.
  SoundBank& bank();

  // Routes the center's popup sounds here. Call Detach or destroy the center first.
  void Attach(NotificationCenter& center);

  // Unlock and progress sounds use separate streams so a tick never cuts off
  // an unlock; a new sound on the same stream replaces the old one.
  bool Play(const NotificationCenter::SoundRequest& request);
  bool Play(std::string_view ref, float volume = 1.0f, NotificationKind channel = NotificationKind::kUnlock);
  bool Play(const Pcm& pcm, float volume, NotificationKind channel);

  void SetMasterVolume(float volume);  // Multiplies the style's volume.
  float master_volume() const;
  void SetMuted(bool muted);
  bool muted() const;
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace recomp::achievements::ui
