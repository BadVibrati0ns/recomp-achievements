#include "demo_library.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <functional>
#include <map>
#include <memory>

#include "recomp/achievements/library.h"
#include "recomp/achievements/system.h"

namespace recomp::achievements::demo {

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kHour = 3600, kDay = 24 * kHour;
constexpr float kPi = 3.14159265f;

// ---- Generated icons ------------------------------------------------------------
// "gen:<hue>:<shape>[:<variant>]": a coloured tile with a light shape on it.

struct Pt {
  float x, y;
};

bool InPolygon(const std::vector<Pt>& poly, float x, float y) {
  bool in = false;
  for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
    if ((poly[i].y > y) != (poly[j].y > y) &&
        x < (poly[j].x - poly[i].x) * (y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
      in = !in;
  }
  return in;
}

std::vector<Pt> Star(int points, float outer, float inner, float rot) {
  std::vector<Pt> p;
  for (int i = 0; i < points * 2; ++i) {
    const float a = rot - kPi / 2 + i * kPi / points;
    const float r = i % 2 ? inner : outer;
    p.push_back({std::cos(a) * r, std::sin(a) * r});
  }
  return p;
}

// Is (x, y), in -1..1 tile space, inside the shape?
bool Inside(const std::string& shape, int variant, float x, float y) {
  const float r = std::sqrt(x * x + y * y);
  const float rot = variant * 0.35f;
  const float cx = x * std::cos(rot) + y * std::sin(rot), cy = -x * std::sin(rot) + y * std::cos(rot);
  if (shape == "circle") return r < 0.5f;
  if (shape == "ring") return r > 0.34f && r < 0.56f;
  if (shape == "diamond") return std::fabs(cx) + std::fabs(cy) < 0.62f;
  if (shape == "square") return std::max(std::fabs(cx), std::fabs(cy)) < 0.4f;
  if (shape == "hex") return std::max(std::fabs(x) * 0.866f + std::fabs(y) * 0.5f, std::fabs(y)) < 0.5f;
  static const std::vector<Pt> kStar = Star(5, 0.62f, 0.27f, 0), kSpark = Star(4, 0.64f, 0.18f, 0);
  if (shape == "star") return InPolygon(kStar, x, y);
  if (shape == "spark") return InPolygon(kSpark, cx, cy);
  if (shape == "triangle") return InPolygon({{0, -0.55f}, {0.58f, 0.45f}, {-0.58f, 0.45f}}, cx, cy);
  if (shape == "chevron")
    return InPolygon({{-0.5f, -0.45f}, {-0.15f, -0.45f}, {0.35f, 0}, {-0.15f, 0.45f}, {-0.5f, 0.45f}, {0, 0}}, x, y);
  if (shape == "bolt")
    return InPolygon({{0.12f, -0.62f}, {-0.36f, 0.08f}, {-0.02f, 0.08f}, {-0.14f, 0.62f}, {0.36f, -0.1f}, {0.02f, -0.1f}},
                     x, y);
  if (shape == "castle") {
    if (y > 0.5f || y < -0.5f) return false;
    if (std::fabs(x) < 0.5f && y > -0.12f) return !(std::fabs(x) < 0.12f && y > 0.18f);  // Wall and gate.
    if (std::fabs(std::fabs(x) - 0.38f) < 0.14f && y > -0.42f) return true;              // Towers.
    return std::fabs(x) < 0.12f && y > -0.5f;                                               // Keep.
  }
  if (shape == "mountain")
    return InPolygon({{-0.62f, 0.45f}, {-0.15f, -0.35f}, {0.05f, 0.0f}, {0.25f, -0.5f}, {0.62f, 0.45f}}, x, y);
  if (shape == "wheel") {
    if (r > 0.42f && r < 0.6f) return true;
    if (r < 0.14f) return true;
    const float a = std::atan2(cy, cx);
    return r < 0.45f && std::fabs(std::sin(a * 3.0f)) < 0.14f / std::max(r, 0.05f);
  }
  if (shape == "leaf") {
    const float d1 = std::hypot(x - 0.32f, y + 0.32f), d2 = std::hypot(x + 0.32f, y - 0.32f);
    return d1 < 0.68f && d2 < 0.68f;
  }
  if (shape == "planet") {
    if (r < 0.34f) return true;
    const float ex = cx / 0.68f, ey = cy / 0.2f;
    const float e = ex * ex + ey * ey;
    return e > 0.75f && e < 1.0f && !(cy < 0 && r < 0.4f);
  }
  return r < 0.5f;
}

void Hsv(float h, float s, float v, float out[3]) {
  h = std::fmod(h, 360.0f) / 60.0f;
  const float c = v * s, x = c * (1 - std::fabs(std::fmod(h, 2.0f) - 1)), m = v - c;
  float r = 0, g = 0, b = 0;
  if (h < 1) r = c, g = x;
  else if (h < 2) r = x, g = c;
  else if (h < 3) g = c, b = x;
  else if (h < 4) g = x, b = c;
  else if (h < 5) r = x, b = c;
  else r = c, b = x;
  out[0] = r + m, out[1] = g + m, out[2] = b + m;
}

bool MakeIcon(std::string_view ref, LibraryIcon& out) {
  if (!ref.starts_with("gen:")) return false;
  std::vector<std::string> parts;
  std::string cur;
  for (char c : ref.substr(4)) {
    if (c == ':') parts.push_back(cur), cur.clear();
    else cur += c;
  }
  parts.push_back(cur);
  if (parts.size() < 2) return false;
  const float hue = static_cast<float>(std::atof(parts[0].c_str()));
  const std::string& shape = parts[1];
  const bool game = parts.size() > 2 && parts[2] == "game";
  const int variant = parts.size() > 2 && !game ? std::atoi(parts[2].c_str()) : 0;
  const int n = game ? 256 : 128;
  out.width = out.height = n;
  out.rgba.assign(static_cast<size_t>(n) * n * 4, 255);
  float top[3], bottom[3], ink[3];
  Hsv(hue + variant * 9.0f, 0.62f, game ? 0.42f : 0.5f, top);
  Hsv(hue + 24.0f + variant * 9.0f, 0.78f, game ? 0.16f : 0.24f, bottom);
  Hsv(hue - 12.0f, 0.18f, 1.0f, ink);
  const int ss = 4;  // Supersampling per axis.
  for (int py = 0; py < n; ++py) {
    for (int px = 0; px < n; ++px) {
      int hits = 0;
      for (int sy = 0; sy < ss; ++sy)
        for (int sx = 0; sx < ss; ++sx) {
          const float x = ((px + (sx + 0.5f) / ss) / n) * 2 - 1, y = ((py + (sy + 0.5f) / ss) / n) * 2 - 1;
          hits += Inside(shape, variant, x * (game ? 1.15f : 1.0f), y * (game ? 1.15f : 1.0f));
        }
      const float cov = static_cast<float>(hits) / (ss * ss);
      const float t = static_cast<float>(py) / n;
      // A soft light from the top left.
      const float glow = std::max(0.0f, 1.0f - std::hypot(px / float(n) - 0.25f, py / float(n) - 0.2f) * 1.6f) * 0.18f;
      uint8_t* p = &out.rgba[(static_cast<size_t>(py) * n + px) * 4];
      for (int c = 0; c < 3; ++c) {
        const float bg = top[c] + (bottom[c] - top[c]) * t + glow;
        const float v = bg + (ink[c] - bg) * cov * 0.94f;
        p[c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
      }
      p[3] = 255;
    }
  }
  return true;
}

bool IconSource(std::string_view ref, std::string_view, const fs::path&, LibraryIcon& out) {
  return MakeIcon(ref, out);
}

// ---- The games ----------------------------------------------------------------

Achievement A(std::string id, std::string name, std::string desc, uint32_t points, std::string category = {}) {
  Achievement a;
  a.local_id = std::move(id);
  a.name = std::move(name);
  a.description = std::move(desc);
  a.points = points;
  a.category = std::move(category);
  return a;
}

Achievement Trophy(uint32_t id, std::string local, std::string name, std::string desc, Tier tier) {
  Achievement a = A(std::move(local), std::move(name), std::move(desc), 0);
  a.platform_id = id;
  a.tier = tier;
  return a;
}

Achievement& Checklist(Achievement& a, std::string event, std::vector<std::string> items) {
  a.trigger.type = "checklist";
  a.trigger.event = std::move(event);
  a.checklist = std::move(items);
  return a;
}

void Icons(AchievementSet& set, int hue, const std::vector<std::string>& shapes) {
  for (size_t i = 0; i < set.achievements.size(); ++i)
    set.achievements[i].icon =
        "gen:" + std::to_string(hue) + ":" + shapes[i % shapes.size()] + ":" + std::to_string(i % 5);
}

// One timed thing a demo session does.
struct Act {
  uint64_t ago;  // Seconds before `now`.
  std::string profile;
  std::string kind;  // "unlock", "event", "stat"
  std::string key = {}, target = {};
  double value = 0;
};

struct Game {
  std::string id, title, version, icon;
  std::vector<AchievementSet> sets;
  std::vector<Act> acts;  // Oldest first.
  uint64_t last_played_ago = 0;
};

const Credit kAuthors{"The recomp-achievements authors", "demo data", ""};

Game NeonDrift() {
  Game g{"neon_drift", "Neon Drift: Midnight Run", "1.0.3", "gen:300:chevron:game", {}, {}, 2 * kHour};
  AchievementSet s;
  s.id = "neon_drift";
  s.title = "Neon Drift";
  s.platform = Platform::kXbox360;
  s.credits = {kAuthors};
  const uint32_t pts[] = {10, 15, 20, 25, 50, 75, 100, 100, 125, 150, 150, 180};
  const char* names[] = {"First Gear", "Drift King", "Clean Sweep", "Night Owl", "Overtaker", "Photo Finish",
                         "Chrome Collector", "Untouchable", "Garage Full", "Midnight Legend", "Full Throttle",
                         "Neon Royalty"};
  const char* descs[] = {"Finish your first street race.",
                         "Hold a single drift for 10 seconds.",
                         "Win a race without touching a wall.",
                         "Race every district after midnight.",
                         "Pass 50 rivals in one night.",
                         "Win by less than a tenth of a second.",
                         "Collect every chrome rim set.",
                         "Finish a cup without a scratch.",
                         "Own 25 cars at once.",
                         "Win the Midnight Cup.",
                         "Reach 300 km/h on the coast road.",
                         "Finish every event in the career."};
  for (int i = 0; i < 12; ++i) {
    Achievement a = A("a" + std::to_string(i + 1), names[i], descs[i], pts[i], i < 8 ? "Career" : "Mastery");
    a.platform_id = static_cast<uint32_t>(i + 1);
    s.achievements.push_back(a);
  }
  const char* online[] = {"Lobby Leader", "Crew Up", "Ranked Rookie", "Ghost Racer", "World Tour"};
  const char* online_desc[] = {"Host 10 online races.", "Join a racing crew.", "Win a ranked race.",
                               "Beat a friend's ghost lap.", "Race in every online region."};
  for (int i = 0; i < 5; ++i) {
    Achievement a = A("o" + std::to_string(i + 1), online[i], online_desc[i], 50, "Online");
    a.platform_id = static_cast<uint32_t>(20 + i);
    s.achievements.push_back(a);
  }
  Icons(s, 300, {"chevron", "bolt", "diamond", "star", "spark", "ring"});
  // The servers are gone: the online ones move to a pack that starts off.
  const uint32_t ids[] = {20, 21, 22, 23, 24};
  AchievementSet pack = SplitSet(s, ids, "neon_drift_online", "Neon Drift Online", false);
  g.sets = {s, pack};
  for (int i = 0; i < 12; ++i) {
    const uint64_t ago = i < 11 ? (9 - static_cast<uint64_t>(i) * 3 / 4) * kDay + (12 - i) * kHour : 2 * kHour + 600;
    g.acts.push_back({ago, "Racer", "unlock", "neon_drift:a" + std::to_string(i + 1)});
  }
  return g;
}

Game Starfall() {
  Game g{"starfall_odyssey", "Starfall Odyssey", "2.1", "gen:215:planet:game", {}, {}, 1 * kDay};
  AchievementSet s;
  s.id = "starfall";
  s.title = "Starfall Odyssey Trophies";
  s.platform = Platform::kPlayStation3;
  s.credits = {kAuthors};
  s.achievements = {
      Trophy(0, "t0", "Master of the Stars", "Collect every other trophy.", Tier::kPlatinum),
      Trophy(1, "t1", "Supernova", "Finish the story on the hardest difficulty.", Tier::kGold),
      Trophy(2, "t2", "Event Horizon", "Escape the black hole without losing a crew member.", Tier::kGold),
      Trophy(3, "t3", "Star Map", "Chart every system in the galaxy.", Tier::kGold),
      Trophy(4, "t4", "Cartographer", "Land on every planet of the Lyra cluster.", Tier::kSilver),
      Trophy(5, "t5", "Stargazer", "Collect 50 star fragments.", Tier::kSilver),
      Trophy(6, "t6", "Full Crew", "Recruit every crew member.", Tier::kSilver),
      Trophy(7, "t7", "Nebula Runner", "Cross the Veil Nebula in under three minutes.", Tier::kSilver),
      Trophy(8, "t8", "Peacemaker", "End a war without firing a shot.", Tier::kSilver),
      Trophy(9, "t9", "Collector", "Own every ship hull.", Tier::kSilver),
  };
  const char* bronzes[] = {"Liftoff", "First Contact", "Salvager", "Trader", "Smooth Landing", "Repairs",
                           "Long Haul", "Shipshape", "Moonwalk", "Signal Found", "Asteroid Miner", "Hyperdrive",
                           "Old Friends", "Homecoming"};
  const char* bronze_desc[] = {"Leave the home system.", "Meet an alien species.", "Salvage a derelict ship.",
                               "Sell cargo at three stations.", "Land without a scratch.", "Repair your ship mid-flight.",
                               "Travel 1,000 light years.", "Upgrade every ship system.", "Walk on a moon.",
                               "Decode the first signal.", "Mine 100 asteroids.", "Make your first jump.",
                               "Find the lost colony ship.", "Return home after the war."};
  for (int i = 0; i < 14; ++i)
    s.achievements.push_back(
        Trophy(static_cast<uint32_t>(10 + i), "t" + std::to_string(10 + i), bronzes[i], bronze_desc[i], Tier::kBronze));
  Checklist(s.achievements[4], "land",
            {"Vega", "Sheliak", "Sulafat", "Aladfar", "Alathfar", "Epsilon", "Zeta", "Delta"});
  Achievement& frag = s.achievements[5];
  frag.trigger.type = "count";
  frag.trigger.event = "fragment";
  frag.trigger.threshold = 50;
  frag.trigger.params = {{"distinct", false}};
  s.achievements[1].secret = true;
  s.achievements[1].locked_description = "Some endings are hidden.";
  Icons(s, 215, {"star", "planet", "spark", "ring", "hex", "diamond"});
  g.sets = {s};
  const char* p1[] = {"t10", "t11", "t21", "t12", "t13", "t14", "t6", "t15", "t7", "t3"};
  // A second profile on the same console played a little, early on.
  for (int i = 0; i < 3; ++i)
    g.acts.push_back({(16 - static_cast<uint64_t>(i)) * kDay, "Navigator", "unlock", std::string("starfall:") + p1[i]});
  for (int i = 0; i < 10; ++i)
    g.acts.push_back({(12 - static_cast<uint64_t>(i)) * kDay + 3 * kHour, "Captain", "unlock",
                      std::string("starfall:") + p1[i]});
  for (const char* planet : {"Vega", "Sheliak", "Sulafat", "Aladfar", "Alathfar"})
    g.acts.push_back({2 * kDay, "Captain", "event", "land", planet});
  for (int i = 0; i < 37; ++i) g.acts.push_back({1 * kDay + 3600, "Captain", "event", "fragment", "shard"});
  return g;
}

Game PocketGarden() {
  // A game's own list: categories, checklists, secrets, and no icons at all.
  Game g{"pocket_garden", "Pocket Garden", "0.9.4", "", {}, {}, 4 * kDay};
  AchievementSet s;
  s.id = "pocket_garden";
  s.title = "Pocket Garden";
  s.credits = {kAuthors};
  s.achievements = {
      A("sprout", "First Sprout", "Plant your first seed.", 5, "Growing"),
      A("green_thumb", "Green Thumb", "Water 100 plants.", 15, "Growing"),
      A("harvest", "Harvest Moon", "Harvest under a full moon.", 10, "Growing"),
      A("seeds", "Seed Library", "Plant one of every seed.", 25, "Collections"),
      A("seasons", "Every Season", "Grow something in every season.", 20, "Collections"),
      A("pots", "Potter", "Fill every pot on the balcony.", 10, "Collections"),
      A("snail", "Snail Mail", "Read the letter the snail brought.", 15, "Secrets"),
      A("night", "Night Bloom", "Find the flower that opens at night.", 25, "Secrets"),
      A("rain", "Rain Dance", "Water the garden during a storm.", 10, "Growing"),
      A("gift", "Good Neighbour", "Give flowers to every neighbour.", 15, "Collections"),
  };
  Achievement& water = s.achievements[1];
  water.trigger.type = "stat";
  water.trigger.stat = "plants_watered";
  water.trigger.threshold = 100;
  Checklist(s.achievements[3], "plant",
            {"Sunflower", "Tomato", "Basil", "Lavender", "Strawberry", "Pumpkin", "Mint", "Moonflower"});
  Checklist(s.achievements[4], "season", {"Spring", "Summer", "Autumn", "Winter"});
  Checklist(s.achievements[9], "gift", {"Ada", "Bo", "Cyra", "Dex", "Elm"});
  for (int i : {6, 7}) s.achievements[i].secret = true;
  s.achievements[6].locked_description = "Something slow is on its way.";
  s.achievements[7].locked_description = "Stay up late.";
  g.sets = {s};
  g.acts = {
      {9 * kDay, "Gardener", "unlock", "pocket_garden:sprout"},
      {8 * kDay, "Gardener", "unlock", "pocket_garden:pots"},
      {7 * kDay, "Gardener", "unlock", "pocket_garden:snail"},
      {5 * kDay, "Gardener", "unlock", "pocket_garden:rain"},
      {4 * kDay + kHour, "Gardener", "stat", "plants_watered", "", 64},
  };
  for (const char* seed : {"Sunflower", "Tomato", "Basil", "Lavender", "Strawberry"})
    g.acts.push_back({6 * kDay, "Gardener", "event", "plant", seed});
  for (const char* season : {"Spring", "Summer"}) g.acts.push_back({5 * kDay, "Gardener", "event", "season", season});
  for (const char* n : {"Ada", "Bo", "Cyra"}) g.acts.push_back({4 * kDay + 2 * kHour, "Gardener", "event", "gift", n});
  return g;
}

Game CastleQuest() {
  // A copy of a RetroAchievements-style list: points, badges and credits.
  Game g{"castle_quest_dx", "Castle Quest DX", "1.2", "gen:110:castle:game", {}, {}, 6 * kDay};
  AchievementSet s;
  s.id = "castle_quest_dx";
  s.title = "Castle Quest DX";
  s.platform = Platform::kRetroAchievements;
  s.credits = {{"The recomp-achievements authors", "achievement set (demo)", ""},
               {"The recomp-achievements authors", "badge artwork (demo)", ""}};
  s.source = {{"copied_from", "a RetroAchievements-style list (demo data, not a real set)"}};
  const char* names[] = {"Gatecrasher", "Moat Diver", "Key Keeper", "Torch Bearer", "Dungeon Delver", "Bat Swatter",
                         "Armory", "Royal Guard", "Throne Room", "Bell Ringer", "Secret Stair", "No Damage",
                         "Speed Knight", "Treasure Hoard", "Dragon's Bane", "True Ending"};
  const char* descs[] = {"Open the castle gate.", "Swim the moat without drowning.", "Find all three keys in world 1.",
                         "Light every torch in the east wing.", "Reach the bottom of the dungeon.",
                         "Defeat 50 bats.", "Collect every weapon.", "Beat the guard captain.",
                         "Reach the throne room.", "Ring the tower bell.", "Find the hidden staircase.",
                         "Clear world 2 without taking damage.", "Finish world 3 in under 4 minutes.",
                         "Collect 999 coins.", "Defeat the dragon.", "See the true ending."};
  const uint32_t pts[] = {2, 3, 5, 5, 10, 5, 10, 10, 10, 3, 5, 25, 25, 10, 25, 50};
  for (int i = 0; i < 16; ++i) {
    Achievement a = A("ra" + std::to_string(1000 + i), names[i], descs[i], pts[i]);
    a.platform_id = static_cast<uint32_t>(1000 + i);
    a.trigger.type = "memory";
    a.trigger.params = {{"condition", "0xH" + std::to_string(1200 + i) + "=1"}};
    s.achievements.push_back(a);
  }
  s.achievements[11].credits = {{"The recomp-achievements authors", "logic", ""}};
  s.achievements[15].credits = {{"The recomp-achievements authors", "design", ""}};
  Icons(s, 110, {"castle", "diamond", "star", "ring", "triangle", "spark"});
  g.sets = {s};
  for (int i = 0; i < 14; ++i)
    g.acts.push_back({(13 - static_cast<uint64_t>(i) / 2) * kDay - static_cast<uint64_t>(i) * 600, "Hero", "unlock",
                      "castle_quest_dx:ra" + std::to_string(1000 + (i < 11 ? i : i + 1))});
  return g;
}

Game TurboKart() {
  Game g{"turbo_kart", "Turbo Kart Rally", "1.0", "gen:28:wheel:game", {}, {}, 21 * kDay};
  AchievementSet s;
  s.id = "turbo_kart";
  s.title = "Turbo Kart Rally";
  s.platform = Platform::kXbox360;
  s.credits = {kAuthors};
  const char* names[] = {"Green Light", "Banana Peel", "Shortcut Hunter", "Rocket Start", "Cup Winner", "Mirror Mode",
                         "Kart Mechanic", "Photo Mode", "Rival Beaten", "All Tracks", "Gold Cups", "Time Trialist",
                         "Party Starter", "Coin Rain", "Drift Master", "Legend Cup", "Turbo Max", "Perfect Lap",
                         "Kart Collection", "Champion"};
  for (int i = 0; i < 20; ++i) {
    Achievement a = A("k" + std::to_string(i + 1), names[i], "A Turbo Kart Rally challenge.", 50);
    a.platform_id = static_cast<uint32_t>(i + 1);
    s.achievements.push_back(a);
  }
  Icons(s, 28, {"wheel", "bolt", "star", "triangle", "ring"});
  g.sets = {s};
  g.acts = {{23 * kDay, "Racer", "unlock", "turbo_kart:k1"}, {21 * kDay + 600, "Racer", "unlock", "turbo_kart:k4"}};
  return g;
}

std::vector<Game> AllGames() { return {NeonDrift(), Starfall(), PocketGarden(), CastleQuest(), TurboKart()}; }

fs::path SavesFor(const fs::path& root, const fs::path& saves, const std::string& game) {
  const fs::path base = saves.empty() ? root.parent_path() / (root.filename().string() + "-saves") : saves;
  return base / game;
}

uint64_t UnixNow() { return static_cast<uint64_t>(std::time(nullptr)); }

// The profile the "console" has signed in; the System asks before each unlock.
struct Session {
  std::shared_ptr<std::string> profile = std::make_shared<std::string>("default");
  std::shared_ptr<std::atomic<uint64_t>> now_ms = std::make_shared<std::atomic<uint64_t>>(0);
};

void Apply(System& system, const Act& act) {
  if (act.kind == "unlock") system.Unlock(act.key);
  else if (act.kind == "event") system.ReportEvent(act.key, act.target);
  else if (act.kind == "stat") system.ReportStat(act.key, act.value);
}

LibraryOptions OptionsFor(const Game& g, const fs::path& root) {
  LibraryOptions o;
  o.game_id = g.id;
  o.title = g.title;
  o.version = g.version;
  o.icon = g.icon;
  o.root = root;
  o.wine_host = false;
  o.icon_source = IconSource;
  return o;
}

}  // namespace

std::vector<DemoGameInfo> DemoGames() {
  std::vector<DemoGameInfo> out;
  for (const auto& g : AllGames()) out.push_back({g.id, g.title});
  return out;
}

bool WriteDemoLibrary(const fs::path& root, const fs::path& saves, uint64_t now, std::string* error) {
  if (!now) now = UnixNow();
  std::error_code ec;
  fs::create_directories(root, ec);
  auto games = AllGames();
  for (Game& g : games) {
    std::stable_sort(g.acts.begin(), g.acts.end(), [](const Act& a, const Act& b) { return a.ago > b.ago; });
    const fs::path save_dir = SavesFor(root, saves, g.id);
    fs::remove_all(save_dir, ec);
    fs::create_directories(save_dir, ec);
    Session session;
    if (!g.acts.empty()) *session.profile = g.acts.front().profile;
    System system;
    // Unlock times and library times follow the session's fake clock.
    system.SetClock([t = session.now_ms] { return t->load() / 1000; });
    system.SetStorage(save_dir, [p = session.profile] { return *p; });
    for (const auto& set : g.sets) system.AddSet(set);
    uint64_t start = g.acts.empty() ? g.last_played_ago : g.acts.front().ago;
    session.now_ms->store((now - std::max(start, g.last_played_ago) - kHour) * 1000);
    LibraryOptions options = OptionsFor(g, root);
    options.clock = [t = session.now_ms] { return t->load(); };
    {
      Library library(system, options);
      if (!library.enabled()) {
        if (error) *error = "can't write the library at " + root.string();
        return false;
      }
      uint64_t step = 0;
      for (const Act& act : g.acts) {
        if (act.profile != *session.profile) {
          library.Flush();
          *session.profile = act.profile;
          system.ReloadProgress();
        }
        // Each act a few seconds apart, so events keep their order.
        session.now_ms->store((now - act.ago) * 1000 + (step++ % 50) * 1000);
        Apply(system, act);
      }
      session.now_ms->store((now - g.last_played_ago) * 1000);
      library.Flush();
      if (library.stats().failures) {
        if (error) *error = "writes failed in " + library.dir().string();
        return false;
      }
    }  // The Library writes last_played as the session ends.
  }
  return true;
}

bool PlayDemoGame(const fs::path& root, const std::string& game_id, const std::vector<DemoStep>& steps,
                  const fs::path& saves, const std::string& profile, std::string* error) {
  for (const Game& g : AllGames()) {
    if (g.id != game_id) continue;
    std::string who = profile;
    if (who.empty()) who = g.acts.empty() ? "default" : g.acts.back().profile;
    System system;
    system.SetStorage(SavesFor(root, saves, g.id), [who] { return who; });
    for (const auto& set : g.sets) system.AddSet(set);
    Library library(system, OptionsFor(g, root));
    for (const auto& s : steps) {
      if (s.kind == "unlock") system.Unlock(s.key);
      else system.ReportEvent(s.key, s.target);
    }
    library.Flush();
    return library.stats().failures == 0;
  }
  if (error) *error = "no demo game '" + game_id + "'";
  return false;
}

}  // namespace recomp::achievements::demo
