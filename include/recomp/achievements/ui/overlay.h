#pragma once

// The achievement viewer and popups, drawn in software into one RGBA image,
// for games with any renderer (or none of Dear ImGui). Each frame the game
// fills an OverlayInput, calls Frame(), and when it returns true uploads
// image() as a texture and blends it over its frame (straight alpha:
// src * a + dst * (1 - a)). See docs/overlay.md.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "recomp/achievements/library.h"
#include "recomp/achievements/system.h"
#include "recomp/achievements/ui/canvas.h"
#include "recomp/achievements/ui/icon_cache.h"
#include "recomp/achievements/ui/notification_center.h"
#include "recomp/achievements/ui/sounds.h"
#include "recomp/achievements/ui/style.h"
#include "recomp/achievements/ui/viewer_parts.h"

namespace recomp::achievements::app {
class App;  // recomp/achievements/app/app.h: the All games page.
}

namespace recomp::achievements::ui {

// What the game passes each frame: true while a key or button is held. Fill
// what you have; the rest stays false. Pad buttons use Xbox names
// (PlayStation: a = cross, b = circle, x = square, y = triangle, lb = L1,
// rb = R1, start = Start/Options, back = Select/Share).
struct OverlayInput {
  // Keyboard.
  bool f[13] = {};  // f[1]..f[12] = F1..F12; f[0] is unused.
  bool up = false, down = false, left = false, right = false;
  bool enter = false, space = false, escape = false, backspace = false;
  bool page_up = false, page_down = false, home = false, end = false;
  bool tab = false, shift = false;
  // Gamepad.
  bool pad_up = false, pad_down = false, pad_left = false, pad_right = false;
  bool pad_a = false, pad_b = false, pad_x = false, pad_y = false;
  bool pad_lb = false, pad_rb = false, pad_start = false, pad_back = false;
  float pad_lx = 0.0f, pad_ly = 0.0f;  // Left stick, -1..1, +y = down.
  float pad_ry = 0.0f;                 // Right stick y scrolls the list.
  // Mouse, in image pixels. Optional.
  bool mouse_present = false;
  float mouse_x = 0.0f, mouse_y = 0.0f;
  bool mouse_left = false;
  float mouse_wheel = 0.0f;  // Notches this frame, + = away from the player (scroll up).
};

// Pad buttons, for chords.
enum PadButton : uint32_t {
  kPadA = 1u << 0,
  kPadB = 1u << 1,
  kPadX = 1u << 2,
  kPadY = 1u << 3,
  kPadLB = 1u << 4,
  kPadRB = 1u << 5,
  kPadStart = 1u << 6,
  kPadBack = 1u << 7,
  kPadUp = 1u << 8,
  kPadDown = 1u << 9,
  kPadLeft = 1u << 10,
  kPadRight = 1u << 11,
};
uint32_t PadButtons(const OverlayInput& in);
// Sets or clears the OverlayInput fields for the buttons in `mask`.
void SetPadButtons(OverlayInput& in, uint32_t mask, bool down);

// "start+lb", "options+l1", "back", ... (names: a b x y lb rb start back up
// down left right, and cross circle square triangle l1 r1 options select
// share menu view). "none" or "" gives 0. False on an unknown name.
bool ParsePadChord(std::string_view text, uint32_t& out);
// "F1".."F12" gives 1..12; "none" or "" gives 0. False otherwise.
bool ParseToggleKey(std::string_view text, int& out);

struct OverlayOptions {
  // Styles and sounds, as in PopupUiOptions: the game's style file, the
  // player's folder (achievements_style.json, sounds/*.wav), extra sound
  // folders, and the player's global preferences for every game.
  std::filesystem::path game_style_file;
  std::filesystem::path user_dir;
  std::vector<std::filesystem::path> sound_dirs;
  bool global_preferences = true;
  bool create_global_preferences = true;  // Create GlobalConfigDir() with a starter file.
  // Icon for achievements without one, usually the game's desktop icon.
  std::string fallback_icon;
  // The shared achievement library every game writes to (docs/library.md).
  // On once library.game_id is set; its icons come from this overlay's
  // IconCache and its game icon defaults to fallback_icon.
  LibraryOptions library;

  // Parts the game already has; null = the overlay makes its own. A borrowed
  // center is neither attached to the System nor given sounds here, and a
  // borrowed IconCache must hand out CPU images (UseImageTextures()).
  NotificationCenter* center = nullptr;
  StyleManager* styles = nullptr;
  IconCache* icons = nullptr;
  bool advance_center = true;  // Frame() calls center.Update(dt).

  // Sounds. With play_sound the overlay resolves each popup's sound (override
  // WAVs included) and hands over the PCM for the game to mix; sound_handler
  // gets the raw requests instead. Neither: silent. Either way the player's
  // popup sound setting is applied first: nothing is sent while it's off, and
  // the volume passed already includes it (GetPopupSoundSettings()).
  std::function<void(std::shared_ptr<const Pcm> pcm, float volume, NotificationKind kind)> play_sound;
  NotificationCenter::SoundHandler sound_handler;

  // Toggle bindings; the player's "viewer" style keys override them.
  std::string toggle_key = "F8";
  std::string toggle_pad = "start+lb";
  // How long GameInput() holds back a chord button pressed on its own, so the
  // game doesn't act on the first half of the chord. A button released within
  // this time reaches the game as a one-frame tap; one held longer reaches it
  // late. 0 passes chord buttons straight through.
  float chord_grace = 0.15f;
  ButtonLabels buttons = ButtonLabels::kXbox;

  // Popups keep playing over the open viewer (true), or wait until it closes.
  bool popups_over_viewer = true;
  bool allow_reveal_secrets = true;
  bool show_credits = true;
  // The settings page (Start / F2, or the gear in the header). Hide it, or
  // the sections a game already offers in its own menus.
  bool show_settings = true;
  bool settings_sound = true;  // Popup sound on/off and volume.
  bool settings_sets = true;   // Turning each achievement set on and off.
  bool settings_reset = true;  // Resetting chosen achievements, a set, or everything.
  // How long a reset has to be held to go through.
  float reset_hold_seconds = 1.2f;
  // Sets that are off get tabs too, marked Off, with their rows dimmed; they
  // still leave the totals. For views of another game's data (the library
  // app); a game's own viewer hides them until they're turned back on.
  bool show_disabled_sets = false;
  std::string title = "Achievements";

  // The All games page: a last tab after the sets that opens every game in
  // the shared library, as the desktop app shows it, and starts other games
  // through Steam or directly (docs/launching.md). Shown once library.game_id
  // is set and the library is on.
  bool show_library_page = true;
  // Library folders it shows; empty = DefaultLibraryRoots().
  std::vector<std::filesystem::path> library_roots;
  // Called (from Frame()) after the player started another game with "Quit
  // current game" ticked: save and exit. Null: the box isn't offered and the
  // game keeps running.
  std::function<void()> on_quit_request;
  // Launch hooks, as in app::AppOptions: null/empty = the real ones. For tests.
  std::function<bool(const LaunchCommand& command, std::string* error)> start_game;
  std::function<std::vector<SteamShortcut>()> steam_shortcuts;
  std::filesystem::path launcher_prefs;

  // TTF/OTF bytes; empty = the bundled DejaVu Sans. A regular font without a
  // bold one draws bold text thickened.
  std::vector<uint8_t> font;
  std::vector<uint8_t> font_bold;
  // image() in premultiplied alpha (blend: src + dst * (1 - a)).
  bool premultiplied = false;
};

class Overlay {
 public:
  explicit Overlay(System& system, OverlayOptions options = {});
  ~Overlay();
  Overlay(const Overlay&) = delete;
  Overlay& operator=(const Overlay&) = delete;

  // The output image size (the game's back buffer or window). ui_scale sizes
  // the viewer (DPI, 4K); 0 = height / 1080. Popups scale with their style.
  void SetSize(int width, int height, float ui_scale = 0.0f);

  // Handles the toggle and navigation, advances popups and animation, and
  // redraws what changed. True when image() changed and should be uploaded.
  bool Frame(float dt, const OverlayInput& input);
  const Image& image() const;
  // What changed in the last Frame() that returned true. The whole image when
  // the overlay becomes visible again, so a game that stops uploading while
  // nothing shows never keeps stale pixels.
  PixelRect dirty_rect() const;
  // Anything on screen (popups or viewer); false means image() is clear.
  bool visible() const;

  void Open();
  // Opens the viewer on its settings page (e.g. from the game's own menu).
  void OpenSettings();
  void Close();
  void Toggle();
  bool is_open() const;
  // True while the viewer is open, and after it closes until every key and
  // button is released (so the key that closed it isn't a fresh press for the
  // game): keep input from the game.
  bool wants_input() const;
  // What the game should see this frame, after Frame(): nothing while
  // wants_input(), the toggle key removed, chord buttons held back for
  // chord_grace, and late taps of them added back. Call once per frame.
  OverlayInput GameInput(const OverlayInput& raw) const;

  void SetButtonLabels(ButtonLabels labels);
  // A button of the host app's beside the title, e.g. a profile picker:
  // clicking it, or Start / F2 when the settings page is off, calls on_press
  // (from inside Frame(); it may call SetHostButton again). hint labels it in
  // the footer (empty: label). An empty label or null on_press removes it.
  void SetHostButton(std::string label, std::string hint, std::function<void()> on_press);
  // A font a style can pick by name ("font": "...").
  void RegisterFont(std::string name, std::shared_ptr<Font> font);
  // Re-reads the style files and drops cached sounds.
  bool ReloadStyles(std::string* error = nullptr);

  NotificationCenter& center();
  StyleManager& styles();
  IconCache& icons();
  SoundBank& sounds();
  // Null unless options.library is on.
  Library* library();
  // Opens the viewer on the All games page (e.g. from the game's own menu).
  // False when there's no library.
  bool OpenLibraryPage();
  // The All games page while it's showing, else null. For tests and tools.
  app::App* library_page();

  // What the viewer shows, for tests and tools.
  struct ViewState {
    std::string tab;  // Set id; "" = all sets.
    int filter = 0;   // 0 all, 1 unlocked, 2 locked.
    bool reveal_secrets = false;
    bool credits = false;
    std::string selected;            // Key of the selected achievement.
    std::vector<std::string> keys;   // Listed achievements, in order.
    std::vector<std::string> names;  // As shown (secret text for hidden ones).
    std::vector<std::string> expanded;
    float scroll = 0.0f;  // Pixels.
    // The settings page and the reset mode of the list.
    bool settings = false;
    bool reset_mode = false;   // The list picks achievements to reset.
    std::vector<std::string> tabs;  // Set ids; "" = all sets; ":library" = All games.
    // Settings rows, in order: "sound", "volume", "set:<id>", "reset:pick",
    // "reset:set:<id>", "reset:all".
    std::vector<std::string> setting_rows;
    std::string setting_selected;
    float hold = 0.0f;  // 0..1 while a reset is being held.
    std::string host_button;  // SetHostButton's label; empty when none.
    bool library_page = false;  // The All games page is showing.
  };
  ViewState view_state() const;

  struct Stats {
    uint64_t frames = 0;   // Frame() calls.
    uint64_t renders = 0;  // Calls that redrew something.
    double last_render_ms = 0.0;
  };
  Stats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace recomp::achievements::ui
