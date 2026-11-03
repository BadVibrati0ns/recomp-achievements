#include "recomp/achievements/guest_memory.h"

namespace recomp::achievements {

GuestMemory& GuestMemory::AddRegion(MemoryRegion region) {
  if (region.host && region.size > 0) regions_.push_back(std::move(region));
  return *this;
}

GuestMemory& GuestMemory::AddRegion(uint32_t guest_start, uint64_t size, const uint8_t* host,
                                    Endian endian, ByteSwizzle swizzle) {
  MemoryRegion region;
  region.guest_start = guest_start;
  region.size = size;
  region.host = host;
  region.endian = endian;
  region.swizzle = swizzle;
  return AddRegion(std::move(region));
}

GuestMemory GuestMemory::N64Recomp(const uint8_t* rdram, uint32_t size) {
  GuestMemory memory;
  memory.AddRegion(0, size, rdram, Endian::kBig, ByteSwizzle::kWord32);
  return memory;
}

GuestMemory GuestMemory::PS2Recomp(const uint8_t* ee_ram, uint32_t size,
                                   const uint8_t* scratchpad) {
  GuestMemory memory;
  memory.AddRegion(0, size, ee_ram, Endian::kLittle);
  memory.AddRegion(0x02000000, 0x4000, scratchpad, Endian::kLittle);
  return memory;
}

GuestMemory GuestMemory::Psp(const uint8_t* ram, uint32_t size) {
  GuestMemory memory;
  memory.AddRegion(0, size, ram, Endian::kLittle);
  return memory;
}

GuestMemory GuestMemory::Xbox360(const uint8_t* membase, uint32_t start, uint64_t size) {
  GuestMemory memory;
  memory.AddRegion(start, size, membase ? membase + start : nullptr, Endian::kBig);
  return memory;
}

const MemoryRegion* GuestMemory::FindRegion(uint32_t address) const {
  for (const MemoryRegion& region : regions_) {
    if (address >= region.guest_start &&
        static_cast<uint64_t>(address - region.guest_start) < region.size) {
      return &region;
    }
  }
  return nullptr;
}

uint8_t GuestMemory::ReadRegionByte(const MemoryRegion& region, uint32_t address) {
  uint64_t offset = address - region.guest_start;
  if (region.transform) {
    offset = region.transform(offset);
    if (offset >= region.size) return 0;
    return region.host[offset];
  }
  switch (region.swizzle) {
    case ByteSwizzle::kWord32: offset ^= 3; break;
    case ByteSwizzle::kWord16: offset ^= 1; break;
    case ByteSwizzle::kNone: break;
  }
  // A swizzle can move the last partial word past the end of an odd-sized region.
  if (offset >= region.size) return 0;
  return region.host[offset];
}

bool GuestMemory::IsMapped(uint32_t address) const {
  if (peek_) return true;
  return FindRegion(address) != nullptr;
}

uint8_t GuestMemory::ReadU8(uint32_t address) const {
  if (peek_) return static_cast<uint8_t>(peek_(address, 1));
  const MemoryRegion* region = FindRegion(address);
  return region ? ReadRegionByte(*region, address) : 0;
}

uint32_t GuestMemory::Peek(uint32_t address, uint32_t num_bytes) const {
  if (peek_) return peek_(address, num_bytes);
  if (num_bytes == 0 || num_bytes > 4) return 0;
  // Byte by byte, so values may straddle regions and swizzled words.
  const MemoryRegion* region = FindRegion(address);
  uint32_t value = 0;
  for (uint32_t i = 0; i < num_bytes; ++i) {
    const uint32_t a = address + i;
    if (!region || static_cast<uint64_t>(a - region->guest_start) >= region->size ||
        a < region->guest_start) {
      region = FindRegion(a);
    }
    if (region) value |= static_cast<uint32_t>(ReadRegionByte(*region, a)) << (8 * i);
  }
  return value;
}

uint16_t GuestMemory::ReadU16(uint32_t address) const {
  const uint32_t le = Peek(address, 2);
  const MemoryRegion* region = peek_ ? nullptr : FindRegion(address);
  if (region && region->endian == Endian::kBig) {
    return static_cast<uint16_t>(((le & 0xFF) << 8) | (le >> 8));
  }
  return static_cast<uint16_t>(le);
}

uint32_t GuestMemory::ReadU32(uint32_t address) const {
  const uint32_t le = Peek(address, 4);
  const MemoryRegion* region = peek_ ? nullptr : FindRegion(address);
  if (region && region->endian == Endian::kBig) {
    return (le >> 24) | ((le >> 8) & 0xFF00) | ((le << 8) & 0xFF0000) | (le << 24);
  }
  return le;
}

}  // namespace recomp::achievements
