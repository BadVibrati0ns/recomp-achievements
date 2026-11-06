// Writes the demo achievement library, or plays one demo game for a moment
// (an unlock or an event, at the real clock) like a game running beside the
// library app. See demo_library.h.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "demo_library.h"

using namespace recomp::achievements::demo;

namespace {

int Usage() {
  std::fprintf(stderr,
               "usage: recomp-achievements-demo <library dir> [--saves DIR]\n"
               "       recomp-achievements-demo --play <library dir> <game id> [--saves DIR] [--profile P]\n"
               "                                unlock <key> | event <name> <target> ...\n"
               "games:");
  for (const auto& g : DemoGames()) std::fprintf(stderr, " %s", g.id.c_str());
  std::fprintf(stderr, "\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string saves, profile;
  for (size_t i = 0; i + 1 < args.size();) {
    if (args[i] == "--saves" || args[i] == "--profile") {
      (args[i] == "--saves" ? saves : profile) = args[i + 1];
      args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
    } else {
      ++i;
    }
  }
  std::string error;
  if (!args.empty() && args[0] == "--play") {
    if (args.size() < 5) return Usage();
    std::vector<DemoStep> steps;
    for (size_t i = 3; i < args.size();) {
      if (args[i] == "unlock" && i + 1 < args.size()) {
        steps.push_back({"unlock", args[i + 1], {}});
        i += 2;
      } else if (args[i] == "event" && i + 2 < args.size()) {
        steps.push_back({"event", args[i + 1], args[i + 2]});
        i += 3;
      } else {
        return Usage();
      }
    }
    if (!PlayDemoGame(args[1], args[2], steps, saves, profile, &error)) {
      std::fprintf(stderr, "recomp-achievements-demo: %s\n", error.empty() ? "write failed" : error.c_str());
      return 1;
    }
    return 0;
  }
  if (args.size() != 1 || args[0].starts_with("-")) return Usage();
  if (!WriteDemoLibrary(args[0], saves, 0, &error)) {
    std::fprintf(stderr, "recomp-achievements-demo: %s\n", error.c_str());
    return 1;
  }
  std::printf("Wrote %zu demo games to %s\n", DemoGames().size(), args[0].c_str());
  return 0;
}
