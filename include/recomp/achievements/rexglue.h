#pragma once

// Binding for rexglue (Xbox 360) recomps. With rexglue built with
// patches/rexglue-achievements.patch (CMake RECOMP_ACH_REXGLUE_PATCHED, the
// default), it hooks the kernel achievement callbacks that patch adds:
//   XGIUserWriteAchievements          -> System::Unlock(set_id, id)
//   XamUserCreateAchievementEnumerator <- System's unlocked state
//   XamShowAchievementsUI(Ex)/DetailsUI -> options.on_show_achievements
// With stock rexglue (RECOMP_ACH_REXGLUE_PATCHED=OFF) nothing is hooked; the
// recomp's own XAM shims pass those calls to xam() instead.
// It also builds the title's AchievementSet from the XEX's XDBF plus the best
// title update / content spa.bin, and serves its images for "xdbf:<id>" icons.
// See docs/xbox360.md.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/sources/xam.h"
#include "recomp/achievements/sources/xdbf.h"

namespace rex::system {
class KernelState;
}

namespace recomp::achievements {

class System;

struct RexglueOptions {
  // XLanguage for names; 0 = rexglue's user_language cvar.
  uint32_t language = 0;
  // Build the set from XDBF and AddSet it. Off: the game provides the set
  // itself (e.g. a JSON file) and only the kernel callbacks are wired, by
  // platform id.
  bool add_set = true;
  // Merge the best spa.bin found under content_roots over the XEX's XDBF.
  bool load_title_update = true;
  // Roots holding 0000000000000000/<title id>/... (rexglue's content root).
  // Empty: the working directory and <working directory>/game.
  std::vector<std::filesystem::path> content_roots;
  std::string category;  // Default category for the built set.
  std::string title;     // Overrides the XDBF title name.

  // Game opened the dashboard achievement list.
  std::function<void(uint32_t user_index, uint32_t title_id)> on_show_achievements;
  // Game wrote an achievement id the set doesn't have.
  std::function<void(uint32_t user_index, uint32_t achievement_id)> on_unknown_achievement;
};

class RexglueBinding {
 public:
  ~RexglueBinding();  // Unhooks the kernel callbacks.
  RexglueBinding(const RexglueBinding&) = delete;
  RexglueBinding& operator=(const RexglueBinding&) = delete;

  // Clears the kernel callbacks. Call before the System is destroyed if the
  // binding outlives it.
  void Unbind();
  // Re-reads the XDBF and spa.bin and rebuilds the set (e.g. after a TU was
  // installed, or when bound before the executable module was loaded).
  bool Reload();

  const xbox::XdbfTitle& title() const;
  const std::filesystem::path& title_update_path() const;
  const std::string& set_id() const;
  uint32_t title_id() const;
  // The XAM calls on raw guest data, for recomps that shim XAM themselves.
  // Wired to the options' handlers.
  xbox::XamAchievements& xam();
  // True when the kernel callbacks are hooked (patched rexglue).
  bool hooks_kernel() const;
  // PNG bytes for "xdbf:<id>"; empty for other refs. Valid until Reload().
  std::span<const uint8_t> ResolveIcon(std::string_view ref) const;

  struct State;

 private:
  friend std::unique_ptr<RexglueBinding> BindRexglue(rex::system::KernelState*, System&,
                                                     std::string, RexglueOptions);
  explicit RexglueBinding(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
};

// Call once the executable module is loaded (title_xdbf() reads it). Returns
// null when kernel_state is null.
std::unique_ptr<RexglueBinding> BindRexglue(rex::system::KernelState* kernel_state, System& system,
                                            std::string set_id, RexglueOptions options = {});

}  // namespace recomp::achievements
