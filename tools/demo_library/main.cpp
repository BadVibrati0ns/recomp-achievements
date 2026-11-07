// Writes the demo achievement library, or plays one demo game for a moment
// (an unlock or an event, at the real clock) like a game running beside the
// library app. See demo_library.h.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
               "       recomp-achievements-demo --run <library dir> <game id> [--every S] [--linger S] [steps]\n"
               "                                the game running: steps S seconds apart (default 2.5),\n"
               "                                then open for --linger seconds (default 20)\n"
               "games:");
  for (const auto& g : DemoGames()) std::fprintf(stderr, " %s", g.id.c_str());
  std::fprintf(stderr, "\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  // Each demo game's play.sh runs this binary again.
#if defined(__linux__)
  SetDemoLauncher(std::filesystem::read_symlink("/proc/self/exe"));
#else
  SetDemoLauncher(argv[0]);
#endif
  std::string saves, profile, every = "2.5", linger = "20";
  for (size_t i = 0; i + 1 < args.size();) {
    if (args[i] == "--saves" || args[i] == "--profile" || args[i] == "--every" || args[i] == "--linger") {
      (args[i] == "--saves" ? saves : args[i] == "--profile" ? profile : args[i] == "--every" ? every : linger) = args[i + 1];
      args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
    } else {
      ++i;
    }
  }
  std::string error;
  if (!args.empty() && (args[0] == "--play" || args[0] == "--run")) {
    const bool run = args[0] == "--run";
    if (args.size() < (run ? 3u : 5u)) return Usage();
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
    const bool ok = run ? RunDemoGame(args[1], args[2], steps, std::atof(every.c_str()), std::atof(linger.c_str()), saves,
                                      profile, &error)
                        : PlayDemoGame(args[1], args[2], steps, saves, profile, &error);
    if (!ok) {
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
