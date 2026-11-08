# ImGui hosts

A game that draws with Dear ImGui shows the popups with `ImGuiRenderer` and the list with
`AchievementListWindow`, usually through `PopupUi`. To make it look and behave like a game that uses the
software overlay ([overlay.md](overlay.md)), wire these four things.

## 1. Open the list the same way

```cpp
ui::PopupUiOptions options;               // toggle_key "F8", toggle_pad "start+lb", chord_grace 0.15 s
ui::PopupUi achievements_ui(achievements, options);

// Every frame, before the game reads input:
ui::OverlayInput raw = MyInputToOverlayInput();  // Keys, pad buttons, sticks.
achievements_ui.HandleInput(dt, raw);
const ui::OverlayInput game = achievements_ui.GameInput(raw);
// ...the game acts on `game`, not `raw`.

// Inside the ImGui frame:
achievements_ui.Draw();  // Popups, and the list while list_open().
```

`HandleInput` runs the same `ui::ViewerToggle` (`viewer_toggle.h`) that the overlay uses, so every game
opens the list the same way:

- **Toggle:** F8 or Start + LB opens and closes the list. The chord fires when its last button goes
  down, in either order, and holding it doesn't repeat.
- **Back:** B or Esc closes the list, unless one of its dialogs is open (ImGui closes that first).
- **Player's bindings:** the player's `viewer` style keys (`"key"`, `"pad"`) replace the defaults, in
  the game's style file or the player's [global preferences](styling.md).
- **What the game sees (`GameInput`):**
  - nothing while the list is open;
  - nothing after it closes, until every key and button is released, so the press that closed it
    isn't a fresh press for the game;
  - never the toggle key;
  - the chord's buttons, but only after `chord_grace`. LB pressed on its own reaches the game late,
    or as a one-frame tap if it is released sooner, so LB + Start doesn't also act in the game.
- **Game's own menu:** a game can open or close the list from its menu with `SetListOpen()`. Closing
  latches input the same way.
- **Own flag:** `Draw(bool*)` still takes a flag for hosts that keep their own. Hosts that use it don't
  get the toggle.

A host that doesn't use `PopupUi` uses `ui::ViewerToggle` directly: `Update(dt, raw, open)` returns
true on the frame to toggle, `Closed()` latches after closing, and `GameInput(raw, open)` filters input.

## 2. Let ImGui's navigation drive the list

With the list open, pass the pad to ImGui as usual (`ImGuiConfigFlags_NavEnableGamepad`). The list is
plain ImGui windows, tabs and buttons. Feed ImGui the raw input while `wants_input()` is true, and the
game `GameInput()`.

## 3. Popup size on small screens

Popups scale with display height / `reference_height` (1080). A Steam Deck's 800 lines would shrink them
to 0.74, so on screens that short they don't go below `small_screen_scale` (1.0). Under
`small_screen_height` (800) the floor shrinks in proportion: 720 lines give 0.9. The same rule sizes the
software overlay's viewer and the library app.

There's nothing to wire: `ImGuiRenderer` reads it from the style. Remove any fixed scale a game set as a
stopgap. Players who want handheld popups larger set `"small_screen_scale": 1.3` in their global
`achievements_style.json`, and `"small_screen_height": 0` turns the rule off ([styling.md](styling.md)).

## 4. The rest is shared

Styles, sounds, the player's global preferences, the settings (theme and sizes, sound, sets, resets) and the
[library](library.md) are the same objects in both kinds of host. `PopupUiOptions::library.game_id` turns
the library on, the same as `OverlayOptions::library`.

The overlay's [All games page](overlay.md#all-games) is drawn in software. An ImGui game that wants it
can run a `ui::Overlay` beside `PopupUi` for the page alone, or open the desktop app.
