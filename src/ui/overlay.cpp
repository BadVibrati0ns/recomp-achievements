#include "recomp/achievements/ui/overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <set>

#include "recomp/achievements/ui/painter.h"

namespace recomp::achievements::ui {

namespace {

// ---- Bindings -----------------------------------------------------------------

std::string Lower(std::string_view s) {
  std::string o(s);
  for (char& c : o)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return o;
}

std::string Trim(std::string_view s) {
  while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
  while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
  return std::string(s);
}

enum KeyBit : uint32_t {
  kKeyUp = 1u << 0,
  kKeyDown = 1u << 1,
  kKeyLeft = 1u << 2,
  kKeyRight = 1u << 3,
  kKeyEnter = 1u << 4,
  kKeySpace = 1u << 5,
  kKeyEscape = 1u << 6,
  kKeyBackspace = 1u << 7,
  kKeyPageUp = 1u << 8,
  kKeyPageDown = 1u << 9,
  kKeyHome = 1u << 10,
  kKeyEnd = 1u << 11,
  kKeyTab = 1u << 12,
};

uint32_t KeyBits(const OverlayInput& in) {
  uint32_t k = 0;
  if (in.up) k |= kKeyUp;
  if (in.down) k |= kKeyDown;
  if (in.left) k |= kKeyLeft;
  if (in.right) k |= kKeyRight;
  if (in.enter) k |= kKeyEnter;
  if (in.space) k |= kKeySpace;
  if (in.escape) k |= kKeyEscape;
  if (in.backspace) k |= kKeyBackspace;
  if (in.page_up) k |= kKeyPageUp;
  if (in.page_down) k |= kKeyPageDown;
  if (in.home) k |= kKeyHome;
  if (in.end) k |= kKeyEnd;
  if (in.tab) k |= kKeyTab;
  return k;
}

uint32_t FKeyBits(const OverlayInput& in) {
  uint32_t k = 0;
  for (int i = 1; i <= 12; ++i)
    if (in.f[i]) k |= 1u << i;
  return k;
}

// ---- Colours ------------------------------------------------------------------

Color WithAlpha(Color c, float a) {
  c.a *= a;
  return c;
}

Color Mix(const Color& a, const Color& b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

float Luma(const Color& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

// Text colour that reads on top of c.
Color OnColor(const Color& c) { return Luma(c) > 0.55f ? Color::Rgba(16, 16, 18) : Color::Rgba(255, 255, 255); }

std::string FormatTime(uint64_t unix_seconds) {
  if (!unix_seconds) return {};
  const std::time_t t = static_cast<std::time_t>(unix_seconds);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
  return buf;
}

std::string CreditsLine(const std::vector<Credit>& credits) {
  std::string out;
  for (const auto& c : credits) {
    if (!out.empty()) out += ", ";
    out += c.name;
    if (!c.role.empty()) out += " (" + c.role + ")";
  }
  return out;
}

const char* PointsSuffix(Platform p) { return p == Platform::kXbox360 ? "G" : " pts"; }

// Scales every coordinate, so popups laid out for a virtual display size land
// on the real image when the game's ui_scale differs from height / 1080.
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

enum class Device { kKeyboard, kPad };

// What a hint glyph stands for.
enum class Action { kDetails, kBack, kFilter, kSecrets, kPrevSet, kNextSet, kCredits, kMove };

}  // namespace

uint32_t PadButtons(const OverlayInput& in) {
  uint32_t b = 0;
  if (in.pad_a) b |= kPadA;
  if (in.pad_b) b |= kPadB;
  if (in.pad_x) b |= kPadX;
  if (in.pad_y) b |= kPadY;
  if (in.pad_lb) b |= kPadLB;
  if (in.pad_rb) b |= kPadRB;
  if (in.pad_start) b |= kPadStart;
  if (in.pad_back) b |= kPadBack;
  if (in.pad_up) b |= kPadUp;
  if (in.pad_down) b |= kPadDown;
  if (in.pad_left) b |= kPadLeft;
  if (in.pad_right) b |= kPadRight;
  return b;
}

void SetPadButtons(OverlayInput& in, uint32_t mask, bool down) {
  auto set = [&](uint32_t bit, bool& field) {
    if (mask & bit) field = down;
  };
  set(kPadA, in.pad_a);
  set(kPadB, in.pad_b);
  set(kPadX, in.pad_x);
  set(kPadY, in.pad_y);
  set(kPadLB, in.pad_lb);
  set(kPadRB, in.pad_rb);
  set(kPadStart, in.pad_start);
  set(kPadBack, in.pad_back);
  set(kPadUp, in.pad_up);
  set(kPadDown, in.pad_down);
  set(kPadLeft, in.pad_left);
  set(kPadRight, in.pad_right);
}

bool ParsePadChord(std::string_view text, uint32_t& out) {
  static const std::map<std::string, uint32_t, std::less<>> kNames = {
      {"a", kPadA},         {"cross", kPadA},       {"south", kPadA},     {"b", kPadB},
      {"circle", kPadB},    {"east", kPadB},        {"x", kPadX},         {"square", kPadX},
      {"west", kPadX},      {"y", kPadY},           {"triangle", kPadY},  {"north", kPadY},
      {"lb", kPadLB},       {"l1", kPadLB},         {"rb", kPadRB},       {"r1", kPadRB},
      {"start", kPadStart}, {"options", kPadStart}, {"menu", kPadStart},  {"back", kPadBack},
      {"select", kPadBack}, {"share", kPadBack},    {"view", kPadBack},   {"up", kPadUp},
      {"down", kPadDown},   {"left", kPadLeft},     {"right", kPadRight},
  };
  const std::string t = Lower(Trim(text));
  out = 0;
  if (t.empty() || t == "none") return true;
  uint32_t mask = 0;
  size_t pos = 0;
  while (pos <= t.size()) {
    const size_t plus = t.find('+', pos);
    const std::string name = Trim(std::string_view(t).substr(pos, plus == std::string::npos ? std::string::npos : plus - pos));
    auto it = kNames.find(name);
    if (it == kNames.end()) return false;
    mask |= it->second;
    if (plus == std::string::npos) break;
    pos = plus + 1;
  }
  out = mask;
  return true;
}

bool ParseToggleKey(std::string_view text, int& out) {
  const std::string t = Lower(Trim(text));
  out = 0;
  if (t.empty() || t == "none") return true;
  if (t.size() < 2 || t[0] != 'f') return false;
  int n = 0;
  for (size_t i = 1; i < t.size(); ++i) {
    if (t[i] < '0' || t[i] > '9') return false;
    n = n * 10 + (t[i] - '0');
  }
  if (n < 1 || n > 12) return false;
  out = n;
  return true;
}

// ---- Impl -----------------------------------------------------------------------

struct Overlay::Impl {
  // One line of the list: a set heading, a category heading or an achievement.
  struct Entry {
    enum Kind { kSet, kCategory, kRow } kind = kRow;
    float y = 0, h = 0;  // In list space.
    std::string title, detail;
    // Rows.
    std::string key;  // Kept as a copy: view's pointers die with their set.
    AchievementView view;
    bool hidden = false, unlocked = false;
    std::string name, description, time, points;
    std::vector<std::string> lines;  // Description, wrapped for the current state.
    bool expandable = false;
    std::vector<std::pair<std::string, bool>> checklist;  // Item, done.
    size_t checklist_done = 0;
    std::string credits;
    int columns = 1;
  };
  struct CreditLine {
    std::string text;
    float size = 0;
    bool bold = false, dim = false;
    float y = 0;
  };
  // Pixel sizes for the viewer at the current scale.
  struct Metrics {
    float s = 1;
    RectF panel, list;
    float pad = 0, radius = 0;
    float title = 0, header_h = 0;
    float tabs_y = 0, tab_h = 0, filter_y = 0, filter_h = 0, footer_y = 0;
    float icon = 0, row_pad = 0, name = 0, desc = 0, meta = 0, line_gap = 0, right_w = 0, text_x = 0, text_w = 0;
  };
  struct Theme {
    Color backdrop, panel, accent, text, subtext, track, border, row, row_selected;
    const Style* style = nullptr;
  };

  System& system;
  OverlayOptions options;
  std::unique_ptr<NotificationCenter> own_center;
  std::unique_ptr<StyleManager> own_styles;
  std::unique_ptr<IconCache> own_icons;
  NotificationCenter* center;
  StyleManager* styles;
  IconCache* icons;
  SoundBank bank;
  int unlock_listener = -1, progress_listener = -1;
  std::atomic<uint64_t> data_serial{1};

  std::shared_ptr<Font> font, font_bold;
  std::map<std::string, std::shared_ptr<Font>, std::less<>> named_fonts;

  Canvas canvas;
  int width = 0, height = 0;
  float ui_scale = 0.0f;  // 0 = height / 1080.
  bool size_changed = false;
  PixelRect last_dirty;
  Stats stats;

  // Bindings, re-read when the styles change.
  uint64_t bindings_generation = 0;
  int toggle_key = 8;
  uint32_t toggle_chord = kPadStart | kPadLB;
  ButtonLabels labels = ButtonLabels::kXbox;
  bool labels_from_style = false;

  // Input edges.
  uint32_t prev_pad = 0, prev_keys = 0, prev_fkeys = 0;
  bool prev_mouse = false;
  float prev_mouse_x = -1, prev_mouse_y = -1;
  uint32_t latched_pad = 0, latched_keys = 0;
  bool chord_held = false;
  // What the game sees (GameInput): chord buttons held back, taps to replay,
  // and a latch after closing until everything is released.
  uint32_t held_back = 0;
  float held_back_t = 0.0f;
  uint32_t tap = 0;
  bool release_latch = false;
  bool shown = false;  // visible() as of the last returned image.
  bool stick_x_held = false;
  int repeat_dir = 0;  // -1 up, +1 down.
  float repeat_timer = 0;
  Device device = Device::kKeyboard;
  // A press that was the first half of the closing chord is undone.
  struct Undo {
    float age = 1e9f;
    size_t tab = 0;
    int filter = 0;
    bool reveal = false;
    int selected = 0;
  } undo;
  float clock = 0;

  // Viewer state.
  bool open = false;
  float open_t = 0;  // 0..1 animation.
  bool credits_screen = false;
  size_t tab = 0;  // 0 = all sets when there are several.
  int filter = 0;  // 0 all, 1 unlocked, 2 locked.
  bool reveal = false;
  int selected = 0;  // Index into rows.
  std::string selected_key;
  float scroll = 0, scroll_target = 0;
  float drawn_scroll = -1;
  std::set<std::string> expanded;
  bool viewer_dirty = true;
  bool was_viewer_visible = false;
  float drawn_open_t = 0;

  // Model.
  bool model_stale = true;
  uint64_t model_generation = 0, model_serial = 0, model_style_generation = 0;
  float model_scale = 0, model_width = 0;
  std::vector<const AchievementSet*> sets;
  std::vector<std::string> tab_ids;  // "" = all.
  std::vector<std::string> tab_titles;
  std::vector<Entry> entries;
  std::vector<int> rows;  // Entry indices of the achievement rows.
  std::vector<CreditLine> credit_lines;
  float content_h = 0, credits_h = 0;
  System::Totals totals;

  // Hit areas from the last viewer render.
  std::vector<std::pair<RectF, size_t>> tab_hits;
  RectF filter_hits[3];
  RectF secrets_hit;
  float icon_poll = 0;
  std::vector<void*> drawn_icons;
  std::vector<int> drawn_rows;

  // Popups drawn last time, to clear them.
  struct PopupSig {
    uint64_t serial;
    int x0, y0, x1, y1, alpha, text;
    void* icon;
    bool operator==(const PopupSig&) const = default;
  };
  std::vector<PopupSig> popup_sigs;
  PixelRect popup_area;

  Impl(System& sys, OverlayOptions opt) : system(sys), options(std::move(opt)) {
    if (options.center) {
      center = options.center;
    } else {
      own_center = std::make_unique<NotificationCenter>();
      center = own_center.get();
    }
    if (options.styles) {
      styles = options.styles;
    } else {
      own_styles = std::make_unique<StyleManager>();
      styles = own_styles.get();
    }
    if (options.icons) {
      icons = options.icons;
    } else {
      own_icons = std::make_unique<IconCache>();
      icons = own_icons.get();
      UseImageTextures(*icons);
    }
    labels = options.buttons;

    std::filesystem::path global_dir;
    if (options.global_preferences)
      global_dir = options.create_global_preferences ? EnsureGlobalConfigDir() : GlobalConfigDir();
    if (own_styles) {
      if (!options.game_style_file.empty()) styles->LoadGameStyleFile(options.game_style_file);
      if (!global_dir.empty()) styles->LoadGlobalStyleFile(global_dir / "achievements_style.json");
      if (!options.user_dir.empty()) styles->LoadUserStyleFile(options.user_dir / "achievements_style.json");
    }
    // Later folders win: the game's, the player's global ones, then this game's.
    for (const auto& dir : options.sound_dirs) bank.AddOverrideDir(dir);
    if (!global_dir.empty()) bank.AddOverrideDir(global_dir / "sounds");
    if (!options.user_dir.empty()) bank.AddOverrideDir(options.user_dir / "sounds");
    if (!options.fallback_icon.empty()) icons->SetFallbackIcon(options.fallback_icon);

    if (own_center) {
      center->UseStyles(*styles);
      if (options.sound_handler) {
        center->SetSoundHandler(options.sound_handler);
      } else if (options.play_sound) {
        center->SetSoundHandler([this](const NotificationCenter::SoundRequest& r) {
          if (auto pcm = bank.ForEvent(r.kind, r.tier, r.sound)) options.play_sound(pcm, r.volume, r.kind);
        });
      }
      center->Attach(system);
    }
    unlock_listener = system.AddUnlockListener([this](const UnlockEvent&) { ++data_serial; });
    progress_listener = system.AddProgressListener([this](const ProgressEvent&) { ++data_serial; });

    if (!options.font.empty()) font = Font::FromTtf(options.font);
    if (!options.font_bold.empty()) font_bold = Font::FromTtf(options.font_bold);
    if (!font) font = Font::Builtin(false);
    if (!font_bold) {
      if (!options.font.empty() && font) {
        // The game's regular font, thickened, so text keeps one typeface.
        font_bold = Font::FromTtf(options.font);
        if (font_bold) font_bold->SetSyntheticBold(true);
      } else {
        font_bold = Font::Builtin(true);
      }
    }
    if (font && font != Font::Builtin(false) && Font::Builtin(false)) font->SetFallback(Font::Builtin(false));
    if (font_bold && font_bold != Font::Builtin(true) && Font::Builtin(true)) font_bold->SetFallback(Font::Builtin(true));
  }

  ~Impl() {
    system.RemoveListener(unlock_listener);
    system.RemoveListener(progress_listener);
    if (own_center) {
      center->Detach();
      center->SetSoundHandler(nullptr);
    }
  }

  // ---- Scale and theme ---------------------------------------------------------

  float BaseScale() const { return ui_scale > 0 ? ui_scale : height / 1080.0f; }

  const AchievementSet* TabSet() const {
    if (sets.empty()) return nullptr;
    if (tab < tab_ids.size() && !tab_ids[tab].empty())
      for (auto* s : sets)
        if (s->id == tab_ids[tab]) return s;
    return sets.front();
  }

  std::shared_ptr<const Style> ChromeStyle() const { return styles->Resolve(TabSet()); }

  Theme MakeTheme(const Style& st) const {
    const ui::ViewerStyle& v = st.viewer;
    Theme t;
    t.style = &st;
    t.backdrop = v.backdrop.value_or(Color::Rgba(0, 0, 0, 165));
    // Opaque: a see-through panel lets the game's edges ghost through the text.
    Color panel = st.unlock.colors.background;
    panel.a = 1.0f;
    t.panel = v.panel.value_or(panel);
    t.accent = v.accent.value_or(st.progress.colors.bar_fill);
    t.text = v.text.value_or(st.unlock.colors.name);
    t.subtext = v.subtext.value_or(st.unlock.colors.description);
    t.track = st.progress.colors.bar_track;
    t.border = WithAlpha(t.text, 0.10f);
    t.row = Mix(t.panel, t.text, 0.045f);
    t.row_selected = Mix(t.panel, t.accent, 0.20f);
    t.row.a = t.row_selected.a = 1.0f;
    return t;
  }

  Metrics MakeMetrics(const Style& st) const {
    Metrics m;
    // Below ~0.75 the text gets too small to read at 720p.
    m.s = std::max(0.75f, BaseScale() * st.viewer.scale);
    const float s = m.s;
    const float margin = std::max(16.0f, 36.0f * s);
    const float pw = std::min(static_cast<float>(width) - 2 * margin, 1120.0f * s);
    const float ph = std::min(static_cast<float>(height) - 2 * margin, 1000.0f * s);
    m.panel = {(width - pw) * 0.5f, (height - ph) * 0.5f, (width + pw) * 0.5f, (height + ph) * 0.5f};
    m.pad = 28.0f * s;
    m.radius = std::clamp(st.unlock.rounding, 0.0f, 16.0f) * s;
    m.title = 30.0f * s;
    m.header_h = 66.0f * s;
    m.tabs_y = m.panel.y0 + m.pad + m.header_h;
    m.tab_h = 40.0f * s;
    m.filter_y = m.tabs_y + m.tab_h + 14.0f * s;
    m.filter_h = 34.0f * s;
    m.footer_y = m.panel.y1 - 56.0f * s;
    m.list = {m.panel.x0 + m.pad, m.filter_y + m.filter_h + 16.0f * s, m.panel.x1 - m.pad, m.footer_y - 8.0f * s};
    m.icon = 64.0f * s;
    m.row_pad = 14.0f * s;
    m.name = 21.0f * s;
    m.desc = 16.5f * s;
    m.meta = 14.5f * s;
    m.line_gap = 5.0f * s;
    m.right_w = 150.0f * s;
    m.text_x = m.row_pad + m.icon + 18.0f * s;
    m.text_w = std::max(80.0f * s, m.list.width() - m.text_x - m.right_w - m.row_pad);
    return m;
  }

  // ---- Model ---------------------------------------------------------------

  bool Hidden(const Achievement& a, bool unlocked) const { return a.secret && !unlocked && !reveal; }

  void BuildModel() {
    sets = system.sets();
    // Keep the tab on the same set across rebuilds.
    const std::string cur_tab = tab < tab_ids.size() ? tab_ids[tab] : std::string();
    tab_ids.clear();
    tab_titles.clear();
    if (sets.size() > 1) {
      tab_ids.push_back("");
      tab_titles.push_back("All");
    }
    for (auto* s : sets) {
      tab_ids.push_back(s->id);
      tab_titles.push_back(s->title.empty() ? s->id : s->title);
    }
    tab = 0;
    for (size_t i = 0; i < tab_ids.size(); ++i)
      if (tab_ids[i] == cur_tab) tab = i;

    const auto st = ChromeStyle();
    const Metrics m = MakeMetrics(*st);
    const std::string scope = tab < tab_ids.size() ? tab_ids[tab] : std::string();
    totals = system.GetTotals(scope);
    entries.clear();
    rows.clear();
    const auto views = system.Snapshot();
    float y = 0;
    const bool all = scope.empty() && sets.size() > 1;
    for (const AchievementSet* set : sets) {
      if (!scope.empty() && set->id != scope) continue;
      if (all) {
        Entry e;
        e.kind = Entry::kSet;
        e.title = set->title.empty() ? set->id : set->title;
        const auto t = system.GetTotals(set->id);
        e.detail = std::to_string(t.unlocked) + " / " + std::to_string(t.count);
        if (t.points)
          e.detail += "   " + std::to_string(t.unlocked_points) + " / " + std::to_string(t.points) +
                      PointsSuffix(set->platform);
        e.y = y;
        e.h = 54.0f * m.s;
        y += e.h;
        entries.push_back(std::move(e));
      }
      // Categories in first-seen order, as the ImGui list does.
      std::vector<std::string> categories;
      std::map<std::string, std::vector<const AchievementView*>> by_category;
      for (const auto& v : views) {
        if (!v.achievement || v.achievement->set_id != set->id) continue;
        const bool unlocked = v.progress.unlocked;
        if (filter == 1 && !unlocked) continue;
        if (filter == 2 && unlocked) continue;
        if (!by_category.count(v.achievement->category)) categories.push_back(v.achievement->category);
        by_category[v.achievement->category].push_back(&v);
      }
      for (const auto& cat : categories) {
        const auto& list = by_category[cat];
        if (categories.size() > 1 || !cat.empty()) {
          Entry e;
          e.kind = Entry::kCategory;
          e.title = cat.empty() ? "General" : cat;
          const size_t done = static_cast<size_t>(
              std::count_if(list.begin(), list.end(), [](const AchievementView* v) { return v->progress.unlocked; }));
          e.detail = std::to_string(done) + " / " + std::to_string(list.size());
          e.y = y;
          e.h = 40.0f * m.s;
          y += e.h;
          entries.push_back(std::move(e));
        }
        for (const AchievementView* v : list) {
          Entry e = MakeRow(*v, *set, *st, m);
          e.y = y;
          y += e.h + 8.0f * m.s;
          rows.push_back(static_cast<int>(entries.size()));
          entries.push_back(std::move(e));
        }
      }
    }
    content_h = y;
    BuildCredits(m);

    // Keep the selection on the same achievement when it's still listed.
    int found = -1;
    for (size_t i = 0; i < rows.size(); ++i)
      if (entries[rows[i]].key == selected_key)
        found = static_cast<int>(i);
    selected = found >= 0 ? found : std::clamp(selected, 0, std::max(0, static_cast<int>(rows.size()) - 1));
    if (!rows.empty()) selected_key = entries[rows[selected]].key;
    scroll_target = std::clamp(scroll_target, 0.0f, MaxScroll(m));
    scroll = std::clamp(scroll, 0.0f, MaxScroll(m));

    model_stale = false;
    model_generation = system.generation();
    model_serial = data_serial.load();
    model_style_generation = styles->generation();
    model_scale = m.s;
    model_width = m.list.width();
    viewer_dirty = true;
  }

  Entry MakeRow(const AchievementView& v, const AchievementSet& set, const Style& st, const Metrics& m) const {
    const Font& reg = *font;
    Entry e;
    e.kind = Entry::kRow;
    e.view = v;
    e.key = v.achievement->key;
    const Achievement& a = *v.achievement;
    e.unlocked = v.progress.unlocked;
    e.hidden = Hidden(a, e.unlocked);
    // Secret text follows the ImGui list: a locked secret shows its locked
    // description even when revealed.
    e.name = e.hidden ? st.secret_name : a.name;
    e.description = e.hidden ? (a.locked_description.empty() ? st.secret_description : a.locked_description)
                             : (!e.unlocked && a.secret && !a.locked_description.empty() ? a.locked_description
                                                                                          : a.description);
    if (a.tier != Tier::kNone) e.points = st.TierName(a.tier);
    else if (a.points) e.points = std::to_string(a.points) + PointsSuffix(set.platform);
    if (e.unlocked && v.progress.unlock_time) e.time = "Unlocked " + FormatTime(v.progress.unlock_time);
    if (!a.checklist.empty() && !e.hidden) {
      for (const auto& item : a.checklist) {
        const bool ok = e.unlocked || std::find(v.progress.completed.begin(), v.progress.completed.end(), item) !=
                                          v.progress.completed.end();
        e.checklist.emplace_back(item, ok);
        if (ok) ++e.checklist_done;
      }
    }
    if (options.show_credits && !a.credits.empty() && !e.hidden) e.credits = "Credits: " + CreditsLine(a.credits);
    const bool is_open = expanded.count(a.key) > 0;
    const auto full = Canvas::WrapText(reg, m.desc, e.description, m.text_w, 12);
    e.expandable = !e.checklist.empty() || !e.credits.empty() || full.size() > 2;
    e.lines = Canvas::WrapText(reg, m.desc, e.description, m.text_w, is_open ? 12 : 2);

    float h = m.name;
    if (!e.lines.empty()) h += m.line_gap + e.lines.size() * (m.desc + 3.0f * m.s) - 3.0f * m.s;
    const bool bar = !e.unlocked && v.progress_total > 0;
    if (bar || !e.time.empty() || !e.checklist.empty()) h += m.line_gap + 4.0f * m.s + m.meta;
    if (is_open && e.expandable) {
      if (!e.checklist.empty()) {
        e.columns = m.text_w >= 560.0f * m.s ? 3 : m.text_w >= 320.0f * m.s ? 2 : 1;
        const int n = static_cast<int>(e.checklist.size());
        const int lines = (n + e.columns - 1) / e.columns;
        h += 10.0f * m.s + lines * 26.0f * m.s;
      }
      if (!e.credits.empty()) h += 10.0f * m.s + m.meta;
    }
    e.h = std::max(m.icon, h) + m.row_pad * 2;
    return e;
  }

  void BuildCredits(const Metrics& m) {
    credit_lines.clear();
    float y = 0;
    auto add = [&](std::string text, float size, bool b, bool dim, float before = 0) {
      y += before;
      // Long lines wrap to the list width.
      for (auto& line : Canvas::WrapText(b ? *font_bold : *font, size, text, m.list.width() - 2 * m.row_pad, 8)) {
        credit_lines.push_back({std::move(line), size, b, dim, y});
        y += size + 6.0f * m.s;
      }
    };
    const std::string scope = tab < tab_ids.size() ? tab_ids[tab] : std::string();
    add("Credits", 26.0f * m.s, true, false);
    const auto views = system.Snapshot();
    for (const AchievementSet* set : sets) {
      if (!scope.empty() && set->id != scope) continue;
      add(set->title.empty() ? set->id : set->title, 20.0f * m.s, true, false, 18.0f * m.s);
      if (set->credits.empty()) add("No credits listed for this set.", 16.0f * m.s, false, true);
      for (const auto& c : set->credits) {
        add(c.name + (c.role.empty() ? "" : "  -  " + c.role), 17.0f * m.s, false, false);
        if (!c.url.empty()) add(c.url, 14.5f * m.s, false, true);
      }
      bool heading = false;
      for (const auto& v : views) {
        if (!v.achievement || v.achievement->set_id != set->id || v.achievement->credits.empty()) continue;
        if (Hidden(*v.achievement, v.progress.unlocked)) continue;
        if (!heading) add("Achievement credits", 16.0f * m.s, true, true, 8.0f * m.s);
        heading = true;
        add(v.achievement->name + ": " + CreditsLine(v.achievement->credits), 16.0f * m.s, false, false);
      }
    }
    add("Achievement viewer", 20.0f * m.s, true, false, 22.0f * m.s);
    add("recomp-achievements, by The recomp-achievements authors (0BSD).", 16.0f * m.s, false, false);
    add("Text in DejaVu Sans (Bitstream Vera licence, DejaVu changes public domain).", 14.5f * m.s, false, true);
    credits_h = y;
  }

  float MaxScroll(const Metrics& m) const {
    return std::max(0.0f, (credits_screen ? credits_h : content_h) - m.list.height() + 12.0f * m.s);
  }

  void EnsureModel() {
    // The set pointers may be stale after a generation change, so check that
    // before anything resolves the current tab's style.
    if (model_stale || model_generation != system.generation() || model_serial != data_serial.load() ||
        model_style_generation != styles->generation()) {
      BuildModel();
      return;
    }
    const Metrics m = MakeMetrics(*ChromeStyle());
    if (model_scale != m.s || model_width != m.list.width()) BuildModel();
  }

  // ---- Bindings ----------------------------------------------------------------

  void RefreshBindings() {
    const uint64_t gen = styles->generation();
    if (gen == bindings_generation) return;
    bindings_generation = gen;
    const auto st = styles->Resolve(nullptr);
    int key = 0;
    if (!ParseToggleKey(st->viewer.key.empty() ? options.toggle_key : st->viewer.key, key))
      ParseToggleKey(options.toggle_key, key);
    toggle_key = key;
    uint32_t chord = 0;
    if (!ParsePadChord(st->viewer.pad.empty() ? options.toggle_pad : st->viewer.pad, chord))
      ParsePadChord(options.toggle_pad, chord);
    toggle_chord = chord;
    const std::string b = Lower(st->viewer.buttons);
    if (b == "playstation" || b == "ps" || b == "ps3") labels = ButtonLabels::kPlayStation, labels_from_style = true;
    else if (b == "xbox" || b == "xbox360") labels = ButtonLabels::kXbox, labels_from_style = true;
    else if (labels_from_style) labels = options.buttons, labels_from_style = false;
  }

  // ---- Actions --------------------------------------------------------------

  void SetOpen(bool o) {
    if (o == open) return;
    open = o;
    viewer_dirty = true;
    if (open) {
      // Buttons still held from the toggle mustn't act as navigation.
      latched_pad = prev_pad;
      latched_keys = prev_keys;
      repeat_dir = 0;
      credits_screen = false;
      model_stale = true;
    } else {
      credits_screen = false;
      release_latch = true;
    }
  }

  void Select(int index, const Metrics& m, bool instant = false) {
    if (rows.empty()) return;
    selected = std::clamp(index, 0, static_cast<int>(rows.size()) - 1);
    const Entry& e = entries[rows[selected]];
    selected_key = e.key;
    // Keep the row (and any heading right above it) in view.
    const float margin = 16.0f * m.s;
    float top = e.y - margin;
    for (int i = rows[selected] - 1; i >= 0 && entries[i].kind != Entry::kRow; --i) top = entries[i].y - margin;
    if (selected == 0) top = 0;
    const float bottom = e.y + e.h + margin - m.list.height();
    if (scroll_target > top) scroll_target = top;
    if (scroll_target < bottom) scroll_target = std::min(bottom, top);
    scroll_target = std::clamp(scroll_target, 0.0f, MaxScroll(m));
    if (instant) scroll = scroll_target;
    viewer_dirty = true;
  }

  void SetTab(size_t t, const Metrics& m) {
    if (tab_ids.empty()) return;
    t = std::min(t, tab_ids.size() - 1);
    if (t == tab) return;
    tab = t;
    selected = 0;
    selected_key.clear();
    scroll = scroll_target = 0;
    BuildModel();
    (void)m;
  }

  void SetFilter(int f) {
    filter = (f % 3 + 3) % 3;
    BuildModel();
    const auto st = ChromeStyle();
    Select(selected, MakeMetrics(*st));
  }

  void SetReveal(bool r) {
    if (!options.allow_reveal_secrets) return;
    reveal = r;
    BuildModel();
  }

  void ToggleExpanded() {
    if (rows.empty()) return;
    const Entry& e = entries[rows[selected]];
    if (!e.expandable || !e.view.achievement) return;
    const std::string& key = e.key;
    if (!expanded.erase(key)) expanded.insert(key);
    BuildModel();
    const auto st = ChromeStyle();
    Select(selected, MakeMetrics(*st));
  }

  void SaveUndo() { undo = {0.0f, tab, filter, reveal, selected}; }

  void HandleInput(float dt, const OverlayInput& in) {
    const uint32_t pad = PadButtons(in);
    const uint32_t keys = KeyBits(in);
    const uint32_t fkeys = FKeyBits(in);
    const uint32_t pad_down = pad & ~prev_pad;
    const uint32_t keys_down = keys & ~prev_keys;
    const uint32_t f_down = fkeys & ~prev_fkeys;
    latched_pad &= pad;
    latched_keys &= keys;
    const bool stick = std::fabs(in.pad_lx) > 0.5f || std::fabs(in.pad_ly) > 0.5f || std::fabs(in.pad_ry) > 0.5f;
    const bool mouse_moved = in.mouse_present && (in.mouse_x != prev_mouse_x || in.mouse_y != prev_mouse_y);
    const Device before = device;
    if (pad_down || stick) device = Device::kPad;
    if (keys_down || f_down || (in.mouse_present && (in.mouse_wheel != 0 || (in.mouse_left && !prev_mouse))))
      device = Device::kKeyboard;
    if (device != before && open) viewer_dirty = true;
    undo.age += dt;

    // Game-facing input. Taps are reported for one frame only.
    tap = 0;
    if (release_latch && !open && !pad && !keys && !fkeys && !(in.mouse_present && in.mouse_left) &&
        !stick) {
      release_latch = false;
    }
    if (!open && options.chord_grace > 0.0f && toggle_chord) {
      held_back_t += dt;
      const uint32_t released = held_back & ~pad;
      held_back &= pad;
      const uint32_t fresh = pad_down & toggle_chord;
      if (fresh) {
        if (!held_back) held_back_t = 0.0f;
        held_back |= fresh;
      }
      if (held_back && held_back_t >= options.chord_grace) held_back = 0;  // Not a chord: let it through.
      tap = released;
    }

    // The toggle. The chord fires once when its last button goes down, in
    // any order; holding it doesn't repeat.
    bool toggled = false;
    if (toggle_key > 0 && (f_down & (1u << toggle_key))) toggled = true;
    const bool chord_full = toggle_chord && (pad & toggle_chord) == toggle_chord;
    if (chord_full && !chord_held && (pad_down & toggle_chord)) {
      toggled = true;
      if (open && undo.age < 0.5f) {
        // The chord's first button already moved the list; put it back.
        tab = undo.tab;
        filter = undo.filter;
        reveal = undo.reveal;
        selected = undo.selected;
        model_stale = true;
      }
    }
    chord_held = chord_full;

    prev_pad = pad;
    prev_keys = keys;
    prev_fkeys = fkeys;
    const bool mouse_click = in.mouse_present && in.mouse_left && !prev_mouse;
    prev_mouse = in.mouse_present && in.mouse_left;
    prev_mouse_x = in.mouse_x;
    prev_mouse_y = in.mouse_y;

    if (toggled) {
      SetOpen(!open);
      latched_pad = pad;
      latched_keys = keys;
      held_back = 0;
      tap = 0;
      return;
    }
    if (!open) return;
    EnsureModel();
    const auto st = ChromeStyle();
    const Metrics m = MakeMetrics(*st);
    const uint32_t pd = pad_down & ~latched_pad;
    const uint32_t kd = keys_down & ~latched_keys;
    const uint32_t ph = pad & ~latched_pad;
    const uint32_t kh = keys & ~latched_keys;
    const bool chord_member = (pd & toggle_chord) != 0;

    if ((pd & kPadB) || (kd & (kKeyEscape | kKeyBackspace))) {
      if (credits_screen) {
        credits_screen = false;
        scroll = scroll_target = 0;
        Select(selected, m, true);
      } else {
        SetOpen(false);
      }
      return;
    }
    if (options.show_credits && ((pd & kPadBack) || (f_down & 2u))) {
      if (chord_member) SaveUndo();
      credits_screen = !credits_screen;
      scroll = scroll_target = 0;
      if (!credits_screen) Select(selected, m, true);
      viewer_dirty = true;
      return;
    }

    // Sets: LB/RB, left/right, the stick.
    int tab_step = 0;
    if (pd & kPadLB) tab_step = -1;
    if (pd & kPadRB) tab_step = 1;
    if ((pd & kPadLeft) || (kd & kKeyLeft)) tab_step = -1;
    if ((pd & kPadRight) || (kd & kKeyRight)) tab_step = 1;
    const bool sx = std::fabs(in.pad_lx) > 0.6f;
    if (sx && !stick_x_held) tab_step = in.pad_lx < 0 ? -1 : 1;
    if (std::fabs(in.pad_lx) < 0.3f) stick_x_held = false;
    if (sx) stick_x_held = true;
    if (tab_step && tab_ids.size() > 1) {
      if (chord_member) SaveUndo();
      const size_t n = tab_ids.size();
      SetTab((tab + n + tab_step) % n, m);
      if (credits_screen) BuildModel();
      return;
    }

    if (!credits_screen) {
      if ((pd & kPadX) || (kd & kKeyTab)) {
        if (chord_member) SaveUndo();
        SetFilter(filter + (in.shift ? -1 : 1));
        return;
      }
      if (options.allow_reveal_secrets && ((pd & kPadY) || (kd & kKeySpace))) {
        if (chord_member) SaveUndo();
        SetReveal(!reveal);
        return;
      }
      if ((pd & kPadA) || (kd & kKeyEnter)) {
        if (chord_member) SaveUndo();
        ToggleExpanded();
        return;
      }
    }

    // Up and down, with key repeat.
    int dir = 0;
    if ((ph & kPadUp) || (kh & kKeyUp) || in.pad_ly < -0.5f) dir = -1;
    if ((ph & kPadDown) || (kh & kKeyDown) || in.pad_ly > 0.5f) dir = 1;
    int step = 0;
    if (dir != repeat_dir) {
      repeat_dir = dir;
      repeat_timer = 0.38f;
      step = dir;
    } else if (dir) {
      repeat_timer -= dt;
      if (repeat_timer <= 0) {
        repeat_timer += 0.065f;
        step = dir;
      }
    }
    const int page = std::max(1, static_cast<int>(m.list.height() / (m.icon + m.row_pad * 2 + 8.0f * m.s)) - 1);
    if (kd & kKeyPageUp) step = -page;
    if (kd & kKeyPageDown) step = page;
    if (kd & kKeyHome) step = -1000000;
    if (kd & kKeyEnd) step = 1000000;
    if (credits_screen) {
      const float line = 48.0f * m.s;
      if (step) scroll_target = std::clamp(scroll_target + step * line, 0.0f, MaxScroll(m));
    } else if (step && !rows.empty()) {
      Select(selected + step, m);
    }

    // Free scrolling: right stick and the wheel. The selection follows when
    // it leaves the view.
    float free = 0;
    if (std::fabs(in.pad_ry) > 0.2f) free += in.pad_ry * 1500.0f * m.s * dt;
    if (in.mouse_present && in.mouse_wheel != 0) free -= in.mouse_wheel * 110.0f * m.s;
    if (free != 0) {
      scroll_target = std::clamp(scroll_target + free, 0.0f, MaxScroll(m));
      if (!credits_screen && !rows.empty()) {
        const Entry& e = entries[rows[selected]];
        if (e.y < scroll_target || e.y + e.h > scroll_target + m.list.height()) {
          int best = selected;
          for (size_t i = 0; i < rows.size(); ++i) {
            const Entry& r = entries[rows[i]];
            if (r.y >= scroll_target && r.y + r.h <= scroll_target + m.list.height()) {
              best = static_cast<int>(i);
              if (static_cast<int>(i) > selected || free < 0) break;
            }
          }
          if (best != selected) {
            selected = best;
            selected_key = entries[rows[best]].key;
          }
        }
      }
      viewer_dirty = true;
    }

    // Mouse: hover selects, click expands or picks a tab or filter, a click
    // outside the panel closes.
    if (in.mouse_present && (mouse_moved || mouse_click)) {
      const float mx = in.mouse_x, my = in.mouse_y;
      auto inside = [&](const RectF& r) { return mx >= r.x0 && mx < r.x1 && my >= r.y0 && my < r.y1; };
      if (mouse_click && !inside(m.panel)) {
        SetOpen(false);
        return;
      }
      if (mouse_click) {
        for (const auto& [r, t] : tab_hits)
          if (inside(r)) {
            SetTab(t, m);
            return;
          }
        for (int f = 0; f < 3; ++f)
          if (inside(filter_hits[f])) {
            SetFilter(f);
            return;
          }
        if (inside(secrets_hit)) {
          SetReveal(!reveal);
          return;
        }
      }
      if (!credits_screen && inside(m.list)) {
        const float ly = my - m.list.y0 + std::round(scroll);
        for (size_t i = 0; i < rows.size(); ++i) {
          const Entry& e = entries[rows[i]];
          if (ly >= e.y && ly < e.y + e.h) {
            if (static_cast<int>(i) != selected) {
              selected = static_cast<int>(i);
              selected_key = e.key;
              viewer_dirty = true;
            }
            if (mouse_click) ToggleExpanded();
            break;
          }
        }
      }
    }
  }

  // ---- Drawing helpers ----------------------------------------------------------

  float TextW(bool b, float size, std::string_view t) const { return (b ? font_bold : font)->TextWidth(size, t); }

  void DrawText(bool b, float size, float x, float y, std::string_view t, const Color& c) {
    canvas.Text(b ? *font_bold : *font, size, x, y, t, c);
  }

  // A button or key glyph centred vertically on cy; returns its width.
  float DrawGlyph(Action a, float x, float cy, float s, const Theme& th, float alpha, bool draw = true) {
    const float d = 26.0f * s;  // Face button diameter.
    const float r = d * 0.5f;
    if (device == Device::kKeyboard) {
      const char* label = "";
      switch (a) {
        case Action::kDetails: label = "Enter"; break;
        case Action::kBack: label = "Esc"; break;
        case Action::kFilter: label = "Tab"; break;
        case Action::kSecrets: label = "Space"; break;
        case Action::kPrevSet: label = "\xE2\x86\x90"; break;  // ←
        case Action::kNextSet: label = "\xE2\x86\x92"; break;  // →
        case Action::kCredits: label = "F1"; break;
        case Action::kMove: label = "\xE2\x86\x91\xE2\x86\x93"; break;  // ↑↓
      }
      const float ts = 14.0f * s;
      const float w = std::max(d, TextW(true, ts, label) + 14.0f * s);
      if (draw) {
        const RectF k{x, cy - r, x + w, cy + r};
        canvas.FillRect(k, WithAlpha(th.text, 0.10f * alpha), 5.0f * s);
        canvas.StrokeRect(k, WithAlpha(th.text, 0.45f * alpha), 5.0f * s, 1.25f * s);
        DrawText(true, ts, x + (w - TextW(true, ts, label)) * 0.5f, cy - ts * 0.5f, label, WithAlpha(th.text, alpha));
      }
      return w;
    }
    const bool ps = labels == ButtonLabels::kPlayStation;
    auto shoulder = [&](const char* label) {
      const float ts = 13.0f * s;
      const float w = std::max(d * 1.5f, TextW(true, ts, label) + 14.0f * s);
      if (draw) {
        const RectF k{x, cy - r * 0.85f, x + w, cy + r * 0.85f};
        canvas.FillRect(k, WithAlpha(th.text, 0.12f * alpha), r * 0.6f);
        canvas.StrokeRect(k, WithAlpha(th.text, 0.5f * alpha), r * 0.6f, 1.25f * s);
        DrawText(true, ts, x + (w - TextW(true, ts, label)) * 0.5f, cy - ts * 0.5f, label, WithAlpha(th.text, alpha));
      }
      return w;
    };
    auto face = [&](int which) {  // 0 a/cross, 1 b/circle, 2 x/square, 3 y/triangle
      if (!draw) return d;
      const float cx = x + r;
      if (!ps) {
        static const Color kColors[4] = {Color::Rgba(92, 176, 54), Color::Rgba(214, 58, 46),
                                         Color::Rgba(40, 112, 214), Color::Rgba(236, 178, 22)};
        static const char* kLetters[4] = {"A", "B", "X", "Y"};
        canvas.FillCircle(cx, cy, r, WithAlpha(kColors[which], alpha));
        const float ts = 16.0f * s;
        DrawText(true, ts, cx - TextW(true, ts, kLetters[which]) * 0.5f, cy - ts * 0.5f, kLetters[which],
                 WithAlpha(Color::Rgba(255, 255, 255), alpha));
        return d;
      }
      static const Color kColors[4] = {Color::Rgba(124, 168, 228), Color::Rgba(232, 96, 92),
                                       Color::Rgba(222, 142, 200), Color::Rgba(64, 196, 164)};
      canvas.FillCircle(cx, cy, r, WithAlpha(Color::Rgba(28, 28, 32), alpha));
      canvas.StrokeCircle(cx, cy, r - 0.75f * s, 1.5f * s, WithAlpha(th.text, 0.35f * alpha));
      const Color c = WithAlpha(kColors[which], alpha);
      const float k = r * 0.42f, t = 2.2f * s;
      switch (which) {
        case 0:
          canvas.Line(cx - k, cy - k, cx + k, cy + k, t, c);
          canvas.Line(cx - k, cy + k, cx + k, cy - k, t, c);
          break;
        case 1: canvas.StrokeCircle(cx, cy, k, t, c); break;
        case 2: canvas.StrokeRect({cx - k * 0.9f, cy - k * 0.9f, cx + k * 0.9f, cy + k * 0.9f}, c, 0, t); break;
        default: {
          const Vec2 p0{cx, cy - k * 1.05f}, p1{cx + k * 1.05f, cy + k * 0.75f}, p2{cx - k * 1.05f, cy + k * 0.75f};
          canvas.Line(p0.x, p0.y, p1.x, p1.y, t, c);
          canvas.Line(p1.x, p1.y, p2.x, p2.y, t, c);
          canvas.Line(p2.x, p2.y, p0.x, p0.y, t, c);
        }
      }
      return d;
    };
    switch (a) {
      case Action::kDetails: return face(0);
      case Action::kBack: return face(1);
      case Action::kFilter: return face(2);
      case Action::kSecrets: return face(3);
      case Action::kPrevSet: return shoulder(ps ? "L1" : "LB");
      case Action::kNextSet: return shoulder(ps ? "R1" : "RB");
      case Action::kCredits: return shoulder(ps ? "SELECT" : "BACK");
      case Action::kMove: {
        // A d-pad cross.
        if (draw) {
          const float cx = x + r, w = r * 0.36f;
          canvas.FillRect({cx - w, cy - r, cx + w, cy + r}, WithAlpha(th.text, 0.8f * alpha), w * 0.5f);
          canvas.FillRect({cx - r, cy - w, cx + r, cy + w}, WithAlpha(th.text, 0.8f * alpha), w * 0.5f);
        }
        return d;
      }
    }
    return d;
  }

  // Glyph then label; returns the total width.
  float DrawHint(Action a, const char* label, float x, float cy, float s, const Theme& th, float alpha,
                 bool draw = true) {
    const float gw = DrawGlyph(a, x, cy, s, th, alpha, draw);
    const float ts = 15.0f * s;
    if (draw) DrawText(false, ts, x + gw + 8.0f * s, cy - ts * 0.5f, label, WithAlpha(th.subtext, alpha));
    return gw + 8.0f * s + TextW(false, ts, label);
  }

  void DrawIconTile(const Entry& e, RectF r, const Theme& th, float alpha, float s) {
    const Achievement& a = *e.view.achievement;
    const AchievementSet* set = e.view.set;
    const std::filesystem::path base = set ? set->base_dir : std::filesystem::path();
    // As the ImGui list: a locked icon when there is one, else the icon
    // dimmed; hidden secrets show only their locked icon (or the fallback).
    void* tex = nullptr;
    bool dim = false;
    if (!e.hidden) {
      if (!e.unlocked && !a.locked_icon.empty()) tex = icons->Get(a.locked_icon, a.set_id, base);
      if (!tex) {
        tex = icons->GetOrFallback(a.icon, a.set_id, base);
        dim = !e.unlocked;
      }
    } else {
      tex = icons->GetOrFallback(a.locked_icon, a.set_id, base);
    }
    drawn_icons.push_back(tex);
    const float rad = 8.0f * s;
    if (tex) {
      const float k = dim ? 0.43f : 1.0f;
      canvas.DrawImage(*static_cast<const CanvasImage*>(tex), r, Color{k, k, k, alpha}, rad);
    } else {
      const Color tile = e.unlocked ? Mix(th.panel, th.accent, 0.35f) : Mix(th.panel, th.text, 0.08f);
      canvas.FillRect(r, WithAlpha(tile, alpha), rad);
      CanvasPainter p(canvas, font.get(), font_bold.get());
      const Color col = e.unlocked ? th.text : WithAlpha(th.text, 0.45f);
      DrawTrophy(p, {(r.x0 + r.x1) * 0.5f, (r.y0 + r.y1) * 0.5f}, r.width() * 0.5f, WithAlpha(col, alpha));
    }
    if (e.unlocked) {
      // A small check badge on unlocked rows.
      const float br = 11.0f * s;
      const float cx = r.x1 - br * 0.6f, cy = r.y1 - br * 0.6f;
      canvas.FillCircle(cx, cy, br + 2.0f * s, WithAlpha(th.panel, alpha));
      canvas.FillCircle(cx, cy, br, WithAlpha(th.accent, alpha));
      DrawCheck(cx, cy, br * 0.55f, 2.0f * s, WithAlpha(OnColor(th.accent), alpha));
    }
  }

  void DrawCheck(float cx, float cy, float k, float t, const Color& c) {
    canvas.Line(cx - k, cy, cx - k * 0.3f, cy + k * 0.7f, t, c);
    canvas.Line(cx - k * 0.3f, cy + k * 0.7f, cx + k, cy - k * 0.6f, t, c);
  }

  void DrawRow(const Entry& e, float y, bool sel, const Metrics& m, const Theme& th, float alpha) {
    const float s = m.s;
    const RectF r{m.list.x0, y, m.list.x1, y + e.h};
    canvas.FillRect(r, WithAlpha(sel ? th.row_selected : th.row, alpha), 10.0f * s);
    if (sel) canvas.StrokeRect(r, WithAlpha(th.accent, 0.85f * alpha), 10.0f * s, 2.0f * s);
    const float ix = r.x0 + m.row_pad, iy = y + m.row_pad;
    DrawIconTile(e, {ix, iy, ix + m.icon, iy + m.icon}, th, alpha, s);

    const float tx = r.x0 + m.text_x;
    float ty = y + m.row_pad;
    const float fade = e.unlocked ? 1.0f : 0.72f;
    const Color name_col = WithAlpha(th.text, alpha * (e.unlocked ? 1.0f : 0.85f));
    const Color sub = WithAlpha(th.subtext, alpha * fade);
    DrawText(true, m.name, tx, ty, Canvas::EllipsizeText(*font_bold, m.name, e.name, m.text_w), name_col);
    ty += m.name;

    // Points or tier, right-aligned.
    const float rx = r.x1 - m.row_pad;
    const Achievement& a = *e.view.achievement;
    if (!e.points.empty()) {
      const float ps = 19.0f * s;
      float w = TextW(true, ps, e.points);
      Color pc = WithAlpha(th.text, alpha * fade);
      if (a.tier != Tier::kNone) {
        pc = WithAlpha(th.style->TierColor(a.tier, th.text), alpha * fade);
        const float tr = 13.0f * s;
        CanvasPainter p(canvas, font.get(), font_bold.get());
        DrawTrophy(p, {rx - w - tr - 8.0f * s, y + m.row_pad + m.name * 0.5f}, tr, pc);
      }
      DrawText(true, ps, rx - w, y + m.row_pad + (m.name - ps) * 0.5f, e.points, pc);
    }

    if (!e.lines.empty()) {
      ty += m.line_gap;
      for (const auto& line : e.lines) {
        DrawText(false, m.desc, tx, ty, line, sub);
        ty += m.desc + 3.0f * s;
      }
      ty -= 3.0f * s;
    }

    // Progress, unlock time or checklist count.
    const bool bar = !e.unlocked && e.view.progress_total > 0;
    if (bar || !e.time.empty() || !e.checklist.empty()) {
      ty += m.line_gap + 4.0f * s;
      float mx = tx;
      if (bar) {
        const float bw = std::min(300.0f * s, m.text_w * 0.6f);
        const float frac = static_cast<float>(e.view.progress_current) / static_cast<float>(e.view.progress_total);
        canvas.ProgressBar({mx, ty + m.meta * 0.5f - 4.0f * s, mx + bw, ty + m.meta * 0.5f + 4.0f * s}, frac,
                           WithAlpha(th.track, alpha), WithAlpha(th.accent, alpha));
        mx += bw + 12.0f * s;
        const std::string count =
            std::to_string(e.view.progress_current) + " / " + std::to_string(e.view.progress_total);
        DrawText(true, m.meta, mx, ty, count, WithAlpha(th.text, alpha * 0.9f));
        mx += TextW(true, m.meta, count) + 18.0f * s;
      }
      if (!e.time.empty()) {
        DrawText(false, m.meta, mx, ty, e.time, WithAlpha(th.subtext, alpha * 0.85f));
        mx += TextW(false, m.meta, e.time) + 18.0f * s;
      }
      if (!e.checklist.empty() && !bar) {
        const std::string c =
            "Checklist " + std::to_string(e.checklist_done) + " / " + std::to_string(e.checklist.size());
        DrawText(false, m.meta, mx, ty, c, WithAlpha(th.subtext, alpha * 0.85f));
        mx += TextW(false, m.meta, c) + 18.0f * s;
      }
      ty += m.meta;
    }

    const bool is_open = expanded.count(a.key) > 0 && e.expandable;
    if (sel && e.expandable) {
      // "Details" / "Less" hint at the row's bottom right.
      const char* label = is_open ? "Less" : "Details";
      const float cy = y + e.h - m.row_pad - 10.0f * s;
      const float w = DrawHint(Action::kDetails, label, 0, cy, s * 0.85f, th, alpha, false);
      DrawHint(Action::kDetails, label, rx - w, cy, s * 0.85f, th, alpha);
    }
    if (is_open) {
      if (!e.checklist.empty()) {
        ty += 10.0f * s;
        const float col_w = m.text_w / e.columns;
        const float isz = 15.0f * s;
        for (size_t i = 0; i < e.checklist.size(); ++i) {
          const int col = static_cast<int>(i) % e.columns;
          const int line = static_cast<int>(i) / e.columns;
          const float x = tx + col * col_w;
          const float cy = ty + line * 26.0f * s + 13.0f * s;
          const auto& [item, done] = e.checklist[i];
          if (done) {
            canvas.FillCircle(x + 9.0f * s, cy, 9.0f * s, WithAlpha(th.accent, alpha));
            DrawCheck(x + 9.0f * s, cy, 4.8f * s, 1.8f * s, WithAlpha(OnColor(th.accent), alpha));
          } else {
            canvas.StrokeCircle(x + 9.0f * s, cy, 8.0f * s, 1.5f * s, WithAlpha(th.subtext, 0.6f * alpha));
          }
          DrawText(false, isz, x + 26.0f * s, cy - isz * 0.5f,
                   Canvas::EllipsizeText(*font, isz, item, col_w - 34.0f * s),
                   done ? WithAlpha(th.text, alpha) : WithAlpha(th.subtext, 0.8f * alpha));
        }
        const int lines = (static_cast<int>(e.checklist.size()) + e.columns - 1) / e.columns;
        ty += lines * 26.0f * s;
      }
      if (!e.credits.empty()) {
        ty += 10.0f * s;
        DrawText(false, m.meta, tx, ty, Canvas::EllipsizeText(*font, m.meta, e.credits, m.text_w),
                 WithAlpha(th.subtext, 0.85f * alpha));
      }
    }
  }

  void DrawViewer(float alpha) {
    const auto st = ChromeStyle();
    const Theme th = MakeTheme(*st);
    const Metrics m = MakeMetrics(*st);
    const float s = m.s;
    const float ease = alpha;
    // The canvas was just cleared, so the backdrop is a plain fill (within the clip).
    canvas.Fill({0, 0, width, height}, WithAlpha(th.backdrop, ease));
    // The panel rises a little as it fades in.
    const float rise = (1.0f - ease) * 24.0f * s;
    Metrics mm = m;
    auto shift = [&](RectF& r) {
      r.y0 += rise;
      r.y1 += rise;
    };
    shift(mm.panel);
    shift(mm.list);
    mm.tabs_y += rise;
    mm.filter_y += rise;
    mm.footer_y += rise;
    const RectF& P = mm.panel;
    canvas.FillRect({P.x0, P.y0 + 6.0f * s, P.x1, P.y1 + 10.0f * s}, WithAlpha(Color::Rgba(0, 0, 0, 90), ease),
                    m.radius + 4.0f * s);
    canvas.FillRect(P, WithAlpha(th.panel, ease), m.radius);
    canvas.StrokeRect(P, WithAlpha(th.border, ease), m.radius, 1.0f * s);

    // Header: title, totals and an overall bar.
    const float hx0 = P.x0 + m.pad, hx1 = P.x1 - m.pad;
    const float hy = P.y0 + m.pad;
    DrawText(true, m.title, hx0, hy, options.title, WithAlpha(th.text, ease));
    {
      const std::string count = std::to_string(totals.unlocked) + " / " + std::to_string(totals.count);
      const float cs = 22.0f * s, ls = 15.0f * s;
      std::string pts;
      if (totals.points) {
        const AchievementSet* ts = TabSet();
        const bool xbox = ts && tab_ids.size() && !tab_ids[tab].empty() && ts->platform == Platform::kXbox360;
        pts = std::to_string(totals.unlocked_points) + " / " + std::to_string(totals.points) + (xbox ? "G" : " pts");
      }
      float x = hx1;
      if (!pts.empty()) {
        x -= TextW(true, cs, pts);
        DrawText(true, cs, x, hy + 2.0f * s, pts, WithAlpha(th.text, ease));
        x -= 28.0f * s;
      }
      x -= TextW(false, ls, " unlocked");
      DrawText(false, ls, x, hy + 2.0f * s + (cs - ls) * 0.7f, " unlocked", WithAlpha(th.subtext, ease));
      x -= TextW(true, cs, count);
      DrawText(true, cs, x, hy + 2.0f * s, count, WithAlpha(th.text, ease));
      const float frac = totals.count ? static_cast<float>(totals.unlocked) / totals.count : 0.0f;
      canvas.ProgressBar({hx0, hy + 44.0f * s, hx1, hy + 51.0f * s}, frac, WithAlpha(th.track, ease),
                         WithAlpha(th.accent, ease));
    }

    // Set tabs.
    tab_hits.clear();
    {
      const float ty = mm.tabs_y, th_ = m.tab_h, ts = 17.0f * s;
      const bool many = tab_ids.size() > 1;
      float x0 = hx0, x1 = hx1;
      if (many) {
        const float gw = DrawGlyph(Action::kPrevSet, 0, 0, s, th, ease, false);
        DrawGlyph(Action::kPrevSet, hx0, ty + th_ * 0.5f, s, th, ease);
        DrawGlyph(Action::kNextSet, hx1 - gw, ty + th_ * 0.5f, s, th, ease);
        x0 += gw + 12.0f * s;
        x1 -= gw + 12.0f * s;
      }
      std::vector<float> widths;
      float total = 0;
      for (const auto& t : tab_titles) {
        const std::string label = Canvas::EllipsizeText(*font_bold, ts, t, 320.0f * s);
        widths.push_back(TextW(true, ts, label) + 36.0f * s);
        total += widths.back() + 8.0f * s;
      }
      // Scroll the strip so the current tab is in view.
      float offset = 0, before = 0;
      for (size_t i = 0; i < tab; ++i) before += widths[i] + 8.0f * s;
      if (total > x1 - x0 && tab < widths.size())
        offset = std::clamp(before + widths[tab] * 0.5f - (x1 - x0) * 0.5f, 0.0f, total - (x1 - x0));
      canvas.PushClip(RectF{x0, ty - 2.0f * s, x1, ty + th_ + 2.0f * s});
      float x = x0 - offset;
      for (size_t i = 0; i < tab_titles.size(); ++i) {
        const RectF r{x, ty, x + widths[i], ty + th_};
        const bool cur = i == tab;
        canvas.FillRect(r, cur ? WithAlpha(th.accent, ease) : WithAlpha(th.text, 0.07f * ease), th_ * 0.5f);
        const std::string label = Canvas::EllipsizeText(*font_bold, ts, tab_titles[i], 320.0f * s);
        DrawText(true, ts, x + 18.0f * s, ty + (th_ - ts) * 0.5f, label,
                 cur ? WithAlpha(OnColor(th.accent), ease) : WithAlpha(th.subtext, ease));
        tab_hits.push_back({RectF{std::max(r.x0, x0), r.y0, std::min(r.x1, x1), r.y1}, i});
        x += widths[i] + 8.0f * s;
      }
      canvas.PopClip();
    }

    // Filters and the secrets toggle.
    {
      const float fy = mm.filter_y, fh = m.filter_h, fs = 15.5f * s;
      float x = hx0;
      if (!credits_screen) {
        x += DrawGlyph(Action::kFilter, x, fy + fh * 0.5f, s, th, ease) + 10.0f * s;
        static const char* kNames[3] = {"All", "Unlocked", "Locked"};
        float w = 0;
        float seg[3];
        for (int i = 0; i < 3; ++i) w += seg[i] = TextW(true, fs, kNames[i]) + 32.0f * s;
        canvas.FillRect({x, fy, x + w, fy + fh}, WithAlpha(th.text, 0.07f * ease), fh * 0.5f);
        for (int i = 0; i < 3; ++i) {
          const RectF r{x, fy, x + seg[i], fy + fh};
          filter_hits[i] = r;
          if (i == filter)
            canvas.FillRect({r.x0 + 3.0f * s, r.y0 + 3.0f * s, r.x1 - 3.0f * s, r.y1 - 3.0f * s},
                            WithAlpha(Mix(th.panel, th.text, 0.22f), ease), (fh - 6.0f * s) * 0.5f);
          DrawText(true, fs, x + 16.0f * s, fy + (fh - fs) * 0.5f, kNames[i],
                   WithAlpha(i == filter ? th.text : th.subtext, ease));
          x += seg[i];
        }
        secrets_hit = {};
        if (options.allow_reveal_secrets) {
          const char* label = "Show secrets";
          const float bw = 20.0f * s;
          const float lw = TextW(false, fs, label);
          const float gw = DrawGlyph(Action::kSecrets, 0, 0, s, th, ease, false);
          float sx = hx1 - lw - bw - 10.0f * s - gw - 10.0f * s;
          secrets_hit = {sx, fy, hx1, fy + fh};
          DrawGlyph(Action::kSecrets, sx, fy + fh * 0.5f, s, th, ease);
          sx += gw + 10.0f * s;
          const RectF box{sx, fy + (fh - bw) * 0.5f, sx + bw, fy + (fh + bw) * 0.5f};
          if (reveal) {
            canvas.FillRect(box, WithAlpha(th.accent, ease), 4.0f * s);
            DrawCheck((box.x0 + box.x1) * 0.5f, (box.y0 + box.y1) * 0.5f, 5.5f * s, 2.0f * s,
                      WithAlpha(OnColor(th.accent), ease));
          } else {
            canvas.StrokeRect(box, WithAlpha(th.subtext, 0.8f * ease), 4.0f * s, 1.5f * s);
          }
          DrawText(false, fs, box.x1 + 10.0f * s, fy + (fh - fs) * 0.5f, label, WithAlpha(th.subtext, ease));
        }
      } else {
        for (auto& f : filter_hits) f = {};
        secrets_hit = {};
      }
    }

    // The list (or the credits).
    drawn_icons.clear();
    drawn_rows.clear();
    const RectF L = mm.list;
    const float sc = std::round(scroll);
    drawn_scroll = sc;
    canvas.PushClip(RectF{L.x0 - 4.0f * s, L.y0, L.x1 + 4.0f * s, L.y1});
    if (credits_screen) {
      for (const auto& c : credit_lines) {
        const float y = L.y0 + c.y - sc;
        if (y + c.size < L.y0 || y > L.y1) continue;
        DrawText(c.bold, c.size, L.x0 + m.row_pad, y, c.text, WithAlpha(c.dim ? th.subtext : th.text, ease));
      }
    } else if (rows.empty()) {
      const char* msg = sets.empty()   ? "No achievements yet."
                        : filter == 1 ? "Nothing unlocked yet."
                        : filter == 2 ? "Everything here is unlocked."
                                      : "No achievements in this set.";
      const float ms = 19.0f * s;
      DrawText(false, ms, (L.x0 + L.x1 - TextW(false, ms, msg)) * 0.5f, (L.y0 + L.y1) * 0.5f - ms, msg,
               WithAlpha(th.subtext, ease));
    }
    if (!credits_screen) {
      for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];
        const float y = L.y0 + e.y - sc;
        if (y + e.h < L.y0 || y > L.y1) continue;
        if (e.kind == Entry::kSet) {
          const float ts = 22.0f * s;
          const float yy = y + 14.0f * s;
          DrawText(true, ts, L.x0 + 4.0f * s, yy, e.title, WithAlpha(th.text, ease));
          const float ds = 15.0f * s;
          DrawText(false, ds, L.x1 - 4.0f * s - TextW(false, ds, e.detail), yy + (ts - ds) * 0.6f, e.detail,
                   WithAlpha(th.subtext, ease));
          canvas.FillRect({L.x0, y + e.h - 10.0f * s, L.x1, y + e.h - 8.0f * s}, WithAlpha(th.accent, 0.6f * ease));
        } else if (e.kind == Entry::kCategory) {
          const float ts = 15.0f * s;
          const float yy = y + e.h - ts - 10.0f * s;
          std::string title = e.title;
          for (char& ch : title)
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
          DrawText(true, ts, L.x0 + 4.0f * s, yy, title, WithAlpha(th.subtext, ease));
          const float tw = TextW(true, ts, title);
          DrawText(false, ts, L.x0 + 4.0f * s + tw + 10.0f * s, yy, e.detail, WithAlpha(th.subtext, 0.7f * ease));
        } else {
          const int row = static_cast<int>(std::find(rows.begin(), rows.end(), static_cast<int>(i)) - rows.begin());
          drawn_rows.push_back(row);
          DrawRow(e, y, row == selected, mm, th, ease);
        }
      }
    }
    canvas.PopClip();

    // Scrollbar.
    const float total = credits_screen ? credits_h : content_h;
    if (total > L.height()) {
      const float track_x = P.x1 - m.pad * 0.5f;
      const float h = std::max(30.0f * s, L.height() * L.height() / total);
      const float t = MaxScroll(m) > 0 ? sc / MaxScroll(m) : 0.0f;
      const float y = L.y0 + (L.height() - h) * std::clamp(t, 0.0f, 1.0f);
      canvas.FillRect({track_x - 2.0f * s, L.y0, track_x + 2.0f * s, L.y1}, WithAlpha(th.text, 0.06f * ease), 2.0f * s);
      canvas.FillRect({track_x - 2.5f * s, y, track_x + 2.5f * s, y + h}, WithAlpha(th.text, 0.35f * ease), 2.5f * s);
    }

    // Footer hints for the input in use.
    canvas.FillRect({P.x0 + m.pad, mm.footer_y, P.x1 - m.pad, mm.footer_y + 1.0f * s}, WithAlpha(th.border, ease));
    const float fy = mm.footer_y + (P.y1 - mm.footer_y) * 0.5f;
    float x = hx0;
    if (credits_screen) {
      x += DrawHint(Action::kMove, "Scroll", x, fy, s, th, ease) + 26.0f * s;
      DrawHint(Action::kCredits, "Achievements", x, fy, s, th, ease);
    } else {
      x += DrawHint(Action::kMove, "Move", x, fy, s, th, ease) + 26.0f * s;
      if (!rows.empty() && entries[rows[selected]].expandable)
        x += DrawHint(Action::kDetails, "Details", x, fy, s, th, ease) + 26.0f * s;
      if (options.show_credits) DrawHint(Action::kCredits, "Credits", x, fy, s, th, ease);
    }
    const float bw = DrawHint(Action::kBack, credits_screen ? "Back" : "Close", 0, fy, s, th, ease, false);
    DrawHint(Action::kBack, credits_screen ? "Back" : "Close", hx1 - bw, fy, s, th, ease);
  }

  // Icon textures for the rows on screen, to notice ones that finished loading.
  bool IconsChanged() {
    std::vector<void*> now;
    for (int row : drawn_rows) {
      const Entry& e = entries[rows[row]];
      const Achievement& a = *e.view.achievement;
      const std::filesystem::path base = e.view.set ? e.view.set->base_dir : std::filesystem::path();
      void* tex = nullptr;
      if (!e.hidden) {
        if (!e.unlocked && !a.locked_icon.empty()) tex = icons->Get(a.locked_icon, a.set_id, base);
        if (!tex) tex = icons->GetOrFallback(a.icon, a.set_id, base);
      } else {
        tex = icons->GetOrFallback(a.locked_icon, a.set_id, base);
      }
      now.push_back(tex);
    }
    return now != drawn_icons;
  }

  // ---- Popups ------------------------------------------------------------------

  const Font* PopupFont(const Style& st, bool bold) const {
    const std::string& name = bold ? st.font_bold : st.font;
    if (!name.empty())
      if (auto it = named_fonts.find(name); it != named_fonts.end() && it->second) return it->second.get();
    return bold ? font_bold.get() : font.get();
  }

  struct PopupDraw {
    ActivePopup popup;
    PopupRect rect;
  };

  std::vector<PopupDraw> LayoutPopupsNow() {
    std::vector<PopupDraw> out;
    if (open && !options.popups_over_viewer) return out;
    const auto active = center->Active();
    if (active.empty()) return out;
    // Layout in a virtual display when ui_scale isn't height / 1080.
    const float k = PopupK();
    const Vec2 size{width / k, height / k};
    std::vector<PopupRect> rects;
    {
      // One measurer for the whole stack: styles naming their own fonts are
      // rare, so the first popup's fonts stand in for all.
      const Style& st = *active.front().n.style;
      FontMeasurer fm(PopupFont(st, false), PopupFont(st, true));
      struct Scaled : TextMeasurer {
        const TextMeasurer& m;
        float k;
        Scaled(const TextMeasurer& mm, float kk) : m(mm), k(kk) {}
        float TextWidth(bool b, float size, std::string_view t) const override {
          return m.TextWidth(b, size * k, t) / k;
        }
      } scaled(fm, k);
      rects = LayoutPopups(active, {0, 0}, size, scaled);
    }
    for (size_t i = 0; i < active.size(); ++i) {
      PopupRect r = rects[i];
      if (active[i].n.popup().lines.icon)
        r.icon = icons->GetOrFallback(active[i].n.icon, active[i].n.set_id, active[i].n.icon_base_dir);
      out.push_back({active[i], r});
    }
    return out;
  }

  float PopupK() const { return ui_scale > 0 ? ui_scale / (height / 1080.0f) : 1.0f; }

  PixelRect PopupBounds(const PopupDraw& d) const {
    const float k = PopupK();
    const float m = (8.0f * d.rect.scale + 2.0f) * k;
    return Intersect({static_cast<int>(std::floor(d.rect.min.x * k - m)), static_cast<int>(std::floor(d.rect.min.y * k - m)),
                      static_cast<int>(std::ceil(d.rect.max.x * k + m)), static_cast<int>(std::ceil(d.rect.max.y * k + m))},
                     {0, 0, width, height});
  }

  std::vector<PopupSig> Signatures(const std::vector<PopupDraw>& draws) const {
    std::vector<PopupSig> out;
    for (const auto& d : draws) {
      const auto& r = d.rect;
      // Quarter pixels: anything finer can't change the image.
      auto q = [](float v) { return static_cast<int>(std::lround(v * 4.0f)); };
      float text = 1.0f;
      if (d.popup.n.popup().animation == Animation::kExpand) text = d.popup.expand;
      out.push_back({d.popup.n.serial, q(r.min.x), q(r.min.y), q(r.max.x), q(r.max.y),
                     static_cast<int>(std::lround(r.alpha * 255.0f)), static_cast<int>(std::lround(text * 255.0f)),
                     r.icon});
    }
    return out;
  }

  void DrawPopups(const std::vector<PopupDraw>& draws) {
    const float k = PopupK();
    for (const auto& d : draws) {
      const Style& st = *d.popup.n.style;
      CanvasPainter cp(canvas, PopupFont(st, false), PopupFont(st, true));
      if (k != 1.0f) {
        ScaledPainter sp(cp, k);
        DrawPopup(sp, d.popup, d.rect);
      } else {
        DrawPopup(cp, d.popup, d.rect);
      }
    }
  }

  // ---- Frame -----------------------------------------------------------------

  bool Frame(float dt, const OverlayInput& in) {
    ++stats.frames;
    clock += dt;
    if (width <= 0 || height <= 0 || !font || !font_bold) return false;
    RefreshBindings();
    HandleInput(dt, in);
    // Also while closing: the fading panel still reads the model.
    if (open || open_t > 0.0f) EnsureModel();

    if (options.advance_center && dt > 0 && !(open && !options.popups_over_viewer)) center->Update(dt);

    // Viewer animation and scrolling.
    const float target = open ? 1.0f : 0.0f;
    if (open_t != target) {
      const float speed = dt / 0.16f;
      open_t = open ? std::min(1.0f, open_t + speed) : std::max(0.0f, open_t - speed);
      viewer_dirty = true;
    }
    if (open) {
      const float diff = scroll_target - scroll;
      if (std::fabs(diff) < 0.5f) scroll = scroll_target;
      else scroll += diff * (1.0f - std::exp(-dt * 16.0f));
      if (std::round(scroll) != drawn_scroll) viewer_dirty = true;
      icon_poll += dt;
      if (icon_poll >= 0.25f) {
        icon_poll = 0;
        if (IconsChanged()) viewer_dirty = true;
      }
    }
    const bool viewer_visible = open_t > 0.0f;

    const auto draws = LayoutPopupsNow();
    auto sigs = Signatures(draws);
    const bool popups_changed = sigs != popup_sigs;

    if (!size_changed && !popups_changed && !(viewer_visible && viewer_dirty) && viewer_visible == was_viewer_visible)
      return false;

    const auto t0 = std::chrono::steady_clock::now();
    canvas.ResetDirty();
    PixelRect area;
    for (const auto& d : draws) area = Union(area, PopupBounds(d));
    const bool settled = viewer_visible && was_viewer_visible && open_t == 1.0f && drawn_open_t == 1.0f;
    if (settled && !size_changed) {
      // Fully open both frames: only the panel (and popups) can have changed.
      const auto st = ChromeStyle();
      const Metrics m = MakeMetrics(*st);
      const float g = 16.0f * m.s;
      const PixelRect panel{static_cast<int>(std::floor(m.panel.x0 - g)), static_cast<int>(std::floor(m.panel.y0 - g)),
                            static_cast<int>(std::ceil(m.panel.x1 + g)), static_cast<int>(std::ceil(m.panel.y1 + g))};
      const PixelRect region = Union(Union(panel, popup_area), area);
      canvas.Clear(region);
      canvas.PushClip(region);
      DrawViewer(1.0f);
      DrawPopups(draws);
      canvas.PopClip();
      viewer_dirty = false;
    } else if (viewer_visible || was_viewer_visible || size_changed) {
      canvas.Clear();
      if (viewer_visible) {
        const float e = open_t * open_t * (3.0f - 2.0f * open_t);
        DrawViewer(e);
      }
      DrawPopups(draws);
      viewer_dirty = false;
    } else {
      // Popups only: clear where they were and redraw where they are.
      const PixelRect region = Union(popup_area, area);
      canvas.Clear(region);
      canvas.PushClip(region);
      DrawPopups(draws);
      canvas.PopClip();
    }
    drawn_open_t = viewer_visible ? open_t : 0.0f;
    popup_area = area;
    popup_sigs = std::move(sigs);
    was_viewer_visible = viewer_visible;
    size_changed = false;
    last_dirty = canvas.dirty();
    const bool now_shown = viewer_visible || !popup_sigs.empty();
    if (now_shown && !shown) last_dirty = PixelRect{0, 0, width, height};
    shown = now_shown;
    ++stats.renders;
    stats.last_render_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
  }
};

// ---- Overlay ----------------------------------------------------------------------

Overlay::Overlay(System& system, OverlayOptions options) : impl_(std::make_unique<Impl>(system, std::move(options))) {}
Overlay::~Overlay() = default;

void Overlay::SetSize(int width, int height, float ui_scale) {
  Impl& m = *impl_;
  if (width == m.width && height == m.height && ui_scale == m.ui_scale) return;
  m.width = std::max(0, width);
  m.height = std::max(0, height);
  m.ui_scale = std::max(0.0f, ui_scale);
  m.canvas.Resize(m.width, m.height);
  m.size_changed = true;
  m.model_stale = true;
  m.viewer_dirty = true;
  m.popup_sigs.clear();
  m.popup_area = {};
}

bool Overlay::Frame(float dt, const OverlayInput& input) { return impl_->Frame(dt, input); }

const Image& Overlay::image() const {
  return impl_->options.premultiplied ? impl_->canvas.premultiplied() : impl_->canvas.image();
}

PixelRect Overlay::dirty_rect() const { return impl_->last_dirty; }
bool Overlay::visible() const { return impl_->open_t > 0.0f || !impl_->popup_sigs.empty(); }

void Overlay::Open() { impl_->SetOpen(true); }
void Overlay::Close() { impl_->SetOpen(false); }
void Overlay::Toggle() { impl_->SetOpen(!impl_->open); }
bool Overlay::is_open() const { return impl_->open; }
bool Overlay::wants_input() const { return impl_->open || impl_->release_latch; }

OverlayInput Overlay::GameInput(const OverlayInput& raw) const {
  const Impl& m = *impl_;
  if (m.open || m.release_latch) {
    OverlayInput none;
    none.mouse_present = raw.mouse_present;
    none.mouse_x = raw.mouse_x;
    none.mouse_y = raw.mouse_y;
    return none;
  }
  OverlayInput out = raw;
  if (m.toggle_key > 0 && m.toggle_key < 13) out.f[m.toggle_key] = false;
  SetPadButtons(out, m.held_back, false);
  SetPadButtons(out, m.tap, true);
  return out;
}

void Overlay::SetButtonLabels(ButtonLabels labels) {
  impl_->options.buttons = labels;
  if (!impl_->labels_from_style) impl_->labels = labels;
  impl_->viewer_dirty = true;
}

void Overlay::RegisterFont(std::string name, std::shared_ptr<Font> font) {
  impl_->named_fonts[std::move(name)] = std::move(font);
}

bool Overlay::ReloadStyles(std::string* error) {
  const bool ok = impl_->styles->Reload(error);
  impl_->bank.ClearCache();
  return ok;
}

NotificationCenter& Overlay::center() { return *impl_->center; }
StyleManager& Overlay::styles() { return *impl_->styles; }
IconCache& Overlay::icons() { return *impl_->icons; }
SoundBank& Overlay::sounds() { return impl_->bank; }
Overlay::Stats Overlay::stats() const { return impl_->stats; }

Overlay::ViewState Overlay::view_state() const {
  const Impl& m = *impl_;
  ViewState v;
  v.tab = m.tab < m.tab_ids.size() ? m.tab_ids[m.tab] : std::string();
  v.filter = m.filter;
  v.reveal_secrets = m.reveal;
  v.credits = m.credits_screen;
  v.selected = m.selected_key;
  for (int r : m.rows) {
    const auto& e = m.entries[r];
    v.keys.push_back(e.key);
    v.names.push_back(e.name);
  }
  v.expanded.assign(m.expanded.begin(), m.expanded.end());
  v.scroll = m.scroll;
  return v;
}

}  // namespace recomp::achievements::ui
