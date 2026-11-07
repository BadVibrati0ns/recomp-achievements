#pragma once

// The achievement viewer's toggle (F8, Start + LB), for any host: Overlay
// uses it, PopupUi uses it for the ImGui list, and a game with its own viewer
// can too, so every game opens and closes it the same way (docs/imgui-hosts.md).
//
// Each frame: Update() with the raw input and whether the viewer is open; it
// is true on the frame the toggle fires. Hand the game GameInput() instead of
// the raw input: nothing while the viewer is open (or still latched after
// closing), the toggle key removed, and the chord's buttons held back for
// chord_grace so the game doesn't act on the first half of the chord.

#include <cstdint>
#include <string>

#include "recomp/achievements/ui/overlay.h"
#include "recomp/achievements/ui/style.h"

namespace recomp::achievements::ui {

class ViewerToggle {
 public:
  struct Options {
    std::string key = "F8";        // "F1".."F12" or "none".
    std::string pad = "start+lb";  // Buttons joined by '+' (ParsePadChord), or "none".
    // A chord button pressed on its own is held back from the game this long;
    // released sooner it reaches the game as a one-frame tap, held longer it
    // reaches it late. 0 passes chord buttons straight through.
    float chord_grace = 0.15f;
  };

  ViewerToggle();
  explicit ViewerToggle(Options options);

  // The player's "viewer" style keys ("key", "pad") over the options'; empty
  // or unreadable ones keep the options'. Overlay and PopupUi call it when
  // the styles change.
  void ApplyStyle(const ViewerStyle& viewer);

  // Once per frame, before the viewer reads input. True on the frame the
  // toggle fires: the key goes down, or the chord's last button does (in any
  // order; holding it doesn't repeat).
  bool Update(float dt, const OverlayInput& in, bool open);
  // The viewer just closed (its Back button, or the toggle): everything stays
  // hidden from the game until every key and button is released, so the
  // press that closed it isn't a fresh press for the game.
  void Closed();

  // What the game should see this frame, after Update().
  OverlayInput GameInput(const OverlayInput& raw, bool open) const;
  // True while the game shouldn't get input: open, or latched after closing.
  bool wants_input(bool open) const { return open || release_latch_; }

  // The last Update() that fired was the chord's (not the key's).
  bool fired_by_chord() const { return by_chord_; }
  int key() const { return key_; }  // 1..12, 0 = none.
  uint32_t chord() const { return chord_; }
  const Options& options() const { return options_; }

 private:
  Options options_;
  int key_ = 8;
  uint32_t chord_ = 0;
  uint32_t prev_pad_ = 0, prev_fkeys_ = 0;
  bool chord_held_ = false;
  uint32_t held_back_ = 0;
  float held_back_t_ = 0.0f;
  uint32_t tap_ = 0;
  bool release_latch_ = false;
  bool by_chord_ = false;
};

}  // namespace recomp::achievements::ui
