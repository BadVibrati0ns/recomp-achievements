# PlayStation 3 trophies

`recomp::achievements_trophy` (no dependencies) reads a title's trophy set and
gives a PS3 recomp's `sceNpTrophy` HLE functions a backend. Unlock state lives
in `System`, so trophies share saves, popups and the UI with every other set.

## Input: a decrypted TROPHY.TRP

A trophy set ships as `PS3_GAME/TROPDIR/<NPWRxxxxx_xx>/TROPHY.TRP`. The reader
expects that file **decrypted**. Files taken straight from an encrypted source
are rejected with a message that says so:

- EDAT/NPDRM (`NPD\0` magic) or SCE containers (`SCE\0`): decrypt first, e.g.
  with RPCS3 ("Utilities > Decrypt PS3 binaries" or by installing the game and
  taking the TRP from its `TROPDIR`) or an EDAT tool;
- PS4/Vita sets whose config is `TROPCONF.ESFM` (encrypted XML).

`TrpArchive` handles TRP versions 1 and 2 (PS3, 0x40-byte header) and 3
(0x60-byte header), taking the entry size from the header. The SHA-1 and
signature fields are ignored.

Inside: `TROPCONF.SFM` (the definitions), `TROP.SFM` (default-language text),
`TROP_XX.SFM` (text per `CELL_SYSUTIL_LANG_*` language: 00 Japanese, 01
English, 02 French, 03 Spanish, 04 German, 05 Italian, 06 Dutch, 07
Portuguese, 08 Russian, 09 Korean, 10/11 Chinese, ...), `ICON0.PNG` and
`TROP###.PNG`. A built-in XML reader parses the SFM files (signature comment,
declaration, DOCTYPE, CDATA and entities included).

```cpp
using namespace recomp::achievements;
trophy::TrophyFile file;
std::string error;
if (!file.LoadFile("TROPDIR/NPWR01234_00/TROPHY.TRP", /*language=*/1, &error)) log(error);
```

Text comes from `TROP_<lang>.SFM`, else `TROP.SFM`, else `TROPCONF.SFM`, per
trophy. `ToAchievementSet()` produces `platform = kPlayStation3`,
`platform_id` = trophy id, `local_id` = the id as text (keys
`"<set id>:<id>"`), `tier` from `ttype` (B/S/G/P), PSN points (bronze 15,
silver 30, gold 90, platinum 180), `secret` from `hidden="yes"`, `icon` =
`"trophy:TROP###.PNG"`, and DLC trophies (`gid`) in a category named after
their group.

Icons: `TrophyFile::ResolveIcon("trophy:<file>")` returns the PNG bytes; add it
to the UI's icon cache as a `"trophy"` provider the same way as the Xbox one
(docs/xbox360.md).

Note: `System::Unlock(set_id, platform_id)` treats platform id 0 as "none",
and trophy 0 is usually the platinum. Use keys (`"<set id>:0"`) for trophy 0;
`TrophyContext` does.

## Backend for sceNpTrophy (`sources/trophy_context.h`)

One `TrophyContext` per registered trophy context. Plain host structs; the HLE
module converts them to guest structures (byte order, fixed string sizes:
`SCE_NP_TROPHY_NAME_MAX_SIZE` 128, `SCE_NP_TROPHY_DESCR_MAX_SIZE` 1024,
`SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE` 128, `SCE_NP_TROPHY_GAME_DESCR_MAX_SIZE`
1024).

| HLE function | Context call |
| --- | --- |
| `sceNpTrophyRegisterContext` | `Register(trp_path, {.language = ...})` |
| `sceNpTrophyUnlockTrophy` | `UnlockTrophy(id, &platinum_id)` |
| `sceNpTrophyGetTrophyUnlockState` | `GetTrophyUnlockState(&flags, &count)` |
| `sceNpTrophyGetGameInfo` | `GetGameInfo(&details, &data)` |
| `sceNpTrophyGetTrophyInfo` | `GetTrophyInfo(id, &details, &data)` |
| `sceNpTrophyGetGameIcon` / `GetTrophyIcon` | `GetGameIcon()` / `GetTrophyIcon(id)` |

Behaviour:

- `UnlockTrophy` returns `kAlreadyUnlocked` for an unlocked trophy,
  `kCannotUnlockPlatinum` for the platinum, `kInvalidTrophyId` for unknown ids.
  `platinum_id` receives the platinum this unlock completed, or -1
  (`SCE_NP_TROPHY_INVALID_TROPHY_ID`). The platinum unlocks once every trophy
  whose `pid` points at it is unlocked; `SyncPlatinum()` catches it up after
  loading an older save.
- `GetTrophyUnlockState` fills a `SceNpTrophyFlagArray` layout: 128 bits in
  four 32-bit words, bit `id & 31` of word `id >> 5`
  (`SCE_NP_TROPHY_FLAG_SET`). `count` is the number of trophies in the set.
  Sets with ids of 128 or more return `kExceedsMax` (the in-range bits are
  still filled).
- `GetTrophyInfo` blanks the name and description of a locked hidden trophy
  (pass `reveal_hidden` to override). `TrophyData::timestamp` is a
  `CellRtcTick` (microseconds since 0001-01-01), 0 while locked.
- `Grade` values are the `SCE_NP_TROPHY_GRADE_*` numbers (1 platinum, 2 gold,
  3 silver, 4 bronze).

Map `Result` to the module's error codes:

| `Result` | SCE code |
| --- | --- |
| `kOk` | `CELL_OK` |
| `kNotRegistered` | `SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED` |
| `kInvalidArgument` | `SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT` |
| `kInvalidTrophyId` | `SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID` |
| `kAlreadyUnlocked` | `SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED` |
| `kCannotUnlockPlatinum` | `SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM` |
| `kExceedsMax` | `SCE_NP_TROPHY_ERROR_EXCEEDS_MAX` |
| `kLoadFailed` | `SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST` (or `ILLEGAL_UPDATE`) |

### Example: an RPCS3/ps3recomp-style HLE module

```cpp
#include "recomp/achievements/sources/trophy_context.h"
namespace ach = recomp::achievements;

static ach::System g_achievements;                       // set up storage elsewhere
static std::map<u32, std::unique_ptr<ach::trophy::TrophyContext>> g_contexts;

error_code sceNpTrophyCreateContext(vm::ptr<u32> context, vm::cptr<SceNpCommunicationId> comm_id,
                                    vm::cptr<SceNpCommunicationSignature>, u64) {
  // "NPWR01234" + num -> "NPWR01234_00", also the set id.
  const std::string np_comm_id = fmt::format("{}_{:02d}", comm_id->data, comm_id->num);
  const u32 id = next_context_id++;
  g_contexts[id] = std::make_unique<ach::trophy::TrophyContext>(g_achievements, np_comm_id);
  *context = id;
  return CELL_OK;
}

error_code sceNpTrophyRegisterContext(u32 context, u32 handle, vm::ptr<SceNpTrophyStatusCallback> cb,
                                      vm::ptr<void> arg, u64 options) {
  auto& ctx = *g_contexts.at(context);
  const auto trp = host_path("/app_home/TROPDIR") / ctx.set_id() / "TROPHY.TRP";
  std::string error;
  ach::trophy::TrophyContext::RegisterOptions opts;
  opts.language = system_language();  // CELL_SYSUTIL_LANG_*
  if (ctx.Register(trp, opts, &error) != ach::trophy::Result::kOk) {
    log_error("trophies: {}", error);
    return SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST;
  }
  ctx.SyncPlatinum();
  // Report SCE_NP_TROPHY_STATUS_INSTALLED / PROCESSING_COMPLETE through cb, as the
  // game expects, before returning.
  return CELL_OK;
}

error_code sceNpTrophyUnlockTrophy(u32 context, u32 handle, s32 trophy_id, vm::ptr<s32> platinum_id) {
  s32 plat = -1;
  const auto r = g_contexts.at(context)->UnlockTrophy(trophy_id, &plat);
  if (platinum_id) *platinum_id = plat;
  return ToSceError(r);  // table above
}

error_code sceNpTrophyGetTrophyUnlockState(u32 context, u32 handle, vm::ptr<SceNpTrophyFlagArray> flags,
                                           vm::ptr<u32> count) {
  ach::trophy::FlagArray f;
  u32 n = 0;
  const auto r = g_contexts.at(context)->GetTrophyUnlockState(&f, &n);
  if (r != ach::trophy::Result::kOk && r != ach::trophy::Result::kExceedsMax) return ToSceError(r);
  for (size_t i = 0; i < f.flag_bits.size(); ++i) flags->flag_bits[i] = f.flag_bits[i];  // be_t store
  *count = n;
  return CELL_OK;
}
```

`GetGameInfo` / `GetTrophyInfo` follow the same shape: copy the strings with
truncation into the guest struct and the counts/grade/flags as big-endian
values.
