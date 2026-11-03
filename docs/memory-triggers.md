# Memory triggers

A `"memory"` trigger unlocks an achievement when a condition on the game's
memory becomes true. Conditions use RetroAchievements' condition syntax and
are evaluated by [rcheevos](https://github.com/RetroAchievements/rcheevos)
(MIT, pinned to v11.6.0). Only rcheevos' condition parser and evaluator are
built: no `rc_client`, no web API, no hashing. Nothing in the SDK connects to
RetroAchievements.

Build option: `RECOMP_ACH_MEMORY` (on by default; fetches rcheevos with
FetchContent). Link `recomp::achievements_memory`.

## In a set file

```json
{
  "id": "ten_stars",
  "name": "Ten Star Run",
  "description": "Collect 10 stars without taking damage",
  "points": 25,
  "trigger": {
    "type": "memory",
    "params": { "condition": "M:0xH0040>=10_R:0xH0041=1" }
  }
}
```

A condition with `Measured` (`M:`) gets a progress bar whose total is its
Measured target (10 here); with `Measured%` (`G:`) the bar runs 0-100. No
`threshold` is needed.

## In the recomp

```cpp
#include "recomp/achievements/memory_triggers.h"

namespace ach = recomp::achievements;

ach::MemoryTriggers triggers(system, ach::GuestMemory::N64Recomp(rdram));
triggers.SetErrorCallback([](const ach::ConditionError& e) {
  log("achievement {}: bad condition '{}': {}", e.key, e.condition, e.message);
});

// Once per guest frame, on the thread that owns guest memory:
triggers.Update();
```

`MemoryTriggers` registers every locked achievement whose trigger type is
`"memory"`, evaluates each once per `Update()` and calls `System::Unlock` when
one fires. Measured values go to `System::RecordValue(key, value, total)` with
the Measured target as the total.

It re-syncs whenever `System::generation()` changes:

- sets added, replaced or removed: new conditions are parsed, edited ones
  re-parsed, removed ones dropped; other conditions keep their hit counts;
- progress reset (`Reset()`, `Reset(keys)`), `ReloadProgress()`, `SetStorage()`
  or a profile switch: every condition's hit counts are cleared and it is
  re-armed, and reset achievements are evaluated again. (If sets change in the
  same frame as a reset, hit counts are kept.)

Achievements unlocked by other means (game code, another trigger) are dropped
on the next frame through an unlock listener; that may be called from any
thread, everything else belongs to the thread calling `Update()`.
`Sync()` and `ResetConditions()` do the same work on demand.

`SetPrimedCallback` reports when a condition is primed (everything but its
`Trigger` conditions is true), for an on-screen "challenge" indicator.

`ConditionEvaluator` is the same engine without a `System`: add keyed
conditions, call `Update(memory, fn)`, receive triggered / primed / reset /
measured events. Use it for game-defined trigger types or tools.

Like on RetroAchievements, a condition must be false for at least one frame
before it can fire (`kWaiting`), so loading a save where the condition is
already true doesn't unlock it. Turn this off with
`Options::require_false_first = false`.

## Condition syntax (short)

A condition is a list of requirements joined by `_`. All must be true on the
same frame. `S` starts an alternative group: the core group and at least one
alternative must be true.

Memory operands are `0x<size><address>` with the address in hex:

| Prefix | Size | Prefix | Size |
|---|---|---|---|
| `0xH` | 8-bit | `0xM`..`0xT` | bit 0..7 |
| `0x ` (space) | 16-bit LE | `0xL` / `0xU` | low / high nibble |
| `0xW` | 24-bit LE | `0xK` | number of set bits |
| `0xX` | 32-bit LE | `fF` / `fB` | float LE / BE |
| `0xI` | 16-bit BE | `0xJ` | 24-bit BE |
| `0xG` | 32-bit BE | | |

Constants are decimal (`5`) or `h`-prefixed hex (`h1F`); `0x1F` is a memory
read, not a number. Comparisons: `=`, `!=`, `<`, `<=`, `>`, `>=`; `AddSource`/`SubSource` and
`AddAddress` lines also take `*`, `/`, `&`, `^`, `%`. Operand modifiers: `d` delta (last frame's
value), `p` prior (value before the last change), `b` BCD, `~` inverted.

Hit counts: `.N.` after a requirement means "true on N frames (not
necessarily in a row)". Flags before a requirement:

| Flag | Meaning |
|---|---|
| `R:` | ResetIf: when true, clear all hit counts |
| `P:` | PauseIf: when true, the group stops counting |
| `M:` | Measured: progress value (memory value or hit count) |
| `G:` | Measured as a percentage |
| `Q:` | MeasuredIf: only measure while true |
| `T:` | Trigger: the last step, used for the primed state |
| `A:` / `B:` | AddSource / SubSource into the next requirement |
| `C:` / `D:` | AddHits / SubHits |
| `N:` / `O:` | AndNext / OrNext |
| `I:` | AddAddress (pointers) |
| `Z:` | ResetNextIf |

Examples:

```
0xH0010=5                          byte at 0x10 equals 5
0xH0010>d0xH0010                   byte at 0x10 went up this frame
0xH0050=3.300._R:0xH0051=0         0x50 is 3 on 300 frames; starts over whenever 0x51 is 0
M:0xH0040>=10_R:0xH0041=1          10 stars (progress 0..10), reset on damage
G:0xX0100>=50000                   32-bit LE score as a percentage of 50000
0xG82001234>=h3E8                  Xbox 360: 32-bit BE value at 0x82001234 >= 1000
I:0xX0200_0xH0008=1                pointer at 0x200, then the byte at ptr+8
0xH0010=1_T:0xH0011=1              primed while 0x10=1, fires when 0x11 becomes 1
```

The full reference is RetroAchievements' documentation:
<https://docs.retroachievements.org/developer-docs/achievement-development-overview.html>.

## Address maps

RetroAchievements reads values byte by byte from an address space defined per
console; multi-byte sizes are composed little-endian unless a BE size is used.
`GuestMemory` describes the recomp's memory in those terms: a list of regions
(guest address range, host pointer, guest byte order, byte swizzle or custom
transform), or a peek callback with RetroAchievements' semantics (byte at
`address` in the lowest 8 bits). Unmapped addresses read as 0.

| Helper | RA addresses | Host layout |
|---|---|---|
| `GuestMemory::N64Recomp(rdram, size = 8 MB)` | `0x000000`-`0x7FFFFF` = RDRAM (physical; KSEG0 `0x80000000` is RA `0x0`) | N64Recomp's `rdram`: big-endian words stored as native 32-bit values, so guest byte `a` is `rdram[a ^ 3]` |
| `GuestMemory::PS2Recomp(ee_ram, size = 32 MB, scratchpad = nullptr)` | `0x00000000`-`0x01FFFFFF` = EE RAM, `0x02000000`-`0x02003FFF` = scratchpad (EE `0x70000000`) | little-endian, linear |
| `GuestMemory::Xbox360(membase, start, size)` | guest virtual addresses (RA has no Xbox 360 map) | rexglue/Xenia-style: guest address `A` at `membase + A`, big-endian |

N64 and Xbox 360 values are big-endian, so their conditions use `0xI`, `0xJ`,
`0xG` and `fB` for multi-byte values. N64 sets on RetroAchievements already do.

For the Xbox 360 helper, only pass committed ranges: reserved but unmapped
pages fault when read. Use one `Xbox360(...)` for the main range and
`AddRegion` for others, e.g. the XEX image at `0x82000000` and the heap the
game data lives in.

Custom layouts:

```cpp
ach::GuestMemory mem;
mem.AddRegion(0x00000000, 0x400000, main_ram, ach::Endian::kBig, ach::ByteSwizzle::kWord32);
mem.AddRegion(0x00800000, 0x10000, sram);             // little-endian, linear
ach::GuestMemory peeked([](uint32_t address, uint32_t num_bytes) {
  return my_bus_read(address, num_bytes);               // lowest byte = address
});
```

## Conditions written for an emulator

Existing RetroAchievements conditions address the console's memory as an
emulator exposes it. They only work in a recomp that keeps the same memory
layout for the data they read:

- **N64Recomp and PS2Recomp** run the original code against an RDRAM / EE RAM
  image laid out like the console's, so game data usually sits at the same
  addresses and conditions usually carry over. Check each one: patches, mods
  or relocated buffers move data.
- **Decompilation-based ports** (and native rewrites) lay out their data
  however the compiler likes; addresses from the original game mean nothing
  there. Use game code or the built-in trigger types instead.
- **Xbox 360 / PS3 recomps** have no RetroAchievements sets to port; conditions
  are written fresh against guest virtual addresses.

RetroAchievements' public web API does not publish conditions (an imported
set's `_ra_logic_md5` is only a hash of them), so imported achievements start
as `"manual"`; the recomp writes its own conditions. See
[retroachievements.md](retroachievements.md).
