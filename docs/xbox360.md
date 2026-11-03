# Xbox 360 achievements (XDBF / SPA)

Two libraries:

| Target | Needs | What |
| --- | --- | --- |
| `recomp::achievements_xbox` | nothing | XDBF reader, title-update merge, `AchievementSet` builder, icon bytes |
| `recomp::achievements_rexglue` | a rexglue runtime target, patched | hooks rexglue's kernel to `System` |

## The reader (`recomp/achievements/sources/xdbf.h`)

An Xbox 360 title database (XDBF, also called SPA) lives in two places:

- inside the XEX, as the resource section named after the title id
  (`454108E6`), and
- as `spa.bin` in title updates and marketplace content, which carry the full,
  newer achievement list (DLC achievements, fixed strings).

```cpp
using namespace recomp::achievements;

xbox::XdbfDatabase base, tu;
base.Load(xex_resource_bytes);  // or LoadFile("title.spa")
tu.LoadFile(xbox::FindTitleUpdateSpa({content_root}, 0x454108E6));

xbox::XdbfTitle title;
title.SetBase(std::move(base));
title.SetTitleUpdate(std::move(tu));  // optional

xbox::XdbfSetOptions opts;
opts.set_id = "my_game";
opts.language = xbox::Language::kGerman;
system.AddSet(title.ToAchievementSet(opts));
```

What it reads:

- **XACH**: achievement id, string ids, image id, gamerscore, flags.
- **XSTR**: one string table per language (section 3, id = XLanguage).
  `GetString(id, preferred)` tries the preferred language, then English, then
  the XSTC default language, then any table.
- **XSTC**: the default language.
- **XTHD**: title id and version (`1.0.607.0`).
- **Images** (section 2) by id; `0x8000` is the title icon.

`XdbfTitle` merges the TU over the base: achievements are the union (a TU entry
replaces the base entry with the same id; TU-only ids are appended); strings are
looked up per language in the TU and then the base, so a TU that only has
English doesn't hide the base's translation; images come from the TU first.

The built set: `platform = kXbox360`, `platform_id` = XDBF id, `local_id` = the
id as text (keys are `"<set id>:<id>"`), `points` = gamerscore, `icon` =
`"xdbf:<image id>"`, `locked_description` = the "unachieved" string when it
differs from the description, and `secret` when flag bit 3 (`0x8`, "show
details while locked") is clear.

`FindTitleUpdateSpa(roots, title_id)` looks in
`<root>/0000000000000000/<TITLEID>/00000002/**/spa.bin` (marketplace content),
`.../000B0000/**/spa.bin` (title updates) and `<root>/spa.bin`. Files whose XTHD
names another title are skipped. The best is the highest XTHD version, then the
most achievements, then the largest file. (My Game's three content packs carry
1.0.556, 1.0.600 and 1.0.607 databases with 52, 59 and 68 achievements; 607 is
picked.)

### Icons

`XdbfTitle::ResolveIcon("xdbf:<id>")` returns the PNG bytes (empty when
missing). Register it with the UI's icon cache:

```cpp
icon_cache.AddProvider("xdbf", [&title](const ui::IconRequest& req, ui::IconData& out) {
  auto png = title.ResolveIcon(req.ref);
  if (png.empty()) return ui::IconResult::kMissing;
  out.bytes.assign(png.begin(), png.end());
  return ui::IconResult::kOk;
});
```

With several Xbox sets loaded, dispatch on `req.set_id`.

## rexglue binding (`recomp/achievements/rexglue.h`)

Upstream rexglue has no achievement hooks. `patches/rexglue-achievements.patch`
(against rexglue `7eb0faf`) adds them:

- `KernelState::SetAchievementUnlockCallback`, `SetIsAchievementUnlockedCallback`,
  `SetShowAchievementsUICallback` and the `Notify*` / `IsAchievementUnlocked`
  helpers. `NotifyAchievementUnlocked` also broadcasts `0x2A`, the XN_SYS
  achievement notification titles listen for.
- `XGIUserWriteAchievements` (XGI message `0x000B0008`) calls
  `NotifyAchievementUnlocked` for every `{user, id}` pair the game writes.
- `XamUserCreateAchievementEnumerator` marks achievements the callback reports
  as unlocked (flags `0x00020001`, non-zero unlock time), so in-game
  achievement lists show progress.
- `XamShowAchievementsUI`, `XamShowAchievementsUIEx` and
  `XamShowAchievementDetailsUI` stop being stubs and call the show-UI callback.

Apply it to your rexglue checkout (`git -C third_party/rexglue-sdk apply
.../patches/rexglue-achievements.patch`). My Game's own
`rexglue-my_game-plus.patch` already contains these hunks; don't apply both.

Build with `-DRECOMP_ACH_REXGLUE_TARGET=rex::runtime`, link
`recomp::achievements_rexglue`, and bind once the executable module is loaded:

```cpp
recomp::achievements::RexglueOptions opts;
opts.content_roots = {content_root};  // where 0000000000000000/ lives
opts.on_show_achievements = [&](uint32_t, uint32_t) { achievement_list.Open(); };
auto binding = recomp::achievements::BindRexglue(kernel_state, system, "my_game", opts);
```

- The set is built from `kernel_state`'s executable module XDBF plus the best
  `spa.bin` under `content_roots` (default: the working directory and
  `./game`), in the `user_language` cvar's language unless `opts.language` is
  set. `add_set = false` skips building it, for games that ship their own set
  (e.g. JSON with custom keys and triggers); the hooks then match by
  `platform_id`.
- Every user index unlocks the one local profile. Ids the set doesn't have go
  to `on_unknown_achievement`.
- `binding->ResolveIcon(ref)` serves `xdbf:` icons; `Reload()` re-reads after
  a TU install; the destructor (or `Unbind()`) clears the kernel callbacks, so
  destroy the binding before the `System`.

Known gap: the patched enumerator reads names from the XEX database only, not
the TU's `spa.bin`, so achievements only the TU has don't appear in the game's
own list (they still unlock and show in the SDK's UI).

## Stock rexglue: shimming XAM yourself (`recomp/achievements/sources/xam.h`)

Some recomps keep rexglue stock and change its behaviour only from host shims, for example by overriding an
`__imp__` export and reaching the original through `dlsym(RTLD_NEXT)`. Those recomps can't apply
`patches/rexglue-achievements.patch`, so build with `-DRECOMP_ACH_REXGLUE_PATCHED=OFF`. Then `BindRexglue`
still loads the title's XDBF and the newest spa.bin, but hooks nothing. Your shims pass the game's calls to
`binding->xam()`, an `xbox::XamAchievements` that works on raw guest data:

| Game call | Shim does |
|---|---|
| `XGIUserWriteAchievements` (XGI message `0x000B0008`, `xbox::kXgiUserWriteAchievements`) | `xam().WriteAchievementsMessage(buffer, translate)`, then calls the stock handler |
| `XamEnumerate` on an achievement enumerator | calls the stock export, then `xam().ApplyUnlockState(buffer, items_written)` |
| `XamShowAchievementsUI`, `...UIEx`, `XamShowAchievementDetailsUI` | `xam().ShowAchievementsUI(user_index, title_id)` (runs `on_show_achievements`) |

`translate` turns a guest address into a host pointer (for example `memory->TranslateVirtual`). To catch
only achievement enumerators in `XamEnumerate`, remember the handles your
`XamUserCreateAchievementEnumerator` shim returns.

```cpp
recomp::achievements::RexglueOptions options;
options.on_show_achievements = [](uint32_t, uint32_t) { OpenAchievementList(); };
auto binding = recomp::achievements::BindRexglue(kernel_state, system, "my_game", options);

// In the XGI message shim, before calling the original:
if (message == recomp::achievements::xbox::kXgiUserWriteAchievements) {
  binding->xam().WriteAchievementsMessage(host_buffer, [&](uint32_t address) {
    return memory->TranslateVirtual<const uint8_t*>(address);
  });
}
```

`XamAchievements` doesn't need rexglue at all, so other Xbox 360 frameworks can use it the same way with
their own `System` and set id.
