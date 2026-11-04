# Achievement overlay (no ImGui needed)

`ui::Overlay` draws the achievement viewer and the unlock/progress popups in software, into one RGBA
image the size of the game's window. It needs no Dear ImGui, no SDL and no GPU API, so any recomp can use
it: an SDL2 or SDL3 game, a PSP recomp that blends a CPU canvas in its Vulkan present shim, a game with
its own D3D or GL renderer. The popups look the same as the ImGui renderer's (both draw through
`painter.h`), and the viewer takes its colours from the same styles.

The game's whole job, every frame:

1. fill an `OverlayInput` with a few key and button states,
2. call `Frame(dt, input)`,
3. when it returns true, upload `image()` as a texture (or just `dirty_rect()` of it),
4. draw that texture over the frame with alpha blending,
5. pass the game `GameInput(input)` instead of the raw input.

It lives in `recomp::achievements_ui_base` (`include/recomp/achievements/ui/overlay.h`).

## SDL3

```cpp
#include "recomp/achievements/ui/overlay.h"
#include "recomp/achievements/ui/sdl_support.h"
#include "recomp/achievements/ui/sound_player.h"
using namespace recomp::achievements;

ui::SoundPlayer player;  // SDL3 audio streams
ui::OverlayOptions options;
options.user_dir = user_dir;                     // achievements_style.json, sounds/*.wav
options.fallback_icon = "assets/icon.png";       // icon for achievements without one
options.play_sound = [&](auto pcm, float volume, auto kind) { player.Play(*pcm, volume, kind); };
ui::Overlay overlay(achievements, options);

// Every frame:
overlay.SetSize(width, height);
ui::OverlayInput in;
ui::FillOverlayInput(in, SDL_GetKeyboardState(nullptr), gamepad);  // gamepad may be null
if (overlay.Frame(dt, in)) UpdateTexture(texture, overlay.image(), overlay.dirty_rect());
DrawFullscreenQuad(texture);                       // blend: src * a + dst * (1 - a)
const ui::OverlayInput for_game = overlay.GameInput(in);  // what the game should see
```

`ui::ButtonLabelsFor(gamepad)` tells PlayStation pads apart; pass it to `overlay.SetButtonLabels()` so the
hints show cross/circle/square/triangle.

## Any other input layer

```cpp
ui::OverlayOptions options;
options.buttons = ui::ButtonLabels::kPlayStation;  // a PSP/PS3 recomp
options.play_sound = [&](auto pcm, float volume, auto) { my_mixer.Queue(pcm->samples, volume); };
ui::Overlay overlay(achievements, options);

// Every frame, in the present shim:
ui::OverlayInput in;
in.f[8] = KeyDown(KEY_F8);
in.pad_start = buttons & CTRL_START;  in.pad_lb = buttons & CTRL_LTRIGGER;
in.pad_a = buttons & CTRL_CROSS;      in.pad_b = buttons & CTRL_CIRCLE;
in.pad_up = buttons & CTRL_UP;        in.pad_down = buttons & CTRL_DOWN;
in.pad_ly = analog_y;                 // -1..1, +y down
overlay.SetSize(swapchain_width, swapchain_height);
if (overlay.Frame(dt, in)) UploadToStagingImage(overlay.image().rgba.data());
BlendOverFrame();
```

For a game with its own input bits, `GameInput()` returns the filtered `OverlayInput`, which the game turns
back into its own bits (`ui::PadButtons()` gives the pad as a mask).

Games that open the list from their own menu (or from `XamShowAchievementsUI`) call `Open()`;
`Close()`, `Toggle()` and `is_open()` do the rest. `PCM` is 48 kHz stereo float (`ui::Pcm`).

## What the game sees

`GameInput(raw)` returns the input the game should get this frame. Call it once per frame, after `Frame()`:

- **While the viewer is open, nothing.** After it closes, nothing either until every key and button is
  released (`wants_input()` stays true until then), so the B or Esc that closed it isn't a fresh press.
- **The toggle key (F8) never reaches the game.**
- **A chord button pressed on its own is held back for `chord_grace`** (0.15 s), so the game doesn't act
  on the first half of Start+LB. Held longer, it reaches the game late; released sooner, the game gets it
  as a one-frame tap. Set `chord_grace = 0` to pass chord buttons straight through.

Games that withhold input their own way can keep checking `wants_input()`.

## Uploading only what changed

`Frame()` returns true only when `image()` changed, and `dirty_rect()` is the area to upload. When nothing
shows (`visible()` is false), a game can hide its layer and stop uploading. When the overlay shows again,
`dirty_rect()` covers the whole image, so a texture that missed the final clear is fully refreshed.

## Input

Fill what you have and leave the rest false. Buttons use Xbox names for positions: `pad_a` is the bottom
face button (cross on PlayStation), `pad_b` the right one (circle), `pad_x` the left (square), `pad_y` the
top (triangle).

| `OverlayInput` | SDL3 (`FillOverlayInput`) | SDL2 | XInput |
|---|---|---|---|
| `f[1]`..`f[12]` | `SDL_SCANCODE_F1`..`F12` | same scancodes (`SDL_GetKeyboardState`) | `VK_F1`..`VK_F12` |
| `up` `down` `left` `right` | arrow scancodes | same | `VK_UP` ... |
| `enter` `space` `escape` `backspace` `tab` `shift` | `RETURN`/`KP_ENTER`, `SPACE`, `ESCAPE`, `BACKSPACE`, `TAB`, `LSHIFT`/`RSHIFT` | same | `VK_RETURN` ... |
| `page_up` `page_down` `home` `end` | `PAGEUP`, `PAGEDOWN`, `HOME`, `END` | same | `VK_PRIOR`, `VK_NEXT`, `VK_HOME`, `VK_END` |
| `pad_a` `pad_b` `pad_x` `pad_y` | `SDL_GAMEPAD_BUTTON_SOUTH`/`EAST`/`WEST`/`NORTH` | `SDL_CONTROLLER_BUTTON_A`/`B`/`X`/`Y` | `XINPUT_GAMEPAD_A`/`B`/`X`/`Y` |
| `pad_lb` `pad_rb` | `LEFT_SHOULDER`, `RIGHT_SHOULDER` | `LEFTSHOULDER`, `RIGHTSHOULDER` | `LEFT_SHOULDER`, `RIGHT_SHOULDER` |
| `pad_start` `pad_back` | `START`, `BACK` | `START`, `BACK` | `START`, `BACK` |
| `pad_up` ... `pad_right` | `DPAD_UP` ... | `DPAD_UP` ... | `DPAD_UP` ... |
| `pad_lx` `pad_ly` `pad_ry` | `LEFTX`, `LEFTY`, `RIGHTY` / 32767 | `SDL_CONTROLLER_AXIS_*` / 32767 | `sThumbLX / 32767`, `-sThumbLY / 32767`, `-sThumbRY / 32767` (XInput's +y is up) |
| `mouse_*` | your mouse position in image pixels, left button, wheel notches (+ = away from the player) | | |

## Bindings and controls

The viewer toggles with **F8** and with **Start + LB** (Options + L1). The chord fires once when its last
button goes down, in either order, and holding it doesn't repeat. Buttons still held when the viewer
opens do nothing until they're released. When LB was pressed first while the viewer was open, it may have
switched the set before Start completed the chord; that switch is undone as the viewer closes. The game
still sees the first button of the chord (it can't know a chord is coming), so a game whose Start opens
a pause menu may want `toggle_pad = "back"` or a chord without Start.

Change them with `OverlayOptions::toggle_key` / `toggle_pad` (`"F1"`..`"F12"`, `"start+lb"`, `"back"`,
`"none"`), and players can override both in their global preferences:

```json
{ "viewer": { "key": "F10", "pad": "back", "buttons": "playstation" } }
```

in `achievements_style.json` (see [styling.md](styling.md#viewer-keys-viewer)).

| Action | Controller | Keyboard | Mouse |
|---|---|---|---|
| Move | D-pad, left stick (repeats while held) | Up, Down, Page Up/Down, Home, End | hover |
| Scroll | right stick | | wheel |
| Previous / next set | LB / RB, D-pad left/right | Left / Right | click a tab |
| Filter: All, Unlocked, Locked | X (square) | Tab, Shift+Tab | click |
| Show secrets | Y (triangle) | Space | click |
| Details (checklist, credits, full text) | A (cross) | Enter | click a row |
| Credits | Back / Select | F1 | |
| Close / back | B (circle) | Esc, Backspace | click outside |

The footer shows hints for the device used last (keyboard or controller), with Xbox labels unless the
game says `ButtonLabels::kPlayStation` (or the player's `viewer.buttons` does).

## What the viewer shows

A centred panel over a dimmed game: the title and totals (unlocked count, points, a bar), one tab per set
plus "All" when there are several, the filter and secrets toggle, then the list by set and category.
Each row has the icon (the locked icon, or the icon dimmed while locked; the fallback icon or a trophy
when there's none), the name, the description (or the secret text), points or the PS3 tier, the unlock
time, a progress bar with its count, and a checklist count. Details expand the checklist (ticked items
marked), the achievement's credits and the full description. The credits screen lists each
set's credits and the per-achievement credits. Hidden secrets show the style's `secret.name` and their
locked description until "Show secrets" is on, the same rules as the ImGui list.

Popups keep playing **over** the open viewer, so an unlock while browsing shows up and its row updates.
With `popups_over_viewer = false` they wait (the center isn't advanced) until the viewer closes.

## Options

| `OverlayOptions` | Default | Meaning |
|---|---|---|
| `game_style_file`, `user_dir`, `sound_dirs` | empty | As in `PopupUiOptions`: style files and replacement WAV folders. |
| `global_preferences`, `create_global_preferences` | true, true | Load the player's preferences for every game, and create that folder with a commented starter file. |
| `fallback_icon` | empty | Icon for achievements without one (usually the game's desktop icon). |
| `center`, `styles`, `icons` | null | Borrow the game's own instead of making new ones. A borrowed center isn't attached or given sounds; a borrowed `IconCache` must hand out CPU images (`ui::UseImageTextures(cache)`). |
| `advance_center` | true | `Frame()` advances the popups. |
| `play_sound` | null | Gets each popup's PCM (override WAVs applied) to mix yourself. |
| `sound_handler` | null | Gets the raw `SoundRequest`s instead. |
| `toggle_key`, `toggle_pad` | `"F8"`, `"start+lb"` | Bindings (the player's `viewer` style wins). |
| `buttons` | `kXbox` | Hint labels. |
| `popups_over_viewer` | true | See above. |
| `allow_reveal_secrets`, `show_credits` | true, true | |
| `title` | `"Achievements"` | Panel title. |
| `font`, `font_bold` | empty | TTF/OTF bytes; empty uses the bundled DejaVu Sans. A regular font with no bold one is thickened for bold text. Code points a game font lacks fall back to DejaVu. |
| `premultiplied` | false | `image()` in premultiplied alpha (blend `src + dst * (1 - a)`). |

`SetSize(width, height, ui_scale)`: `ui_scale` 0 means `height / 1080`. Pass more for high-DPI windows; the
viewer never goes below 0.75 so it stays readable at 720p. `RegisterFont(name, font)` makes a font
available to styles that name one (`"font": "..."`).

## Performance

`Frame()` redraws only what changed: input that moved something, animation, scrolling, a System change
(unlocks, progress, sets) or an icon that finished loading. With the viewer closed and no popups it
returns false right away. Popup frames clear and redraw only the popups' rectangles; with the viewer open
and settled, only the panel is redrawn. `dirty_rect()` is the area to re-upload.

Measured with `examples/overlay_demo --bench` (Release, GCC 15, one thread on a Ryzen 9 7945HX laptop CPU):

| 1920x1080 | Time |
|---|---|
| Viewer open, selection moves (full panel redraw + straight-alpha conversion) | ~4.7 ms |
| Viewer open, nothing changing | 0 redraws in 300 frames, ~0.02 ms per `Frame()` |
| Xbox popup in/out (60 of 420 frames redraw) | ~0.3 ms per redraw |
| Closed, no popups | 0 redraws, ~0.1 us per `Frame()` |

The game uploads the image only when `Frame()` returns true; during a popup's hold and while the viewer
sits still, nothing is uploaded.

## Fonts

DejaVu Sans and DejaVu Sans Bold (`assets/fonts`, Bitstream Vera licence with public-domain DejaVu
changes, see `assets/fonts/LICENSE`) are compiled into the library by CMake, so no font file ships with the
game. Without CMake, generate the two sources with
`cmake -DINPUT=assets/fonts/DejaVuSans.ttf -DOUTPUT=DejaVuSans.cpp -DSYMBOL=kDejaVuSans -P cmake/embed_file.cmake`
(and the same for `DejaVuSans-Bold.ttf` / `kDejaVuSansBold`), or define `RECOMP_ACH_NO_EMBEDDED_FONTS=1` and
pass `OverlayOptions::font` (CMake: `-DRECOMP_ACH_EMBED_FONTS=OFF`); with neither, nothing is drawn. The text is rasterised with stb_truetype, compiled `static` into the SDK so it
never clashes with a game's own copy (or ImGui's).

## Building blocks

- `ui::Canvas` (`canvas.h`): the rasteriser, usable on its own: anti-aliased rects, rounded rects, circles,
  lines, triangles, progress bars, bilinear image blits with tint and rounding, clipping, UTF-8 text with
  measuring, wrapping and ellipsis, and dirty tracking.
- `ui::Font`: TrueType fonts with a glyph cache per size.
- `ui::CanvasPainter` / `painter.h`: the popup look over a Canvas, as `ImGuiPainter` does over ImGui.

`examples/overlay_demo` drives an Overlay with scripted input and writes raw frames for ffmpeg.
