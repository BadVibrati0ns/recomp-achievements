#include "script.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <thread>

namespace recomp::achievements::app {

using ui::OverlayInput;

namespace {

using Setter = std::function<void(OverlayInput&)>;

const std::map<std::string, Setter>& Keys() {
  static const std::map<std::string, Setter> k = [] {
    std::map<std::string, Setter> m = {
        {"up", [](OverlayInput& i) { i.up = true; }},
        {"down", [](OverlayInput& i) { i.down = true; }},
        {"left", [](OverlayInput& i) { i.left = true; }},
        {"right", [](OverlayInput& i) { i.right = true; }},
        {"enter", [](OverlayInput& i) { i.enter = true; }},
        {"return", [](OverlayInput& i) { i.enter = true; }},
        {"space", [](OverlayInput& i) { i.space = true; }},
        {"escape", [](OverlayInput& i) { i.escape = true; }},
        {"esc", [](OverlayInput& i) { i.escape = true; }},
        {"backspace", [](OverlayInput& i) { i.backspace = true; }},
        {"tab", [](OverlayInput& i) { i.tab = true; }},
        {"shifttab", [](OverlayInput& i) { i.tab = i.shift = true; }},
        {"pageup", [](OverlayInput& i) { i.page_up = true; }},
        {"pagedown", [](OverlayInput& i) { i.page_down = true; }},
        {"home", [](OverlayInput& i) { i.home = true; }},
        {"end", [](OverlayInput& i) { i.end = true; }},
    };
    for (int f = 1; f <= 12; ++f) m["f" + std::to_string(f)] = [f](OverlayInput& i) { i.f[f] = true; };
    return m;
  }();
  return k;
}

const std::map<std::string, Setter>& Pads() {
  static const std::map<std::string, Setter> p = {
      {"a", [](OverlayInput& i) { i.pad_a = true; }},         {"b", [](OverlayInput& i) { i.pad_b = true; }},
      {"x", [](OverlayInput& i) { i.pad_x = true; }},         {"y", [](OverlayInput& i) { i.pad_y = true; }},
      {"lb", [](OverlayInput& i) { i.pad_lb = true; }},       {"rb", [](OverlayInput& i) { i.pad_rb = true; }},
      {"start", [](OverlayInput& i) { i.pad_start = true; }}, {"back", [](OverlayInput& i) { i.pad_back = true; }},
      {"up", [](OverlayInput& i) { i.pad_up = true; }},       {"down", [](OverlayInput& i) { i.pad_down = true; }},
      {"left", [](OverlayInput& i) { i.pad_left = true; }},   {"right", [](OverlayInput& i) { i.pad_right = true; }},
  };
  return p;
}

std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

bool Number(const std::string& s, double& out) {
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end && *end == '\0' && !s.empty() && std::isfinite(out);
}

}  // namespace

bool ParseScript(const std::string& text, std::vector<ScriptCommand>& out, std::string* error) {
  out.clear();
  std::istringstream in(text);
  std::string line;
  int n = 0;
  auto fail = [&](const std::string& why) {
    if (error) *error = "line " + std::to_string(n) + ": " + why;
    return false;
  };
  while (std::getline(in, line)) {
    ++n;
    if (const size_t hash = line.find('#'); hash != std::string::npos && Lower(line).rfind("exec", 0) != 0)
      line.resize(hash);
    std::istringstream words(line);
    ScriptCommand c;
    c.line = n;
    if (!(words >> c.op)) continue;
    c.op = Lower(c.op);
    if (c.op == "exec") {
      std::string rest;
      std::getline(words, rest);
      const size_t start = rest.find_first_not_of(" \t");
      if (start == std::string::npos) return fail("exec needs a command");
      c.args.push_back(rest.substr(start));
      out.push_back(std::move(c));
      continue;
    }
    for (std::string w; words >> w;) c.args.push_back(w);
    double v = 0;
    auto num = [&](size_t i) { return i < c.args.size() && Number(c.args[i], v); };
    auto opt_num = [&](size_t i) { return i >= c.args.size() || Number(c.args[i], v); };
    if (c.op == "wait" || c.op == "realtime") {
      if (c.args.size() != 1 || !num(0) || v < 0) return fail(c.op + " <seconds>");
    } else if (c.op == "key") {
      if (c.args.empty() || !Keys().count(Lower(c.args[0])) || !opt_num(1)) return fail("unknown key or bad time");
    } else if (c.op == "pad") {
      if (c.args.empty() || !Pads().count(Lower(c.args[0])) || !opt_num(1)) return fail("unknown pad button or bad time");
    } else if (c.op == "stick") {
      const std::string axis = c.args.empty() ? "" : Lower(c.args[0]);
      if ((axis != "lx" && axis != "ly" && axis != "ry") || !num(1) || !opt_num(2)) return fail("stick lx|ly|ry <value> [seconds]");
    } else if (c.op == "mouse") {
      if (c.args.size() < 2 || !num(0) || !num(1) || (c.args.size() > 2 && Lower(c.args[2]) != "click"))
        return fail("mouse <x> <y> [click]");
    } else if (c.op == "wheel") {
      if (c.args.size() != 1 || !num(0)) return fail("wheel <notches>");
    } else if (c.op == "size") {
      if (c.args.size() != 2 || !num(0) || v < 1 || !num(1) || v < 1) return fail("size <width> <height>");
    } else if (c.op == "scale") {
      if (c.args.size() != 1 || !num(0) || v <= 0) return fail("scale <factor>");
    } else if (c.op == "labels") {
      const std::string l = c.args.empty() ? "" : Lower(c.args[0]);
      if (l != "xbox" && l != "playstation") return fail("labels xbox|playstation");
    } else if (c.op != "poll") {
      return fail("unknown command '" + c.op + "'");
    }
    out.push_back(std::move(c));
  }
  return true;
}

int RunScript(const std::vector<ScriptCommand>& script, float fps, const ScriptTarget& target) {
  const float dt = 1.0f / fps;
  int frames = 0;
  OverlayInput mouse;  // The mouse stays where it was put.
  auto frame = [&](const Setter& set) {
    OverlayInput in;
    in.mouse_present = mouse.mouse_present;
    in.mouse_x = mouse.mouse_x;
    in.mouse_y = mouse.mouse_y;
    if (set) set(in);
    target.frame(dt, in);
    ++frames;
  };
  auto run_for = [&](double seconds, const Setter& set) {
    const int n = std::max(1, static_cast<int>(std::lround(seconds * fps)));
    for (int i = 0; i < n; ++i) frame(set);
  };
  for (const ScriptCommand& c : script) {
    auto arg = [&](size_t i, double fallback) { return i < c.args.size() ? std::strtod(c.args[i].c_str(), nullptr) : fallback; };
    if (c.op == "wait") {
      const int n = static_cast<int>(std::lround(arg(0, 0) * fps));
      for (int i = 0; i < n; ++i) frame(nullptr);
    } else if (c.op == "realtime") {
      const auto start = std::chrono::steady_clock::now();
      const int n = static_cast<int>(std::lround(arg(0, 0) * fps));
      for (int i = 0; i < n; ++i) {
        std::this_thread::sleep_until(start + std::chrono::microseconds(static_cast<int64_t>(i * 1e6 / fps)));
        frame(nullptr);
      }
    } else if (c.op == "key" || c.op == "pad") {
      const Setter set = (c.op == "key" ? Keys() : Pads()).at(Lower(c.args[0]));
      run_for(arg(1, 0.12), set);
      frame(nullptr);  // Let go, so the next press is an edge.
    } else if (c.op == "stick") {
      const std::string axis = Lower(c.args[0]);
      const float v = static_cast<float>(arg(1, 0));
      run_for(arg(2, 0.3), [&](OverlayInput& in) {
        (axis == "lx" ? in.pad_lx : axis == "ly" ? in.pad_ly : in.pad_ry) = v;
      });
      frame(nullptr);
    } else if (c.op == "mouse") {
      mouse.mouse_present = true;
      mouse.mouse_x = static_cast<float>(arg(0, 0));
      mouse.mouse_y = static_cast<float>(arg(1, 0));
      if (c.args.size() > 2) {
        run_for(0.1, [](OverlayInput& in) { in.mouse_left = true; });
      }
      frame(nullptr);
    } else if (c.op == "wheel") {
      const float v = static_cast<float>(arg(0, 0));
      frame([v](OverlayInput& in) { in.mouse_wheel = v; });
    } else if (c.op == "size") {
      if (target.resize) target.resize(static_cast<int>(arg(0, 1)), static_cast<int>(arg(1, 1)));
    } else if (c.op == "scale") {
      if (target.scale) target.scale(static_cast<float>(arg(0, 1)));
    } else if (c.op == "labels") {
      if (target.labels)
        target.labels(Lower(c.args[0]) == "playstation" ? ui::ButtonLabels::kPlayStation : ui::ButtonLabels::kXbox);
    } else if (c.op == "exec") {
      if (std::system(c.args[0].c_str()) != 0) std::fprintf(stderr, "script line %d: command failed\n", c.line);
    } else if (c.op == "poll") {
      if (target.poll) target.poll();
    }
  }
  return frames;
}

}  // namespace recomp::achievements::app
