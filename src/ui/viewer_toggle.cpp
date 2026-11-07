#include "recomp/achievements/ui/viewer_toggle.h"

#include <cmath>

namespace recomp::achievements::ui {

namespace {

uint32_t FKeys(const OverlayInput& in) {
  uint32_t k = 0;
  for (int i = 1; i <= 12; ++i)
    if (in.f[i]) k |= 1u << i;
  return k;
}

bool AnyKey(const OverlayInput& in) {
  return in.up || in.down || in.left || in.right || in.enter || in.space || in.escape || in.backspace ||
         in.page_up || in.page_down || in.home || in.end || in.tab;
}

}  // namespace

ViewerToggle::ViewerToggle() : ViewerToggle(Options{}) {}

ViewerToggle::ViewerToggle(Options options) : options_(std::move(options)) { ApplyStyle({}); }

void ViewerToggle::ApplyStyle(const ViewerStyle& viewer) {
  int key = 0;
  if (!ParseToggleKey(viewer.key.empty() ? options_.key : viewer.key, key)) ParseToggleKey(options_.key, key);
  key_ = key;
  uint32_t chord = 0;
  if (!ParsePadChord(viewer.pad.empty() ? options_.pad : viewer.pad, chord)) ParsePadChord(options_.pad, chord);
  chord_ = chord;
}

bool ViewerToggle::Update(float dt, const OverlayInput& in, bool open) {
  const uint32_t pad = PadButtons(in), fkeys = FKeys(in);
  const uint32_t pad_down = pad & ~prev_pad_, f_down = fkeys & ~prev_fkeys_;
  const bool stick = std::fabs(in.pad_lx) > 0.5f || std::fabs(in.pad_ly) > 0.5f || std::fabs(in.pad_ry) > 0.5f;

  tap_ = 0;
  if (release_latch_ && !open && !pad && !fkeys && !AnyKey(in) && !(in.mouse_present && in.mouse_left) && !stick)
    release_latch_ = false;
  if (!open && options_.chord_grace > 0.0f && chord_) {
    held_back_t_ += dt;
    const uint32_t released = held_back_ & ~pad;
    held_back_ &= pad;
    const uint32_t fresh = pad_down & chord_;
    if (fresh) {
      if (!held_back_) held_back_t_ = 0.0f;
      held_back_ |= fresh;
    }
    if (held_back_ && held_back_t_ >= options_.chord_grace) held_back_ = 0;  // Not a chord: let it through.
    tap_ = released;
  }

  bool toggled = key_ > 0 && (f_down & (1u << key_));
  const bool chord_full = chord_ && (pad & chord_) == chord_;
  const bool chord_fired = chord_full && !chord_held_ && (pad_down & chord_);
  if (toggled || chord_fired) by_chord_ = chord_fired;
  toggled = toggled || chord_fired;
  chord_held_ = chord_full;
  prev_pad_ = pad;
  prev_fkeys_ = fkeys;
  if (toggled) {
    held_back_ = 0;
    tap_ = 0;
  }
  return toggled;
}

void ViewerToggle::Closed() { release_latch_ = true; }

OverlayInput ViewerToggle::GameInput(const OverlayInput& raw, bool open) const {
  if (open || release_latch_) {
    OverlayInput none;
    none.mouse_present = raw.mouse_present;
    none.mouse_x = raw.mouse_x;
    none.mouse_y = raw.mouse_y;
    return none;
  }
  OverlayInput out = raw;
  if (key_ > 0 && key_ < 13) out.f[key_] = false;
  SetPadButtons(out, held_back_, false);
  SetPadButtons(out, tap_, true);
  return out;
}

}  // namespace recomp::achievements::ui
