#pragma once

// A read-only view of a recomp's guest memory, addressed the way
// RetroAchievements conditions address it. Used by MemoryTriggers; game code
// may also use it to read guest values without knowing the host layout.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace recomp::achievements {

enum class Endian { kLittle, kBig };

// How a guest byte address inside a region becomes a host byte offset.
enum class ByteSwizzle {
  kNone,  // host[offset]
  // host[offset ^ 3]: big-endian 32-bit words stored native-endian on a
  // little-endian host (N64Recomp's RDRAM).
  kWord32,
  // host[offset ^ 1]: big-endian 16-bit units stored native-endian.
  kWord16,
};

// One contiguous guest range backed by host memory.
struct MemoryRegion {
  uint32_t guest_start = 0;
  uint64_t size = 0;  // Bytes; up to 4 GB so a region can cover the whole space.
  const uint8_t* host = nullptr;
  Endian endian = Endian::kLittle;  // Guest byte order, for ReadU16/ReadU32.
  ByteSwizzle swizzle = ByteSwizzle::kNone;
  // Optional: overrides `swizzle`. Maps an offset from guest_start to a host
  // offset; must stay inside the region.
  std::function<uint64_t(uint64_t offset)> transform;
};

class GuestMemory {
 public:
  // RetroAchievements peek semantics: return num_bytes (1, 2 or 4) guest
  // bytes starting at address, the byte at `address` in the lowest 8 bits.
  // Big-endian values are read with the condition's BE sizes (0xI, 0xG...).
  using PeekFn = std::function<uint32_t(uint32_t address, uint32_t num_bytes)>;

  GuestMemory() = default;
  explicit GuestMemory(PeekFn peek) : peek_(std::move(peek)) {}

  // Regions are searched in the order added. Ignored when a PeekFn is set.
  GuestMemory& AddRegion(MemoryRegion region);
  GuestMemory& AddRegion(uint32_t guest_start, uint64_t size, const uint8_t* host,
                         Endian endian = Endian::kLittle,
                         ByteSwizzle swizzle = ByteSwizzle::kNone);
  const std::vector<MemoryRegion>& regions() const { return regions_; }
  bool empty() const { return !peek_ && regions_.empty(); }

  // ---- Ready-made layouts ------------------------------------------------
  // N64Recomp: `rdram` is the RDRAM buffer (physical address 0, i.e. KSEG0
  // 0x80000000) holding big-endian words as native 32-bit values. RA's N64
  // map puts RDRAM at 0x00000000, so RA addresses are physical addresses.
  static GuestMemory N64Recomp(const uint8_t* rdram, uint32_t size = 8u * 1024 * 1024);
  // PS2Recomp: 32 MB of little-endian EE RAM at 0x00000000 (RA's PS2 map),
  // plus the 16 KB scratchpad (EE 0x70000000) at RA's 0x02000000 if given.
  static GuestMemory PS2Recomp(const uint8_t* ee_ram, uint32_t size = 32u * 1024 * 1024,
                               const uint8_t* scratchpad = nullptr);
  // PSP recomps (e.g. psprecomp's psp_mem.ram): `ram` holds guest memory
  // from 0x08000000, little-endian. RA's PSP map puts address 0 at
  // 0x08000000, so user RAM (0x08800000) is RA 0x00800000.
  static GuestMemory Psp(const uint8_t* ram, uint32_t size = 32u * 1024 * 1024);
  // rexglue / Xbox 360: big-endian guest memory where guest virtual address A
  // lives at membase + A. RA has no 360 map; addresses are guest virtual
  // addresses. Only expose ranges that are committed: reading an unmapped
  // page of a reserved address space faults.
  static GuestMemory Xbox360(const uint8_t* membase, uint32_t start, uint64_t size);

  // ---- Reads -------------------------------------------------------------
  // Unmapped bytes read as 0, as in RetroAchievements.
  bool IsMapped(uint32_t address) const;
  uint8_t ReadU8(uint32_t address) const;
  uint32_t Peek(uint32_t address, uint32_t num_bytes) const;
  // Values in the region's guest byte order.
  uint16_t ReadU16(uint32_t address) const;
  uint32_t ReadU32(uint32_t address) const;

 private:
  const MemoryRegion* FindRegion(uint32_t address) const;
  static uint8_t ReadRegionByte(const MemoryRegion& region, uint32_t address);

  PeekFn peek_;
  std::vector<MemoryRegion> regions_;
};

}  // namespace recomp::achievements
