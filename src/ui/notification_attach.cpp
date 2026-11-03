// Kept apart from notification_center.cpp so code that feeds events directly
// (and the tests) doesn't pull in System.

#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/notification_center.h"

namespace recomp::achievements::ui {

void NotificationCenter::Attach(System& system) {
  Detach();
  const int unlock = system.AddUnlockListener([this](const UnlockEvent& e) { OnUnlock(e); });
  const int progress = system.AddProgressListener([this](const ProgressEvent& e) { OnProgress(e); });
  std::lock_guard lock(mutex_);
  detach_ = [&system, unlock, progress] {
    system.RemoveListener(unlock);
    system.RemoveListener(progress);
  };
}

}  // namespace recomp::achievements::ui
