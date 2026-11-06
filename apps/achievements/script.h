#pragma once

// Scripted input for the app's headless mode (--script): one command per
// line, run one after another at a fixed frame rate, so a run is the same
// every time and can be recorded without a display.
//
//   # comment
//   wait 1.5                 let 1.5 s pass
//   key Down [0.3]           hold a key (Up Down Left Right Enter Space Escape
//                            Backspace Tab ShiftTab PageUp PageDown Home End
//                            F1..F12) for 0.3 s (default 0.12), then let go
//   pad a [0.3]              the same for a pad button (a b x y lb rb start
//                            back up down left right)
//   stick ly 0.8 [0.5]       hold a stick axis (lx ly ry), -1..1
//   mouse 640 400 [click]    move the mouse (window pixels), maybe click
//   wheel -2                 scroll the wheel, notches (+ = away from you)
//   size 1280 800            resize the window
//   scale 1.25               the app's scale option
//   labels playstation       button labels (xbox, playstation)
//   exec <command>           run a shell command and wait for it, e.g. a demo
//                            game writing an unlock
//   poll                     look at the library now

#include <functional>
#include <string>
#include <vector>

#include "recomp/achievements/ui/overlay.h"

namespace recomp::achievements::app {

struct ScriptCommand {
  int line = 0;
  std::string op;
  std::vector<std::string> args;
};

// False with a message naming the line on the first bad command.
bool ParseScript(const std::string& text, std::vector<ScriptCommand>& out, std::string* error);

// What a script drives; the runner calls these.
struct ScriptTarget {
  std::function<void(float dt, const ui::OverlayInput& in)> frame;  // One frame.
  std::function<void(int w, int h)> resize;
  std::function<void(float scale)> scale;
  std::function<void(ui::ButtonLabels labels)> labels;
  std::function<void()> poll;
};

// Runs every command at `fps`; returns the number of frames.
int RunScript(const std::vector<ScriptCommand>& script, float fps, const ScriptTarget& target);

}  // namespace recomp::achievements::app
