# Popup styling

Unlock and progress popups are drawn from a **Style**: plain data describing
where a popup sits, how it animates, its colours, which lines of text it shows
and which sounds it plays. Games and players change it with partial JSON files
that are merged field by field, so a file only needs the keys it changes.

## Layers

For each achievement set the style is resolved from, lowest first:

1. **Built-in theme**: `xbox360`, `ps3`, `retro`, `minimal` or `classic`.
   With no theme named anywhere, the set's platform picks one:

   | Platform                | Default theme |
   |-------------------------|---------------|
   | Xbox 360 (XDBF/SPA)     | `xbox360`     |
   | PlayStation 3 (TROPHY)  | `ps3`         |
   | RetroAchievements       | `retro`       |
   | Custom (game JSON/code) | `minimal`     |

2. **Game style JSON**: shipped with the game (`StyleManager::LoadGameStyleFile`
   or `SetGameStyle`).
3. **Game hook**: a C++ callback that edits the Style (`SetGameHook`).
4. **User style JSON**: the player's file, by convention
   `<user dir>/achievements_style.json` (`LoadUserStyleFile`). A missing file is
   fine.

Each JSON layer is applied as: its root object, then `platforms.<platform>`,
then `sets.<set id>`. Platform keys are `custom`, `xbox360`, `ps3`, `retro`.

**Choosing a theme.** The highest patch that contains `"theme"` decides the
base theme, and every patch below it is skipped, because those patches were
tuning a different theme. So a player who writes `"theme": "ps3"` gets the
plain PS3 look plus their own tweaks, not the game's colours on top of it.
`"theme": "auto"` means "the platform's default". `StyleManager::SetThemeOverride`
(e.g. from an in-game settings menu) forces a theme and keeps only the user
layer's tweaks.

Files may contain `//` and `/* */` comments. Keys starting with `_` or `$`, and
`comment`, are ignored. Unknown keys and wrong types are reported through
`StyleManager::warnings()` and otherwise ignored; the rest of the file still
applies.

## Top-level keys

| Key                | Type    | Meaning |
|--------------------|---------|---------|
| `theme`            | string  | Base theme (see above). |
| `scale`            | number  | Extra multiplier on every size. |
| `reference_height` | number  | Sizes are pixels at this display height (default 1080) and scale with the real height. |
| `min_scale`, `max_scale` | number | Clamp for that automatic scale. |
| `max_queued`       | int     | Unlock popups that may wait; the oldest waiting one is dropped beyond this. |
| `font`, `font_bold`| string  | Font names the game registered with `ImGuiRenderer::RegisterFont`. |
| `unlock`           | object  | Popup style for unlocks (below). |
| `progress`         | object or bool | Popup style for progress steps, plus the keys below. `false` turns them off. |
| `secret`           | object  | `name`, `description`: shown in progress popups of locked secret achievements. |
| `sound`            | object or bool | Sounds (below). `false` mutes popups. |
| `tiers`            | object  | Trophy tier colours/names: `"gold": "#E8BD30"` or `"gold": {"color": "#E8BD30", "name": "Gold"}`. |
| `platforms`        | object  | Per-platform patches. |
| `sets`             | object  | Per-set patches, keyed by set id. |

Extra keys inside `progress`:

| Key             | Default | Meaning |
|-----------------|---------|---------|
| `enabled`       | true    | Show progress popups at all. |
| `max_queued`    | 8       | Waiting progress popups. |
| `coalesce`      | true    | A newer step of the same achievement replaces a waiting one, or updates the one on screen (which then holds again). |
| `skip_complete` | true    | Don't show the final step; the unlock popup covers it. |

## Popup keys (`unlock` and `progress`)

| Key | Type | Meaning |
|-----|------|---------|
| `anchor` | string | `top_left`, `top_center`, `top_right`, `middle_left`, `center`, `middle_right`, `bottom_left`, `bottom_center`, `bottom_right` (also `top`, `bottom`, `left`, `right`, and `-` or `centre` spellings). |
| `offset` | [x, y] | Distance from the anchored edges, toward the centre. Also `offset_x`, `offset_y`. |
| `width`, `min_height` | number | Card size. The height grows to fit the text. |
| `padding`, `rounding`, `border` | number | Inner padding, corner radius, border thickness (0 = none). |
| `icon_size`, `icon_rounding` | number | Icon edge and corner radius. |
| `icon_shape` | string | `square` or `circle`. |
| `badge` | string | Text in the fallback badge drawn when there is no icon; `""` draws a small cup. |
| `font_scale` | number | Multiplier on all four font sizes. |
| `font_sizes` | object | `title`, `name`, `points`, `description`. |
| `description_lines` | int | Description wraps to this many lines, the last one ellipsised. |
| `uppercase_title` | bool | Upper-case the title line. |
| `shadow` | bool | Drop shadow. |
| `points_pill` | bool | Points in a rounded pill on the right instead of on the first line. |
| `tier_colors` | bool | Colour points text and fallback badge with the trophy tier. |
| `timing` | object | `in`, `hold`, `out` in seconds. |
| `animation` | string | `none`, `fade`, `slide` (from the nearest edge), `expand` (the card grows out of its icon), `pop` (scales up). |
| `slide_distance` | number | How far `slide` travels. |
| `max_visible` | int | More than 1 stacks popups away from the anchor. Unlock and progress popups at the same anchor and offset share one stack. |
| `spacing` | number | Gap between stacked popups. |
| `colors` | object | `background`, `border`, `shadow`, `title`, `name`, `description`, `points`, `accent` (fallback badge), `icon_border`, `bar_track`, `bar_fill`. |
| `lines` | object | Booleans: `title`, `name`, `points` (or `tier`), `description`, `icon`, `bar` (progress bar). |
| `text` | object | Format strings for `title`, `name`, `points`, `description`. |

Colours are `"#RRGGBB"`, `"#RRGGBBAA"` or `[r, g, b]` / `[r, g, b, a]` in 0..255.

### Text formats

Tokens: `{name}` `{description}` `{points}` `{tier}` `{set}` `{item}`
`{current}` `{total}` `{percent}`. `{points}` is empty for 0 points, `{tier}`
is empty for non-trophies, and the step tokens are empty on unlock popups.

Square brackets mark optional text that disappears when a token inside it is
empty: `"[{points}G - ]{name}"` reads `15G - Street Legend`, or just `Street Legend`
for a 0-point achievement. A line that ends up blank is not drawn.

Progress popups draw: title line (with points/count right-aligned), the bar,
then the name line, e.g. `Kickflip  3/14`, bar, `Flip Trick Encyclopedia`.
That's the default `text` for progress: `{"title": "{item}", "name": "{name}",
"points": "{current}/{total}"}`.

Popups never show credits; the list window does. A locked secret achievement's
progress popup shows `secret.name` and its locked description (or
`secret.description`) and its locked icon instead of its real name.

## Sounds

```json
"sound": {
  "enabled": true,
  "volume": 0.8,
  "unlock": "builtin:xbox360",
  "progress": "builtin:tick",
  "bronze": "", "silver": "", "gold": "", "platinum": ""
}
```

A sound ref is `"builtin:<name>"`, a path to a WAV file (relative paths resolve
from the JSON file that names them), or `"none"` / `null` for silence. The tier
keys override `unlock` for trophies of that tier; empty means "use `unlock`".

Built-in sounds are synthesised at runtime. They are our own sounds, made to
suggest each platform's feel; no console audio is included.

| Name | Used by |
|------|---------|
| `xbox360` | xbox360 unlock: a soft rising pop into a bright two-note chime |
| `ps3_bronze`, `ps3_silver`, `ps3_gold`, `ps3_platinum` | ps3 unlocks: one clear ping, richer per tier; platinum is an arpeggio |
| `retro` | retro unlock: a short chip-tune arpeggio |
| `minimal` | minimal unlock: two soft bell notes |
| `classic` | classic unlock: the My Game recomp's board-landing chord |
| `tick`, `ps3_tick`, `retro_tick`, `classic_tick` | progress steps |

**Replacing sounds without editing JSON.** Put WAV files in an override folder
(`<user dir>/sounds/` with `PopupUi`, or any folder given to
`SoundBank::AddOverrideDir`; later folders win):

| File | Replaces |
|------|----------|
| `<name>.wav` (e.g. `xbox360.wav`, `ps3_gold.wav`) | that built-in, wherever a style uses it |
| `unlock.wav` | every unlock sound |
| `unlock_bronze.wav` ... `unlock_platinum.wav` | unlock sounds for that tier |
| `progress.wav` | every progress sound |

A style that says `"none"` stays silent even if override files exist. WAVs are
decoded with SDL3 (`SDL_LoadWAV`), so any format SDL reads works; without SDL3
sounds are skipped.

## Examples

### Player: move popups, quieter, no step popups

`achievements_style.json` in the user folder:

```json
{
  "unlock": { "anchor": "top_left", "offset": [24, 24] },
  "progress": false,
  "sound": { "volume": 0.4 }
}
```

### Player: PS3 look for everything, my own sound for golds

```json
{
  "theme": "ps3",
  "sound": { "gold": "sounds/my_gold.wav" }
}
```

### Game: keep the My Game look, but platform look for DLC packs

The game's `achievements_style.json`:

```json
{
  "theme": "classic",
  "sets": {
    "my_game": { "unlock": { "text": { "title": "Achievement unlocked" } } }
  },
  "platforms": {
    "retro": { "theme": "auto" }
  }
}
```

### Game: Xbox look with descriptions and stacked popups

```json
{
  "theme": "xbox360",
  "unlock": {
    "lines": { "description": true },
    "text": { "description": "{description}" },
    "rounding": 12,
    "max_visible": 3,
    "animation": "slide"
  }
}
```

### RetroAchievements-style corner stack with custom colours

```json
{
  "theme": "retro",
  "unlock": {
    "anchor": "top_right",
    "colors": { "border": "#4CC9F0", "title": "#4CC9F0", "points": "#4CC9F0" }
  },
  "tiers": { "gold": { "color": "#FFD166", "name": "Hardcore" } }
}
```

## From code

```cpp
using namespace recomp::achievements::ui;

StyleManager styles;
styles.LoadGameStyleFile("assets/achievements_style.json");
styles.LoadUserStyleFile(user_dir / "achievements_style.json");

// Edits after the game JSON, before the player's file.
styles.SetGameHook([](Style& s, const AchievementSet* set) {
  if (set && set->id == "dlc1") s.unlock.colors.border = Color::Rgba(255, 120, 0);
});

// A settings-menu theme picker.
styles.SetThemeOverride("ps3");

// Print the fully resolved style, e.g. as a starting point for a user file.
std::string dump = StyleToJson(*styles.Resolve(nullptr)).dump(2);
```

To replace drawing entirely, either read `NotificationCenter::Active()` and
draw with your own renderer (each `ActivePopup` carries its resolved `Style`,
phase, `alpha`, `slide`, `expand` and stack position; `FormatPopup()` gives the
text lines), or keep the ImGui renderer and install a callback:

```cpp
renderer.SetCustomDraw([&](ImDrawList* dl, const ActivePopup& p, const PopupLayout& l) {
  if (p.n.kind != NotificationKind::kUnlock) return false;  // Built-in look for steps.
  dl->AddRectFilled(l.min, l.max, IM_COL32(0, 0, 0, int(200 * l.alpha)), 4.0f);
  dl->AddText(l.font_bold, 20 * l.scale, ImVec2(l.min.x + 12, l.min.y + 12),
              IM_COL32(255, 255, 255, int(255 * l.alpha)), p.n.name.c_str());
  return true;
});
```
