#include "recomp/achievements/app/app.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <optional>
#include <set>

#include "recomp/achievements/ui/canvas.h"
#include "recomp/achievements/ui/notification_center.h"
#include "recomp/achievements/ui/painter.h"
#include "recomp/achievements/ui/viewer_parts.h"

namespace recomp::achievements::app {

namespace fs = std::filesystem;
using namespace ui;

namespace {

Color WithAlpha(Color c, float a) {
  c.a *= a;
  return c;
}

const char* PlatformName(Platform p) {
  switch (p) {
    case Platform::kXbox360: return "Xbox 360";
    case Platform::kPlayStation3: return "PS3";
    case Platform::kRetroAchievements: return "RetroAchievements";
    case Platform::kCustom: return "Custom";
  }
  return "";
}

Color PlatformColor(Platform p) {
  switch (p) {
    case Platform::kXbox360: return Color::Rgba(78, 160, 46);
    case Platform::kPlayStation3: return Color::Rgba(46, 104, 204);
    case Platform::kRetroAchievements: return Color::Rgba(204, 138, 26);
    case Platform::kCustom: return Color::Rgba(132, 98, 204);
  }
  return {};
}

const char* PointsSuffix(Platform p) { return p == Platform::kXbox360 ? " G" : " pts"; }

std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// Input bits, for edges.
enum Bit : uint32_t {
  kUp = 1u << 0, kDown = 1u << 1, kLeft = 1u << 2, kRight = 1u << 3, kEnter = 1u << 4, kTab = 1u << 5,
  kHome = 1u << 6, kEnd = 1u << 7, kPageUp = 1u << 8, kPageDown = 1u << 9, kF11 = 1u << 10, kEscape = 1u << 11,
  kSpace = 1u << 12,
};

uint32_t Keys(const OverlayInput& in) {
  uint32_t k = 0;
  if (in.up) k |= kUp;
  if (in.down) k |= kDown;
  if (in.left) k |= kLeft;
  if (in.right) k |= kRight;
  if (in.enter) k |= kEnter;
  if (in.tab) k |= kTab;
  if (in.home) k |= kHome;
  if (in.end) k |= kEnd;
  if (in.page_up) k |= kPageUp;
  if (in.page_down) k |= kPageDown;
  if (in.f[11]) k |= kF11;
  if (in.escape) k |= kEscape;
  if (in.space) k |= kSpace;
  return k;
}

bool Inside(const RectF& r, float x, float y) { return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }

// Popups are laid out for a display of height / k and drawn k times larger,
// so they follow the app's scale (at least 1 at 800 lines) like the viewer.
class ScaledPainter : public Painter {
 public:
  ScaledPainter(Painter& inner, float k) : p_(inner), k_(k) {}
  float TextWidth(bool bold, float size, std::string_view t) const override {
    return p_.TextWidth(bold, size * k_, t) / k_;
  }
  void FillRect(Vec2 a, Vec2 b, const Color& c, float r, int corners) override {
    p_.FillRect(S(a), S(b), c, r * k_, corners);
  }
  void StrokeRect(Vec2 a, Vec2 b, const Color& c, float r, float t) override {
    p_.StrokeRect(S(a), S(b), c, r * k_, t * k_);
  }
  void FillCircle(Vec2 c, float r, const Color& col, int seg) override { p_.FillCircle(S(c), r * k_, col, seg); }
  void StrokeCircle(Vec2 c, float r, const Color& col, int seg, float t) override {
    p_.StrokeCircle(S(c), r * k_, col, seg, t * k_);
  }
  void DrawImage(void* tex, Vec2 a, Vec2 b, const Color& tint, float r) override {
    p_.DrawImage(tex, S(a), S(b), tint, r * k_);
  }
  void Text(bool bold, float size, Vec2 pos, const Color& c, std::string_view t) override {
    p_.Text(bold, size * k_, S(pos), c, t);
  }
  void PushClip(Vec2 a, Vec2 b) override { p_.PushClip(S(a), S(b)); }
  void PopClip() override { p_.PopClip(); }

 private:
  Vec2 S(Vec2 v) const { return {v.x * k_, v.y * k_}; }
  Painter& p_;
  float k_;
};

struct RecentItem {
  std::string game, key, name, game_title;
  std::string icon;  // Absolute path; empty: the trophy.
  uint64_t time = 0;
  Tier tier = Tier::kNone;
};

}  // namespace

struct App::Impl {
  AppOptions options;
  std::vector<fs::path> roots;
  bool auto_roots = false;
  int polls = 0;
  float poll_t = 0;
  bool polled = false;
  std::map<std::string, EntryStamp> stamps;
  std::map<std::string, LibraryGame> loaded;   // By game id, with sets.
  std::map<std::string, std::string> chosen;   // Game id -> profile picked on its page.
  std::vector<GameSummary> games;              // Sorted for the grid.
  std::vector<RecentItem> recent;
  uint64_t reloads = 0, popup_count = 0;

  StyleManager styles;
  IconCache icons;
  NotificationCenter center;
  std::shared_ptr<Font> font = Font::Builtin(false), font_bold = Font::Builtin(true);

  int width = 0, height = 0;
  Canvas grid, popups;
  Image out;
  PixelRect dirty, out_dirty;
  bool grid_dirty = true;
  uint64_t drawn_minute = 0;
  ButtonLabels labels = ButtonLabels::kXbox, given_labels = ButtonLabels::kXbox;
  uint64_t style_generation = 0;

  // Grid state.
  SortMode sort = SortMode::kLastPlayed;
  int sel = 0;
  std::string sel_id;
  float scroll = 0, scroll_target = 0, drawn_scroll = -1;
  bool keyboard = true;
  bool fullscreen_request = false;

  // Input edges; buttons held across a screen change stay latched until let go.
  uint32_t prev_pad = 0, prev_keys = 0, latched_pad = 0, latched_keys = 0;
  bool prev_mouse = false;
  float prev_mx = -1, prev_my = -1;
  int repeat_dir = 0;
  float repeat_timer = 0;
  bool stick_x_held = false;

  // Hit areas from the last grid render.
  std::vector<std::pair<RectF, int>> card_hits;
  RectF sort_hits[kSortModes];
  std::vector<std::pair<RectF, std::string>> recent_hits;

  // Launching. The menu offers Steam and starting directly; the choice is
  // remembered per game.
  LauncherPrefs prefs;
  fs::path prefs_file;
  std::set<std::string> running;
  std::map<std::string, float> starting;  // Game id -> seconds left showing "Starting".
  std::vector<std::string> launched;
  struct Menu {
    std::string id, title;
    LaunchPlan plan;
    int choice = -1;  // 0 Steam, 1 direct, -1 neither can start it.
    bool on_quit = false;  // The Quit current game box has the focus.
    std::string error;
    RectF rows[2], quit, play, cancel, box;
  };
  bool QuitBox() const { return menu && options.quit_current && menu->id != options.current_game && !menu->plan.running; }
  std::optional<Menu> menu;
  RectF play_hit;  // The selected card's Play button.

  // The game page: the overlay's viewer over a read-only System.
  struct Page {
    std::string id;
    std::string profile;
    System system;
    std::unique_ptr<Overlay> overlay;
  };
  std::unique_ptr<Page> page;
  PixelRect popup_area;
  bool popups_shown = false;

  explicit Impl(AppOptions o) : options(std::move(o)) {
    auto_roots = options.roots.empty();
    roots = auto_roots ? DefaultLibraryRoots() : options.roots;
    UseImageTextures(icons);
    if (options.global_preferences)
      if (const fs::path dir = GlobalConfigDir(); !dir.empty())
        styles.LoadGlobalStyleFile(dir / "achievements_style.json");
    center.UseStyles(styles);
    prefs_file = options.launcher_prefs.empty() ? DefaultLauncherPrefsFile() : options.launcher_prefs;
    if (!prefs_file.empty()) prefs = LoadLauncherPrefs(prefs_file);
  }

  uint64_t Now() const { return options.now ? options.now() : static_cast<uint64_t>(std::time(nullptr)); }

  // ---- Scale and layout ---------------------------------------------------------

  std::shared_ptr<const Style> AppStyle() const { return styles.Resolve(nullptr); }

  // The window's scale: height / 1080, but never under 1 at 800 lines (a
  // Steam Deck), so text stays readable on small screens.
  float BaseScale() const {
    const float h = static_cast<float>(height);
    return std::max(h / 1080.0f, SmallScreenScale(*AppStyle(), h)) * options.scale;
  }
  float Scale() const { return std::max(0.75f, BaseScale() * AppStyle()->viewer.scale); }

  struct Layout {
    float s = 1, margin = 0;
    float header_y = 0, recent_y = 0, sort_y = 0, sort_h = 0;
    RectF grid;
    float footer_y = 0;
    int columns = 1;
    float card_w = 0, card_h = 0, gap = 0;
    bool recent = false;
  };

  Layout MakeLayout() const {
    Layout l;
    const float s = l.s = Scale();
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    l.margin = std::max(16.0f, 32.0f * s);
    l.header_y = l.margin;
    float y = l.header_y + 66.0f * s;
    l.recent = !recent.empty();
    l.recent_y = y + 8.0f * s;
    if (l.recent) y = l.recent_y + 30.0f * s + 64.0f * s;
    l.sort_y = y + 18.0f * s;
    l.sort_h = 34.0f * s;
    l.footer_y = h - 56.0f * s;
    l.grid = {l.margin, l.sort_y + l.sort_h + 16.0f * s, w - l.margin, l.footer_y - 6.0f * s};
    l.gap = 16.0f * s;
    l.columns = std::max(1, static_cast<int>((l.grid.width() + l.gap) / (380.0f * s + l.gap)));
    l.card_w = (l.grid.width() - (l.columns - 1) * l.gap) / l.columns;
    l.card_h = 170.0f * s;
    return l;
  }

  RectF CardRect(const Layout& l, int i) const {
    const int row = i / l.columns, col = i % l.columns;
    const float x = l.grid.x0 + col * (l.card_w + l.gap);
    const float y = row * (l.card_h + l.gap);
    return {x, y, x + l.card_w, y + l.card_h};  // y in grid space.
  }

  float ContentHeight(const Layout& l) const {
    const int rows = (static_cast<int>(games.size()) + l.columns - 1) / l.columns;
    return rows ? rows * l.card_h + (rows - 1) * l.gap : 0.0f;
  }
  float MaxScroll(const Layout& l) const { return std::max(0.0f, ContentHeight(l) - l.grid.height() + 4.0f * l.s); }

  // ---- The library ---------------------------------------------------------------

  bool Poll() {
    ++polls;
    if (auto_roots && polls % 10 == 0) {
      // New Proton prefixes appear rarely; looking for them is a directory walk.
      auto now_roots = DefaultLibraryRoots();
      if (now_roots != roots) roots = std::move(now_roots), stamps.clear();
    }
    auto now = StampLibrary(roots);
    if (polled && now == stamps) return false;
    const bool first = !polled;
    polled = true;
    const auto before = stamps;
    stamps = std::move(now);
    ++reloads;

    std::map<std::string, LibraryGame> next;
    bool page_sets_changed = false, page_changed = false;
    for (const LibraryGame& listed : ListLibrary(roots)) {
      const fs::path root = listed.root;
      const std::string key = root.string() + "|" + listed.id;
      const auto it = loaded.find(listed.id);
      const auto old_stamp = before.find(key);
      const auto new_stamp = stamps.find(key);
      const bool same = it != loaded.end() && it->second.dir == listed.dir && old_stamp != before.end() &&
                        new_stamp != stamps.end() && old_stamp->second == new_stamp->second;
      if (same) {
        next.emplace(listed.id, std::move(it->second));
        continue;
      }
      auto game = LoadLibraryGame(root, listed.id);
      if (!game) continue;
      if (page && page->id == listed.id) {
        page_changed = true;
        page_sets_changed = it == loaded.end() || old_stamp == before.end() || new_stamp == stamps.end() ||
                            old_stamp->second.sets != new_stamp->second.sets || it->second.dir != listed.dir;
      }
      if (!first && options.popups && it != loaded.end()) QueueChanges(it->second, *game);
      next.emplace(listed.id, std::move(*game));
    }
    loaded = std::move(next);
    Summarize();
    if (page) {
      const auto it = loaded.find(page->id);
      if (it != loaded.end() && page_changed) {
        const LibraryProfile* p = PickProfile(it->second, page->profile);
        MirrorGame(page->system, it->second, p, page_sets_changed);
        page->profile = p ? p->id : std::string();
        UpdateHostButton();
      }
    }
    grid_dirty = true;
    return true;
  }

  void Summarize() {
    games.clear();
    for (const auto& [id, game] : loaded) {
      const auto c = chosen.find(id);
      games.push_back(app::Summarize(game, c == chosen.end() ? std::string() : c->second));
    }
    SortGames(games, sort);
    // Keep the selection on the same game.
    sel = std::clamp(sel, 0, std::max(0, static_cast<int>(games.size()) - 1));
    for (size_t i = 0; i < games.size(); ++i)
      if (games[i].id == sel_id) sel = static_cast<int>(i);
    sel_id = games.empty() ? std::string() : games[sel].id;

    std::vector<LibraryGame> all;
    for (const auto& [id, g] : loaded) all.push_back(g);
    recent.clear();
    for (const LibraryUnlock& u : RecentUnlocks(all, 12)) {
      const LibraryGame& g = loaded.at(u.game);
      RecentItem item{u.game, u.key, u.key, g.title.empty() ? g.id : g.title, {}, u.time, Tier::kNone};
      for (const auto& s : g.sets)
        for (const auto& a : s.set.achievements)
          if (a.key == u.key) {
            item.name = a.name;
            item.icon = a.icon;
            item.tier = a.tier;
          }
      if (item.icon.empty()) item.icon = g.icon.string();
      recent.push_back(std::move(item));
    }
    const Layout l = MakeLayout();
    scroll_target = std::clamp(scroll_target, 0.0f, MaxScroll(l));
    scroll = std::clamp(scroll, 0.0f, MaxScroll(l));
  }

  // Popups for what moved since the last look: new unlocks, and new
  // checklist or count steps.
  void QueueChanges(const LibraryGame& before, const LibraryGame& after) {
    const auto c = chosen.find(after.id);
    const std::string want = c == chosen.end() ? std::string() : c->second;
    const LibraryProfile* p0 = PickProfile(before, want);
    const LibraryProfile* p1 = PickProfile(after, want);
    if (!p1 || (p0 && p0->id != p1->id)) return;
    for (const auto& s : after.sets) {
      if (!s.enabled) continue;
      for (const auto& a : s.set.achievements) {
        const auto now = p1->progress.find(a.key);
        if (now == p1->progress.end()) continue;
        const LibraryProgress* was = nullptr;
        if (p0)
          if (auto it = p0->progress.find(a.key); it != p0->progress.end()) was = &it->second;
        Notification n;
        n.key = a.key;
        n.set_id = s.set.id;
        n.set_title = s.set.title;
        n.platform = s.set.platform;
        n.name = a.name;
        n.description = a.description;
        n.icon = !a.icon.empty() ? a.icon : after.icon.string();
        n.points = a.points;
        n.tier = a.tier;
        n.secret = a.secret;
        if (now->second.progress.unlocked && !(was && was->progress.unlocked)) {
          n.kind = NotificationKind::kUnlock;
          center.Push(std::move(n));
          ++popup_count;
        } else if (!now->second.progress.unlocked && now->second.total &&
                   now->second.current > (was ? was->current : 0)) {
          n.kind = NotificationKind::kProgress;
          n.current = now->second.current;
          n.total = now->second.total;
          for (const auto& item : now->second.progress.completed)
            if (!was || std::find(was->progress.completed.begin(), was->progress.completed.end(), item) ==
                            was->progress.completed.end())
              n.item = item;
          if (a.secret) n.name = AppStyle()->secret_name;
          center.Push(std::move(n));
          ++popup_count;
        }
      }
    }
  }

  // ---- Launching ---------------------------------------------------------------

  // Which games run now: each entry's running.json, checked against the process.
  void RefreshRunning(float dt) {
    std::set<std::string> now;
    for (const auto& [id, game] : loaded)
      if (const auto m = ReadRunningMarker(game.dir); m && MarkerAlive(*m)) now.insert(id);
    for (auto it = starting.begin(); it != starting.end();) {
      it->second -= dt;
      if (it->second <= 0 || now.count(it->first)) {
        it = starting.erase(it);
        grid_dirty = true;
      } else {
        ++it;
      }
    }
    if (now != running) {
      running = std::move(now);
      grid_dirty = true;
      if (menu) menu->plan.running = running.count(menu->id) > 0;
    }
  }

  void OpenMenu(const std::string& id) {
    const auto it = loaded.find(id);
    if (it == loaded.end()) return;
    std::vector<SteamShortcut> shortcuts;
    if (options.shortcuts) {
      shortcuts = options.shortcuts();
    } else {
      for (const auto& f : FindSteamShortcutFiles(GetEnv("HOME")))
        for (auto& s : ReadSteamShortcuts(f)) shortcuts.push_back(std::move(s));
    }
    Menu m;
    m.id = id;
    m.title = it->second.title.empty() ? id : it->second.title;
    m.plan = PlanLaunch(it->second, shortcuts);
    m.plan.running = m.plan.running || running.count(id) > 0 || id == options.current_game;
    const bool can[2] = {!m.plan.steam_game_id.empty(), m.plan.direct};
    if (const auto p = prefs.methods.find(id); p != prefs.methods.end() && can[p->second == LaunchMethod::kDirect])
      m.choice = p->second == LaunchMethod::kDirect ? 1 : 0;
    else
      m.choice = can[0] ? 0 : can[1] ? 1 : -1;
    menu = std::move(m);
    grid_dirty = true;
    Latch();
  }

  void CloseMenu() {
    menu.reset();
    grid_dirty = true;
    Latch();
  }

  bool CanChoose(int i) const { return menu && (i == 0 ? !menu->plan.steam_game_id.empty() : menu->plan.direct); }

  void LaunchChosen() {
    if (!menu || menu->plan.running || menu->choice < 0) return;
    const LaunchMethod method = menu->choice == 0 ? LaunchMethod::kSteam : LaunchMethod::kDirect;
    std::string error;
    const auto command = BuildLaunch(menu->plan, method, &error);
    const bool ok = command && (options.start ? options.start(*command, &error) : StartDetached(*command, &error));
    if (!ok) {
      menu->error = error.empty() ? "Couldn't start the game." : error;
      grid_dirty = true;
      return;
    }
    std::string line = command->url;
    for (const auto& a : command->argv) line += (line.empty() ? "" : " ") + a;
    launched.push_back(line);
    prefs.methods[menu->id] = method;
    const bool quit = QuitBox() && prefs.quit_current;
    if (!prefs_file.empty()) SaveLauncherPrefs(prefs_file, prefs);
    starting[menu->id] = 30.0f;
    CloseMenu();
    if (quit) options.quit_current();
  }

  void ToggleQuit() {
    prefs.quit_current = !prefs.quit_current;
    if (!prefs_file.empty()) SaveLauncherPrefs(prefs_file, prefs);
    grid_dirty = true;
  }

  void MenuInput(uint32_t pd, uint32_t kd, const OverlayInput& in, bool click) {
    if ((pd & kPadB) || (kd & kEscape)) return CloseMenu();
    if ((pd & kPadX) || (kd & kTab)) {
      if (QuitBox()) ToggleQuit();
      return;
    }
    if ((pd & kPadA) || (kd & kEnter) || (pd & kPadY) || (kd & kSpace)) {
      if (menu->plan.running) return CloseMenu();
      if (menu->on_quit && ((pd & kPadA) || (kd & kEnter))) return ToggleQuit();
      return LaunchChosen();
    }
    const bool up = (pd & kPadUp) || (kd & kUp), down = (pd & kPadDown) || (kd & kDown);
    if ((up || down) && !menu->plan.running) {
      // Focus moves Steam, direct, then the box; a method row is also the choice.
      if (down && (menu->choice == 1 || !CanChoose(1)) && QuitBox()) {
        menu->on_quit = true;
      } else if (up && menu->on_quit) {
        menu->on_quit = false;
        if (menu->choice < 0) menu->choice = CanChoose(1) ? 1 : CanChoose(0) ? 0 : -1;
      } else if (!menu->on_quit) {
        const int next = up ? 0 : 1;
        if (CanChoose(next) && next != menu->choice) menu->choice = next;
      }
      menu->error.clear();
      grid_dirty = true;
    }
    if (click) {
      const float mx = in.mouse_x, my = in.mouse_y;
      if (QuitBox() && Inside(menu->quit, mx, my)) return ToggleQuit();
      if (Inside(menu->play, mx, my)) return menu->plan.running ? CloseMenu() : LaunchChosen();
      if (Inside(menu->cancel, mx, my) || !Inside(menu->box, mx, my)) return CloseMenu();
      for (int i = 0; i < 2; ++i)
        if (Inside(menu->rows[i], mx, my) && CanChoose(i) && !menu->plan.running) {
          menu->choice = i;
          menu->on_quit = false;
          menu->error.clear();
          grid_dirty = true;
        }
    }
  }

  // ---- The game page ---------------------------------------------------------------

  void OpenGame(const std::string& id) {
    const auto it = loaded.find(id);
    if (it == loaded.end()) return;
    const LibraryGame& game = it->second;
    auto p = std::make_unique<Page>();
    p->id = id;
    const auto c = chosen.find(id);
    const LibraryProfile* profile = PickProfile(game, c == chosen.end() ? std::string() : c->second);
    p->profile = profile ? profile->id : std::string();
    MirrorGame(p->system, game, profile, true);
    OverlayOptions o;
    o.global_preferences = options.global_preferences;
    o.create_global_preferences = false;  // Read-only: never make the player's folder.
    o.fallback_icon = game.icon.string();
    o.title = game.title.empty() ? game.id : game.title;
    o.show_settings = false;  // Toggling another game's sets comes later.
    o.show_disabled_sets = true;
    o.toggle_key = "none";
    o.toggle_pad = "none";
    o.buttons = given_labels;
    p->overlay = std::make_unique<Overlay>(p->system, std::move(o));
    p->overlay->SetSize(width, height, BaseScale());
    p->overlay->Open();
    page = std::move(p);
    UpdateHostButton();
    Latch();
  }

  // "Profile: <id>" when the game has more than one; pressing it moves on.
  void UpdateHostButton() {
    if (!page) return;
    const auto it = loaded.find(page->id);
    if (it == loaded.end() || it->second.profiles.size() < 2) {
      page->overlay->SetHostButton("", "", nullptr);
      return;
    }
    const auto& profiles = it->second.profiles;
    size_t at = 0;
    for (size_t i = 0; i < profiles.size(); ++i)
      if (profiles[i].id == page->profile) at = i;
    const std::string label = "Profile: " + page->profile + "  (" + std::to_string(at + 1) + "/" +
                              std::to_string(profiles.size()) + ")";
    page->overlay->SetHostButton(label, "Next profile", [this] { NextProfile(); });
  }

  void NextProfile() {
    if (!page) return;
    const auto it = loaded.find(page->id);
    if (it == loaded.end() || it->second.profiles.empty()) return;
    const auto& profiles = it->second.profiles;
    size_t at = 0;
    for (size_t i = 0; i < profiles.size(); ++i)
      if (profiles[i].id == page->profile) at = i;
    const LibraryProfile& next = profiles[(at + 1) % profiles.size()];
    page->profile = next.id;
    chosen[page->id] = next.id;
    MirrorGame(page->system, it->second, &next, false);
    UpdateHostButton();
    Summarize();
    grid_dirty = true;
  }

  // ---- Input ---------------------------------------------------------------

  void Latch() {
    latched_pad = prev_pad;
    latched_keys = prev_keys;
    repeat_dir = 0;
  }

  int RepeatStep(int dir, float dt) {
    int step = 0;
    if (dir != repeat_dir) {
      repeat_dir = dir;
      repeat_timer = 0.38f;
      step = dir;
    } else if (dir) {
      repeat_timer -= dt;
      if (repeat_timer <= 0) {
        repeat_timer += 0.09f;
        step = dir;
      }
    }
    return step;
  }

  void Select(int index, const Layout& l, bool instant = false) {
    if (games.empty()) return;
    sel = std::clamp(index, 0, static_cast<int>(games.size()) - 1);
    sel_id = games[sel].id;
    const RectF r = CardRect(l, sel);
    const float m = 10.0f * l.s;
    if (scroll_target > r.y0 - m) scroll_target = r.y0 - m;
    if (scroll_target < r.y1 + m - l.grid.height()) scroll_target = r.y1 + m - l.grid.height();
    scroll_target = std::clamp(scroll_target, 0.0f, MaxScroll(l));
    if (sel < l.columns) scroll_target = 0;
    if (instant) scroll = scroll_target;
    grid_dirty = true;
  }

  void SetSort(int mode) {
    sort = static_cast<SortMode>((mode % kSortModes + kSortModes) % kSortModes);
    Summarize();
    Select(sel, MakeLayout(), true);
  }

  void HandleInput(float dt, const OverlayInput& in) {
    const uint32_t pad = PadButtons(in), keys = Keys(in);
    const uint32_t pad_down = pad & ~prev_pad, keys_down = keys & ~prev_keys;
    latched_pad &= pad;
    latched_keys &= keys;
    const bool stick = std::fabs(in.pad_lx) > 0.5f || std::fabs(in.pad_ly) > 0.5f || std::fabs(in.pad_ry) > 0.5f;
    const bool click = in.mouse_present && in.mouse_left && !prev_mouse;
    const bool moved = in.mouse_present && (in.mouse_x != prev_mx || in.mouse_y != prev_my);
    const bool was_keyboard = keyboard;
    if (pad_down || stick) keyboard = false;
    bool any_key = keys_down != 0;
    for (int i = 1; i <= 12; ++i) any_key = any_key || in.f[i];
    if (any_key || click || (in.mouse_present && in.mouse_wheel != 0)) keyboard = true;
    if (keyboard != was_keyboard) grid_dirty = true;
    prev_pad = pad;
    prev_keys = keys;
    prev_mouse = in.mouse_present && in.mouse_left;
    prev_mx = in.mouse_x;
    prev_my = in.mouse_y;

    const uint32_t pd = pad_down & ~latched_pad, kd = keys_down & ~latched_keys;
    if (kd & kF11) fullscreen_request = true;
    if (page) return;  // The viewer has its own input (Frame passes it on).
    if (menu) return MenuInput(pd, kd, in, click);
    if (options.on_back && ((pd & kPadB) || (kd & kEscape))) {
      Latch();
      return options.on_back();
    }
    if (pd & kPadBack) fullscreen_request = true;

    const Layout l = MakeLayout();
    if ((pd & kPadX) || (kd & kTab)) {
      SetSort(static_cast<int>(sort) + (in.shift ? -1 : 1));
      return;
    }
    if ((pd & kPadLB) || (pd & kPadRB)) {
      SetSort(static_cast<int>(sort) + ((pd & kPadLB) ? -1 : 1));
      return;
    }
    if (((pd & kPadA) || (kd & kEnter)) && !games.empty()) {
      OpenGame(games[sel].id);
      return;
    }
    if (((pd & kPadY) || (kd & kSpace)) && !games.empty()) {
      OpenMenu(games[sel].id);
      return;
    }

    // Moving: up/down by rows (with repeat), left/right by one card.
    int dir = 0;
    if ((pad & kPadUp & ~latched_pad) || (keys & kUp & ~latched_keys) || in.pad_ly < -0.5f) dir = -l.columns;
    if ((pad & kPadDown & ~latched_pad) || (keys & kDown & ~latched_keys) || in.pad_ly > 0.5f) dir = l.columns;
    if ((pad & kPadLeft & ~latched_pad) || (keys & kLeft & ~latched_keys)) dir = -1;
    if ((pad & kPadRight & ~latched_pad) || (keys & kRight & ~latched_keys)) dir = 1;
    if (std::fabs(in.pad_lx) > 0.6f && std::fabs(in.pad_lx) > std::fabs(in.pad_ly)) dir = in.pad_lx < 0 ? -1 : 1;
    int step = RepeatStep(dir, dt);
    const int page_rows = std::max(1, static_cast<int>(l.grid.height() / (l.card_h + l.gap)));
    if (kd & kPageUp) step = -page_rows * l.columns;
    if (kd & kPageDown) step = page_rows * l.columns;
    if (kd & kHome) step = -1000000;
    if (kd & kEnd) step = 1000000;
    if (step && !games.empty()) {
      int next = sel + step;
      // Up or down past the edge stays in the same column's end row.
      if (std::abs(step) == l.columns && (next < 0 || next >= static_cast<int>(games.size()))) next = sel;
      Select(next, l);
    }

    float free = 0;
    if (std::fabs(in.pad_ry) > 0.2f) free += in.pad_ry * 1500.0f * l.s * dt;
    if (in.mouse_present && in.mouse_wheel != 0) free -= in.mouse_wheel * 110.0f * l.s;
    if (free != 0) {
      scroll_target = std::clamp(scroll_target + free, 0.0f, MaxScroll(l));
      grid_dirty = true;
    }

    if (in.mouse_present && (moved || click)) {
      const float mx = in.mouse_x, my = in.mouse_y;
      if (click) {
        if (!games.empty() && Inside(play_hit, mx, my)) return OpenMenu(games[sel].id);
        for (int i = 0; i < kSortModes; ++i)
          if (Inside(sort_hits[i], mx, my)) return SetSort(i);
        for (const auto& [r, id] : recent_hits)
          if (Inside(r, mx, my)) return OpenGame(id);
      }
      for (const auto& [r, i] : card_hits) {
        if (!Inside(r, mx, my)) continue;
        if (i != sel) {
          sel = i;
          sel_id = games[i].id;
          grid_dirty = true;
        }
        if (click) OpenGame(games[i].id);
        break;
      }
    }
  }

  // ---- Drawing ---------------------------------------------------------------

  void Text(bool bold, float size, float x, float y, std::string_view t, const Color& c) {
    grid.Text(bold ? *font_bold : *font, size, x, y, t, c);
  }
  float TextW(bool bold, float size, std::string_view t) const { return (bold ? font_bold : font)->TextWidth(size, t); }

  void IconTile(const std::string& ref, RectF r, const ViewerTheme& th, float s, float radius) {
    void* tex = ref.empty() ? nullptr : icons.Get(ref);
    if (tex) {
      grid.DrawImage(*static_cast<const CanvasImage*>(tex), r, {}, radius);
      return;
    }
    grid.FillRect(r, MixColor(th.panel, th.accent, 0.28f), radius);
    CanvasPainter p(grid, font.get(), font_bold.get());
    DrawTrophy(p, {(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}, r.width() * 0.5f, th.text);
    (void)s;
  }

  float Pill(float x, float cy, float s, std::string_view label, const Color& fill, const Color& ink) {
    const float ts = 12.5f * s, h = 22.0f * s;
    const float w = TextW(true, ts, label) + 18.0f * s;
    grid.FillRect({x, cy - h * 0.5f, x + w, cy + h * 0.5f}, fill, h * 0.5f);
    Text(true, ts, x + 9.0f * s, cy - ts * 0.5f, label, ink);
    return w;
  }

  void DrawCard(const GameSummary& g, RectF r, bool selected, const Layout& l, const ViewerTheme& th, const Style& st) {
    const float s = l.s;
    grid.FillRect(r, selected ? th.row_selected : th.row, 12.0f * s);
    if (selected) grid.StrokeRect(r, WithAlpha(th.accent, 0.85f), 12.0f * s, 2.0f * s);
    const float pad = 16.0f * s, icon = 104.0f * s;
    const RectF ir{r.x0 + pad, r.y0 + (r.height() - icon) * 0.5f, r.x0 + pad + icon, r.y0 + (r.height() + icon) * 0.5f};
    IconTile(g.icon.string(), ir, th, s, 12.0f * s);
    if (g.complete()) {
      const float br = 13.0f * s, cx = ir.x1 - br * 0.55f, cy = ir.y1 - br * 0.55f;
      grid.FillCircle(cx, cy, br + 2.5f * s, selected ? th.row_selected : th.row);
      grid.FillCircle(cx, cy, br, th.accent);
      DrawCheckMark(grid, cx, cy, br * 0.55f, 2.2f * s, OnColor(th.accent));
    }

    const float tx = ir.x1 + 16.0f * s, tw = r.x1 - pad - tx;
    float y = r.y0 + pad - 2.0f * s;
    const float ts = 19.0f * s;
    // The selected card's Play button sits top right.
    const float title_w = selected ? tw - TextW(true, 14.0f * s, "Play") - 44.0f * s : tw;
    for (const auto& line : Canvas::WrapText(*font_bold, ts, g.title, title_w, 2)) {
      Text(true, ts, tx, y, line, th.text);
      y += ts + 3.0f * s;
    }
    y += 5.0f * s;
    float px = tx;
    px += Pill(px, y + 11.0f * s, s, PlatformName(g.platform), PlatformColor(g.platform), Color::Rgba(255, 255, 255)) +
          6.0f * s;
    if (g.missing)
      px += Pill(px, y + 11.0f * s, s, "Folder not found", Color::Rgba(180, 70, 60), Color::Rgba(255, 255, 255)) +
            6.0f * s;
    else if (g.id == options.current_game)
      px += Pill(px, y + 11.0f * s, s, "Playing", th.accent, OnColor(th.accent)) + 6.0f * s;
    else if (running.count(g.id))
      px += Pill(px, y + 11.0f * s, s, "Running", Color::Rgba(46, 160, 92), Color::Rgba(255, 255, 255)) + 6.0f * s;
    else if (starting.count(g.id))
      px += Pill(px, y + 11.0f * s, s, "Starting\xE2\x80\xA6", MixColor(th.panel, th.text, 0.2f), th.text) + 6.0f * s;
    if (g.sets_off)
      Pill(px, y + 11.0f * s, s, std::to_string(g.sets_off) + (g.sets_off == 1 ? " set off" : " sets off"),
           MixColor(th.panel, th.text, 0.12f), th.subtext);
    if (selected) {
      // Play, for the mouse; Y or Space does the same.
      const float bs = 14.0f * s, bh = 28.0f * s;
      const float bw = TextW(true, bs, "Play") + 34.0f * s;
      const RectF b{r.x1 - pad - bw, r.y0 + pad - 4.0f * s, r.x1 - pad, r.y0 + pad - 4.0f * s + bh};
      grid.FillRect(b, th.accent, bh * 0.5f);
      const float tri = 5.5f * s, cx = b.x0 + 14.0f * s, cy = (b.y0 + b.y1) * 0.5f;
      grid.FillTriangle({cx - tri * 0.6f, cy - tri}, {cx - tri * 0.6f, cy + tri}, {cx + tri, cy}, OnColor(th.accent));
      Text(true, bs, cx + 9.0f * s, cy - bs * 0.55f, "Play", OnColor(th.accent));
      play_hit = b;
    }

    // Bottom up: last played, counts, the bar.
    const float bottom = r.y1 - pad;
    if (g.missing) {
      // Nothing to count: say why instead.
      const float ms = 13.5f * s;
      float my = bottom - 2 * (ms + 4.0f * s);
      for (const auto& line : Canvas::WrapText(*font, ms, "Its folder was moved, deleted or is on a drive that isn't connected.", tw, 2)) {
        Text(false, ms, tx, my, line, WithAlpha(th.subtext, 0.85f));
        my += ms + 4.0f * s;
      }
      return;
    }
    const float ls = 13.5f * s, cs = 14.5f * s;
    std::string played = "Played " + RelativeTime(g.last_played, Now());
    if (!g.last_played) played = "Not played yet";
    if (g.profiles > 1) played += "  \xC2\xB7  " + std::to_string(g.profiles) + " profiles";
    Text(false, ls, tx, bottom - ls, Canvas::EllipsizeText(*font, ls, played, tw), WithAlpha(th.subtext, 0.85f));
    const float cy = bottom - ls - 7.0f * s - cs;
    if (g.trophies) {
      // Trophy counts by tier, best first.
      CanvasPainter p(grid, font.get(), font_bold.get());
      float x = tx;
      for (Tier t : {Tier::kPlatinum, Tier::kGold, Tier::kSilver, Tier::kBronze}) {
        const TierCount& c = g.tiers[static_cast<int>(t)];
        if (!c.total) continue;
        const float tr = 7.5f * s;
        DrawTrophy(p, {x + tr, cy + cs * 0.5f}, tr, st.TierColor(t, th.text));
        x += tr * 2 + 5.0f * s;
        const std::string n = std::to_string(c.unlocked) + "/" + std::to_string(c.total);
        Text(true, cs, x, cy, n, c.unlocked == c.total ? th.text : th.subtext);
        x += TextW(true, cs, n) + 12.0f * s;
      }
    } else {
      std::string counts = std::to_string(g.unlocked) + " / " + std::to_string(g.count);
      if (g.points)
        counts += "  \xC2\xB7  " + std::to_string(g.unlocked_points) + " / " + std::to_string(g.points) +
                  PointsSuffix(g.platform);
      Text(true, cs, tx, cy, Canvas::EllipsizeText(*font_bold, cs, counts, tw), th.text);
    }
    const float bar_cy = cy - 13.0f * s;
    const int pct = static_cast<int>(std::floor(g.fraction() * 100.0f + 1e-4f));
    const std::string pcts = std::to_string(pct) + "%";
    const float ps = 16.0f * s;
    const float pw = TextW(true, ps, "100%");
    grid.ProgressBar({tx, bar_cy - 4.0f * s, tx + tw - pw - 10.0f * s, bar_cy + 4.0f * s}, g.fraction(), th.track,
                     th.accent);
    Text(true, ps, tx + tw - TextW(true, ps, pcts), bar_cy - ps * 0.5f, pcts, g.complete() ? th.accent : th.text);
  }

  void DrawGrid() {
    const auto st = AppStyle();
    const ViewerTheme th = ResolveViewerTheme(*st);
    const Layout l = MakeLayout();
    const float s = l.s;
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const Color bg = MixColor(th.panel, Color::Rgba(0, 0, 0), 0.18f);
    grid.Fill({0, 0, width, height}, Color{bg.r, bg.g, bg.b, 1.0f});
    HintPainter hints{grid, *font, *font_bold, s, labels, keyboard, th.text, th.subtext, 1.0f};

    // Header: title, the number of games, and totals over every game.
    const float x0 = l.margin, x1 = w - l.margin, hy = l.header_y;
    const float title = 30.0f * s;
    Text(true, title, x0, hy, "Achievements", th.text);
    uint32_t count = 0, unlocked = 0;
    size_t complete = 0;
    for (const auto& g : games) count += g.count, unlocked += g.unlocked, complete += g.complete();
    {
      std::string note = std::to_string(games.size()) + (games.size() == 1 ? " game" : " games");
      if (!games.empty()) note += "  \xC2\xB7  " + std::to_string(complete) + " complete";
      const float ns = 15.0f * s;
      Text(false, ns, x0 + TextW(true, title, "Achievements") + 18.0f * s, hy + (title - ns) * 0.62f, note, th.subtext);
      const std::string c = std::to_string(unlocked) + " / " + std::to_string(count);
      const float cs = 22.0f * s, ls = 15.0f * s;
      const int pct = count ? static_cast<int>(std::floor(100.0f * unlocked / count)) : 0;
      const std::string p = std::to_string(pct) + "%";
      float x = x1 - TextW(true, cs, p);
      Text(true, cs, x, hy + 2.0f * s, p, th.text);
      x -= 28.0f * s;
      x -= TextW(false, ls, " unlocked");
      Text(false, ls, x, hy + 2.0f * s + (cs - ls) * 0.7f, " unlocked", th.subtext);
      x -= TextW(true, cs, c);
      Text(true, cs, x, hy + 2.0f * s, c, th.text);
      grid.ProgressBar({x0, hy + 44.0f * s, x1, hy + 51.0f * s}, count ? static_cast<float>(unlocked) / count : 0.0f,
                       th.track, th.accent);
    }

    // Recent unlocks across games.
    recent_hits.clear();
    if (l.recent) {
      const float ts = 15.0f * s;
      Text(true, ts, x0 + 4.0f * s, l.recent_y + 4.0f * s, "RECENT UNLOCKS", th.subtext);
      const float ch = 64.0f * s, cy0 = l.recent_y + 30.0f * s, gap = 12.0f * s;
      const int n = std::max(1, static_cast<int>((x1 - x0 + gap) / (290.0f * s + gap)));
      const float cw = (x1 - x0 - (n - 1) * gap) / n;
      for (int i = 0; i < n && i < static_cast<int>(recent.size()); ++i) {
        const RecentItem& u = recent[i];
        const RectF r{x0 + i * (cw + gap), cy0, x0 + i * (cw + gap) + cw, cy0 + ch};
        grid.FillRect(r, th.row, 10.0f * s);
        const float is = 44.0f * s;
        const RectF ir{r.x0 + 10.0f * s, r.y0 + (ch - is) * 0.5f, r.x0 + 10.0f * s + is, r.y0 + (ch + is) * 0.5f};
        IconTile(u.icon, ir, th, s, 7.0f * s);
        const float tx = ir.x1 + 12.0f * s, tw = r.x1 - 10.0f * s - tx;
        const float ns = 15.5f * s, ds = 13.0f * s;
        float nx = tx;
        if (u.tier != Tier::kNone) {
          CanvasPainter p(grid, font.get(), font_bold.get());
          const float tr = 7.0f * s;
          DrawTrophy(p, {nx + tr, r.y0 + 12.0f * s + ns * 0.5f}, tr, st->TierColor(u.tier, th.text));
          nx += tr * 2 + 6.0f * s;
        }
        Text(true, ns, nx, r.y0 + 12.0f * s, Canvas::EllipsizeText(*font_bold, ns, u.name, tx + tw - nx), th.text);
        const std::string d = u.game_title + "  \xC2\xB7  " + RelativeTime(u.time, Now());
        Text(false, ds, tx, r.y0 + 12.0f * s + ns + 6.0f * s, Canvas::EllipsizeText(*font, ds, d, tw), th.subtext);
        recent_hits.push_back({r, u.game});
      }
    }

    // Sort modes, drawn like the viewer's filters.
    {
      const float fy = l.sort_y, fh = l.sort_h, fs = 15.5f * s;
      float x = x0;
      x += hints.Glyph(PadGlyph::kX, "Tab", x, fy + fh * 0.5f) + 10.0f * s;
      float seg[kSortModes], total = 0;
      for (int i = 0; i < kSortModes; ++i) total += seg[i] = TextW(true, fs, SortName(static_cast<SortMode>(i))) + 32.0f * s;
      grid.FillRect({x, fy, x + total, fy + fh}, WithAlpha(th.text, 0.07f), fh * 0.5f);
      for (int i = 0; i < kSortModes; ++i) {
        const RectF r{x, fy, x + seg[i], fy + fh};
        sort_hits[i] = r;
        const bool cur = i == static_cast<int>(sort);
        if (cur)
          grid.FillRect({r.x0 + 3.0f * s, r.y0 + 3.0f * s, r.x1 - 3.0f * s, r.y1 - 3.0f * s},
                        MixColor(th.panel, th.text, 0.22f), (fh - 6.0f * s) * 0.5f);
        Text(true, fs, x + 16.0f * s, fy + (fh - fs) * 0.5f, SortName(static_cast<SortMode>(i)),
             cur ? th.text : th.subtext);
        x += seg[i];
      }
    }

    // The cards.
    card_hits.clear();
    const float sc = std::round(scroll);
    drawn_scroll = sc;
    grid.PushClip(RectF{l.grid.x0 - 4.0f * s, l.grid.y0 - 4.0f * s, l.grid.x1 + 4.0f * s, l.grid.y1});
    for (size_t i = 0; i < games.size(); ++i) {
      RectF r = CardRect(l, static_cast<int>(i));
      r.y0 += l.grid.y0 - sc;
      r.y1 += l.grid.y0 - sc;
      if (r.y1 < l.grid.y0 - 4.0f * s || r.y0 > l.grid.y1) continue;
      DrawCard(games[i], r, static_cast<int>(i) == sel, l, th, *st);
      card_hits.push_back({RectF{r.x0, std::max(r.y0, l.grid.y0), r.x1, std::min(r.y1, l.grid.y1)}, static_cast<int>(i)});
    }
    if (games.empty()) {
      const float ms = 19.0f * s, ds = 15.0f * s;
      const char* msg = "No games in the library yet.";
      const char* sub = "Games that use recomp-achievements show up here after they run.";
      const float cy = (l.grid.y0 + l.grid.y1) * 0.5f - ms;
      Text(false, ms, (l.grid.x0 + l.grid.x1 - TextW(false, ms, msg)) * 0.5f, cy, msg, th.text);
      Text(false, ds, (l.grid.x0 + l.grid.x1 - TextW(false, ds, sub)) * 0.5f, cy + ms + 10.0f * s, sub, th.subtext);
    }
    grid.PopClip();

    // Scrollbar.
    const float content = ContentHeight(l);
    if (content > l.grid.height()) {
      const float tx = x1 + l.margin * 0.5f;
      const float bh = std::max(30.0f * s, l.grid.height() * l.grid.height() / content);
      const float t = MaxScroll(l) > 0 ? sc / MaxScroll(l) : 0.0f;
      const float by = l.grid.y0 + (l.grid.height() - bh) * std::clamp(t, 0.0f, 1.0f);
      grid.FillRect({tx - 2.0f * s, l.grid.y0, tx + 2.0f * s, l.grid.y1}, WithAlpha(th.text, 0.06f), 2.0f * s);
      grid.FillRect({tx - 2.5f * s, by, tx + 2.5f * s, by + bh}, WithAlpha(th.text, 0.35f), 2.5f * s);
    }

    // Footer hints.
    grid.FillRect({x0, l.footer_y, x1, l.footer_y + 1.0f * s}, th.border);
    const float fy = l.footer_y + (h - l.footer_y) * 0.5f, gap = 26.0f * s;
    float x = x0;
    x += hints.Hint(PadGlyph::kDpad, "\xE2\x86\x91\xE2\x86\x93\xE2\x86\x90\xE2\x86\x92", "Move", x, fy) + gap;
    if (!games.empty()) x += hints.Hint(PadGlyph::kA, "Enter", "Open", x, fy) + gap;
    if (!games.empty()) x += hints.Hint(PadGlyph::kY, "Space", "Play", x, fy) + gap;
    x += hints.Hint(PadGlyph::kX, "Tab", "Sort", x, fy) + gap;
    if (options.on_back) x += hints.Hint(PadGlyph::kB, "Esc", "Back", x, fy) + gap;
    const float fw = hints.Hint(PadGlyph::kBack, "F11", "Fullscreen", 0, fy, false);
    hints.Hint(PadGlyph::kBack, "F11", "Fullscreen", x1 - fw, fy);
    drawn_minute = Now() / 60;
    if (menu) DrawMenu(l, th, hints);
  }

  // The Play menu: a dialog over the dimmed grid.
  void DrawMenu(const Layout& l, const ViewerTheme& th, const HintPainter& hints) {
    Menu& m = *menu;
    const float s = l.s, w = static_cast<float>(width), h = static_cast<float>(height);
    grid.FillRect({0, 0, w, h}, Color{0, 0, 0, 0.55f});
    const float bw = std::min(w - 2 * l.margin, 640.0f * s);
    const float row_h = 74.0f * s, pad = 24.0f * s;
    const bool running_now = m.plan.running;
    float bh = pad + 34.0f * s + 14.0f * s;
    bh += running_now ? 64.0f * s : 2 * row_h + 12.0f * s;
    if (QuitBox()) bh += 44.0f * s + 12.0f * s;
    if (!m.error.empty()) bh += 34.0f * s;
    bh += 20.0f * s + 40.0f * s + pad;
    const RectF box{(w - bw) * 0.5f, (h - bh) * 0.5f, (w + bw) * 0.5f, (h + bh) * 0.5f};
    m.box = box;
    grid.FillRect(box, th.panel, 16.0f * s);
    grid.StrokeRect(box, th.border, 16.0f * s, 1.5f * s);
    const float x0 = box.x0 + pad, x1 = box.x1 - pad;
    float y = box.y0 + pad;
    const float ts = 24.0f * s;
    Text(true, ts, x0, y, Canvas::EllipsizeText(*font_bold, ts, "Play " + m.title, x1 - x0), th.text);
    y += 34.0f * s + 14.0f * s;

    m.rows[0] = m.rows[1] = {};
    if (running_now) {
      const float ms = 17.0f * s, ds = 14.5f * s;
      const bool here = m.id == options.current_game;
      Text(true, ms, x0, y, here ? "You're playing it now." : m.title + " is already running.", th.text);
      Text(false, ds, x0, y + ms + 10.0f * s,
           Canvas::EllipsizeText(*font, ds,
                                 here ? "Pick another game to start it from here."
                                      : "Switch to it instead of starting a second copy.",
                                 x1 - x0),
           th.subtext);
      y += 64.0f * s;
    } else {
      std::string steam_note = "No Steam entry found for this game.";
      if (m.plan.steam_source == "shortcut") steam_note = "Steam shortcut found.";
      if (m.plan.steam_source == "recorded") steam_note = "Steam started it last time.";
      if (!m.plan.steam_game_id.empty()) steam_note += " Steam's overlay and controller settings apply.";
      const char* names[2] = {"Launch through Steam", "Launch directly"};
      const std::string notes[2] = {steam_note, m.plan.direct ? m.plan.direct_note : m.plan.direct_note.empty()
                                                                     ? std::string("Can't be started directly.")
                                                                     : m.plan.direct_note};
      for (int i = 0; i < 2; ++i) {
        const RectF r{x0, y, x1, y + row_h};
        m.rows[i] = r;
        const bool on = CanChoose(i), cur = m.choice == i, focus = cur && !m.on_quit;
        grid.FillRect(r, focus ? th.row_selected : th.row, 10.0f * s);
        if (focus) grid.StrokeRect(r, WithAlpha(th.accent, 0.85f), 10.0f * s, 2.0f * s);
        // A radio dot.
        const float cx = r.x0 + 26.0f * s, cy = (r.y0 + r.y1) * 0.5f, rr = 9.0f * s;
        grid.StrokeCircle(cx, cy, rr, 2.0f * s, on ? th.text : WithAlpha(th.subtext, 0.5f));
        if (cur) grid.FillCircle(cx, cy, rr * 0.5f, th.accent);
        const float tx = cx + rr + 16.0f * s, tw = r.x1 - 14.0f * s - tx;
        const float ns = 17.5f * s, ds = 13.5f * s;
        const Color ink = on ? th.text : WithAlpha(th.subtext, 0.6f);
        Text(true, ns, tx, r.y0 + 15.0f * s, names[i], ink);
        if (on && prefs.methods.count(m.id) &&
            (prefs.methods.at(m.id) == LaunchMethod::kDirect) == (i == 1)) {
          const float lw = TextW(false, ds, "last used");
          Text(false, ds, r.x1 - 16.0f * s - lw, r.y0 + 17.0f * s, "last used", th.subtext);
        }
        Text(false, ds, tx, r.y0 + 15.0f * s + ns + 9.0f * s, Canvas::EllipsizeText(*font, ds, notes[i], tw),
             on ? th.subtext : WithAlpha(th.subtext, 0.6f));
        y += row_h + 12.0f * s;
      }
      m.quit = {};
      if (QuitBox()) {
        // "Quit current game", remembered for every game.
        const float qh = 44.0f * s;
        const RectF r{x0, y, x1, y + qh};
        m.quit = r;
        if (m.on_quit) {
          grid.FillRect(r, th.row_selected, 10.0f * s);
          grid.StrokeRect(r, WithAlpha(th.accent, 0.85f), 10.0f * s, 2.0f * s);
        }
        const float bx = r.x0 + 17.0f * s, bs = 18.0f * s, by = (r.y0 + r.y1 - bs) * 0.5f;
        const RectF b{bx, by, bx + bs, by + bs};
        if (prefs.quit_current) {
          grid.FillRect(b, th.accent, 4.0f * s);
          DrawCheckMark(grid, (b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f, bs * 0.3f, 2.2f * s, OnColor(th.accent));
        } else {
          grid.StrokeRect(b, th.text, 4.0f * s, 2.0f * s);
        }
        const float ns = 16.0f * s;
        Text(true, ns, b.x1 + 14.0f * s, (r.y0 + r.y1 - ns) * 0.5f - 1.0f * s, "Quit current game", th.text);
        const float gw = hints.Glyph(PadGlyph::kX, "Tab", 0, 0, false);
        hints.Glyph(PadGlyph::kX, "Tab", r.x1 - 14.0f * s - gw, (r.y0 + r.y1) * 0.5f);
        y += qh + 12.0f * s;
      }
    }
    if (!m.error.empty()) {
      const float es = 14.5f * s;
      Text(false, es, x0, y + 4.0f * s, Canvas::EllipsizeText(*font, es, m.error, x1 - x0), Color::Rgba(232, 96, 86));
      y += 34.0f * s;
    }

    // Buttons: Play (or Close) on the right, Cancel beside it.
    const float bs = 16.0f * s, bth = 40.0f * s, by = box.y1 - pad - bth;
    const bool can_play = !running_now && m.choice >= 0;
    const std::string main_label = running_now ? "Close" : "Play";
    const float mw = TextW(true, bs, main_label) + hints.Glyph(PadGlyph::kA, "Enter", 0, 0, false) + 40.0f * s;
    m.play = {x1 - mw, by, x1, by + bth};
    grid.FillRect(m.play, can_play || running_now ? th.accent : WithAlpha(th.text, 0.12f), bth * 0.5f);
    const Color main_ink = can_play || running_now ? OnColor(th.accent) : th.subtext;
    {
      HintPainter on = hints;
      on.text = on.subtext = main_ink;
      const float gx = m.play.x0 + 16.0f * s;
      const float gw = on.Glyph(PadGlyph::kA, "Enter", gx, (by + by + bth) * 0.5f);
      Text(true, bs, gx + gw + 8.0f * s, by + (bth - bs) * 0.5f, main_label, main_ink);
    }
    if (!running_now) {
      const float cw = TextW(true, bs, "Cancel") + hints.Glyph(PadGlyph::kB, "Esc", 0, 0, false) + 40.0f * s;
      m.cancel = {m.play.x0 - 12.0f * s - cw, by, m.play.x0 - 12.0f * s, by + bth};
      grid.FillRect(m.cancel, WithAlpha(th.text, 0.10f), bth * 0.5f);
      const float gx = m.cancel.x0 + 16.0f * s;
      const float gw = hints.Glyph(PadGlyph::kB, "Esc", gx, (by + by + bth) * 0.5f);
      Text(true, bs, gx + gw + 8.0f * s, by + (bth - bs) * 0.5f, "Cancel", th.text);
    } else {
      m.cancel = {};
    }
  }

  // ---- Popups ---------------------------------------------------------------

  // Draws the popups; returns the area that changed.
  PixelRect DrawPopups() {
    const auto active = center.Active();
    if (active.empty() && !popups_shown) return {};
    PixelRect area;
    std::vector<PopupRect> rects;
    const float k = BaseScale() / (static_cast<float>(height) / 1080.0f);
    CanvasPainter cp(popups, font.get(), font_bold.get());
    ScaledPainter p(cp, k);
    if (!active.empty()) {
      rects = LayoutPopups(active, {0, 0}, {width / k, height / k}, p);
      for (size_t i = 0; i < active.size(); ++i) {
        if (active[i].n.popup().lines.icon) rects[i].icon = icons.Get(active[i].n.icon);
        const float m = (8.0f * rects[i].scale + 2.0f) * k;
        area = Union(area, Intersect({static_cast<int>(std::floor(rects[i].min.x * k - m)),
                                      static_cast<int>(std::floor(rects[i].min.y * k - m)),
                                      static_cast<int>(std::ceil(rects[i].max.x * k + m)),
                                      static_cast<int>(std::ceil(rects[i].max.y * k + m))},
                                     {0, 0, width, height}));
      }
    }
    const PixelRect region = Union(area, popup_area);
    popups.Clear(region);
    popups.PushClip(region);
    for (size_t i = 0; i < active.size(); ++i) DrawPopup(p, active[i], rects[i]);
    popups.PopClip();
    popup_area = area;
    popups_shown = !active.empty();
    return region;
  }

  // ---- Frame ---------------------------------------------------------------

  void Composite(PixelRect r) {
    r = Intersect(r, {0, 0, width, height});
    if (r.empty()) return;
    const Image& base = grid.image();
    const Image* layers[2] = {page ? &page->overlay->image() : nullptr, popups_shown || !popup_area.empty() ? &popups.image() : nullptr};
    for (int y = r.y0; y < r.y1; ++y) {
      for (int x = r.x0; x < r.x1; ++x) {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        int c[3] = {base.rgba[i], base.rgba[i + 1], base.rgba[i + 2]};
        for (const Image* layer : layers) {
          if (!layer || layer->rgba.size() <= i + 3) continue;
          const int a = layer->rgba[i + 3];
          if (!a) continue;
          for (int k = 0; k < 3; ++k) c[k] = (c[k] * (255 - a) + layer->rgba[i + k] * a + 127) / 255;
        }
        out.rgba[i] = static_cast<uint8_t>(c[0]);
        out.rgba[i + 1] = static_cast<uint8_t>(c[1]);
        out.rgba[i + 2] = static_cast<uint8_t>(c[2]);
        out.rgba[i + 3] = 255;
      }
    }
  }

  void RefreshLabels() {
    const uint64_t gen = styles.generation();
    if (gen == style_generation) return;
    style_generation = gen;
    const std::string b = Lower(AppStyle()->viewer.buttons);
    labels = b == "playstation" || b == "ps" || b == "ps3" ? ButtonLabels::kPlayStation
             : b == "xbox" || b == "xbox360"               ? ButtonLabels::kXbox
                                                           : given_labels;
    grid_dirty = true;
  }

  bool Frame(float dt, const OverlayInput& in) {
    if (width <= 0 || height <= 0 || !font || !font_bold) return false;
    RefreshLabels();
    poll_t += dt;
    if (!polled || poll_t >= options.poll_seconds) {
      const float since = poll_t;
      poll_t = 0;
      Poll();
      RefreshRunning(since);
    }
    HandleInput(dt, in);

    PixelRect changed;
    if (page) {
      OverlayInput forward = in;
      // Buttons held from the grid (the A that opened it) mustn't act here.
      SetPadButtons(forward, latched_pad, false);
      if (latched_keys & kEnter) forward.enter = false;
      if (page->overlay->Frame(dt, forward)) changed = Union(changed, page->overlay->dirty_rect());
      if (!page->overlay->is_open() && !page->overlay->visible()) {
        page.reset();
        Latch();
        changed = {0, 0, width, height};
        grid_dirty = true;
      }
    }

    // Grid scrolling.
    const float diff = scroll_target - scroll;
    if (std::fabs(diff) < 0.5f) scroll = scroll_target;
    else scroll += diff * (1.0f - std::exp(-dt * 16.0f));
    if (std::round(scroll) != drawn_scroll) grid_dirty = true;
    if (Now() / 60 != drawn_minute) grid_dirty = true;  // "5 minutes ago" moves on.

    if (grid_dirty) {
      grid.ResetDirty();
      DrawGrid();
      grid_dirty = false;
      changed = Union(changed, grid.dirty());
    }
    if (dt > 0) center.Update(dt);
    changed = Union(changed, DrawPopups());
    if (changed.empty()) return false;
    Composite(changed);
    out_dirty = changed;
    return true;
  }
};

App::App(AppOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
App::~App() = default;

void App::SetSize(int width, int height) {
  Impl& m = *impl_;
  width = std::max(1, width);
  height = std::max(1, height);
  if (width == m.width && height == m.height) return;
  m.width = width;
  m.height = height;
  m.grid.Resize(width, height);
  m.popups.Resize(width, height);
  m.popup_area = {};
  m.out.width = width;
  m.out.height = height;
  m.out.rgba.assign(static_cast<size_t>(width) * height * 4, 255);
  if (m.page) m.page->overlay->SetSize(width, height, m.BaseScale());
  m.grid_dirty = true;
  if (m.polled) m.Select(m.sel, m.MakeLayout(), true);
}

int App::width() const { return impl_->width; }
int App::height() const { return impl_->height; }
bool App::Frame(float dt, const OverlayInput& input) { return impl_->Frame(dt, input); }
const Image& App::image() const { return impl_->out; }
PixelRect App::dirty_rect() const { return impl_->out_dirty; }

void App::SetButtonLabels(ButtonLabels labels) {
  Impl& m = *impl_;
  if (labels == m.given_labels) return;
  m.given_labels = labels;
  m.style_generation = 0;  // Re-resolve against the player's choice.
  if (m.page) m.page->overlay->SetButtonLabels(labels);
}

void App::SetScale(float scale) {
  Impl& m = *impl_;
  m.options.scale = std::max(0.25f, scale);
  if (m.page) m.page->overlay->SetSize(m.width, m.height, m.BaseScale());
  m.grid_dirty = true;
  if (m.polled) m.Select(m.sel, m.MakeLayout(), true);
}

bool App::TakeFullscreenToggle() {
  const bool r = impl_->fullscreen_request;
  impl_->fullscreen_request = false;
  return r;
}

bool App::Poll() { return impl_->Poll(); }

void App::HoldInput(const OverlayInput& held) {
  Impl& m = *impl_;
  m.prev_pad = PadButtons(held);
  m.prev_keys = Keys(held);
  m.Latch();
}

App::State App::state() const {
  const Impl& m = *impl_;
  State s;
  s.game_page = m.page != nullptr;
  s.sort = m.sort;
  s.games = m.games;
  s.selected = m.sel_id;
  s.scroll = m.scroll;
  for (const auto& r : m.recent) s.recent.push_back(r.game + "/" + r.key);
  if (m.page) {
    s.game = m.page->id;
    s.profile = m.page->profile;
    if (auto it = m.loaded.find(m.page->id); it != m.loaded.end())
      for (const auto& p : it->second.profiles) s.profiles.push_back(p.id);
    s.page = m.page->overlay->view_state();
  }
  if (m.menu) {
    s.launch_menu = true;
    s.launch_game = m.menu->id;
    s.launch_choice = m.menu->choice == 0 ? "steam" : m.menu->choice == 1 ? "direct" : "";
    s.launch_error = m.menu->error;
    s.quit_box = m.QuitBox();
    s.quit_current = m.prefs.quit_current;
  }
  s.running.assign(m.running.begin(), m.running.end());
  for (const auto& [id, t] : m.starting) s.starting.push_back(id);
  s.launched = m.launched;
  s.reloads = m.reloads;
  s.popups = m.popup_count;
  return s;
}

}  // namespace recomp::achievements::app
