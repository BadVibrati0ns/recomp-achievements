// Writes a library entry for a fake game, then reads it back the way the
// future all-games app will.
#include <cstdio>
#include "recomp/achievements/library.h"
#include "recomp/achievements/library_reader.h"
#include "recomp/achievements/system.h"
using namespace recomp::achievements;
int main(int argc, char** argv) {
  const std::filesystem::path root = argv[1];
  System system;
  system.SetStorage(root.parent_path() / "saves", [] { return std::string("player1"); });
  system.LoadSetJson(R"({"set": {"id": "base", "title": "Demo Game"}, "achievements": [
      {"id": "a", "name": "First Steps", "points": 10},
      {"id": "b", "name": "Collector", "points": 20, "trigger": {"type": "count", "event": "coin", "threshold": 3}}]})",
                     "base.achievements.json");
  system.LoadSetJson(R"({"set": {"id": "online", "title": "Online", "enabled": false}, "achievements": [
      {"id": "o", "name": "Party Time", "points": 50}]})", "online.achievements.json");
  {
    LibraryOptions o;
    o.game_id = "demo_game";
    o.version = "1.0";
    o.root = root;
    Library library(system, o);
    system.Unlock("base:a");
    system.ReportEvent("coin", "gold");
    system.ReportEvent("coin", "silver");
    library.Flush();
  }
  for (const auto& g : ListLibrary(root)) {
    const auto t = SumLibrary({g});
    std::printf("Library sees: %s (%s)\n", g.title.c_str(), g.id.c_str());
    std::printf("  counted sets only: %u/%u unlocked, %u/%u points\n", t.unlocked, t.count, t.unlocked_points, t.points);
  }
  return 0;
}
