// GuestMemory and ConditionEvaluator against fake memory buffers.

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "recomp/achievements/guest_memory.h"
#include "recomp/achievements/memory_triggers.h"
#include "test_util.h"

using namespace recomp::achievements;

namespace {

using Kind = ConditionEvaluator::EventKind;

struct Recorded {
  Kind kind;
  std::string key;
  uint32_t value, target;
  bool as_percent;
};

// Evaluates one frame and returns the events it produced.
std::vector<Recorded> Frame(ConditionEvaluator& eval, const GuestMemory& mem) {
  std::vector<Recorded> out;
  eval.Update(mem, [&](const ConditionEvaluator::Event& e) {
    out.push_back({e.kind, e.key, e.value, e.target, e.as_percent});
  });
  return out;
}

bool Has(const std::vector<Recorded>& events, Kind kind) {
  for (const Recorded& e : events)
    if (e.kind == kind) return true;
  return false;
}

struct Ram {
  std::array<uint8_t, 256> bytes{};
  GuestMemory memory() const {
    GuestMemory m;
    m.AddRegion(0, bytes.size(), bytes.data());
    return m;
  }
};

// Stores a big-endian guest word the way N64Recomp does: as a native uint32_t.
void N64Store32(std::vector<uint32_t>& rdram, uint32_t address, uint32_t value) {
  rdram[address / 4] = value;
}

}  // namespace

MEM_TEST(guest_memory_regions) {
  std::array<uint8_t, 4> low{1, 2, 3, 4};
  std::array<uint8_t, 4> high{5, 6, 7, 8};
  GuestMemory mem;
  mem.AddRegion(0x100, 4, low.data()).AddRegion(0x104, 4, high.data());
  CHECK(mem.IsMapped(0x100));
  CHECK(mem.IsMapped(0x107));
  CHECK(!mem.IsMapped(0x108));
  CHECK(!mem.IsMapped(0xFF));
  CHECK(mem.ReadU8(0x105) == 6);
  CHECK(mem.ReadU8(0x200) == 0);  // Unmapped reads are 0.
  // RA peek: lowest byte first; a read may straddle two regions.
  CHECK(mem.Peek(0x100, 4) == 0x04030201u);
  CHECK(mem.Peek(0x102, 4) == 0x06050403u);
  CHECK(mem.Peek(0x106, 4) == 0x00000807u);  // Partly unmapped.
  // Null or empty regions are ignored.
  mem.AddRegion(0x300, 16, nullptr);
  CHECK(!mem.IsMapped(0x300));
}

MEM_TEST(guest_memory_n64_swizzle) {
  std::vector<uint32_t> rdram(64);
  N64Store32(rdram, 0x10, 0x11223344u);
  GuestMemory mem =
      GuestMemory::N64Recomp(reinterpret_cast<const uint8_t*>(rdram.data()), 64 * 4);
  // Guest (big-endian) byte order: 11 22 33 44.
  CHECK(mem.ReadU8(0x10) == 0x11);
  CHECK(mem.ReadU8(0x11) == 0x22);
  CHECK(mem.ReadU8(0x13) == 0x44);
  CHECK(mem.ReadU32(0x10) == 0x11223344u);
  CHECK(mem.ReadU16(0x12) == 0x3344);
  CHECK(mem.Peek(0x10, 4) == 0x44332211u);
  // A 16-bit read across a word boundary.
  N64Store32(rdram, 0x14, 0x55667788u);
  CHECK(mem.ReadU16(0x13) == 0x4455);
  CHECK(mem.regions().front().endian == Endian::kBig);
}

MEM_TEST(guest_memory_ps2) {
  std::vector<uint8_t> ee(32u * 1024 * 1024);
  const uint32_t value = 0xCAFEBABEu;
  std::memcpy(&ee[0x1FFFFFC], &value, 4);
  GuestMemory mem = GuestMemory::PS2Recomp(ee.data());
  CHECK(mem.IsMapped(0x1FFFFFF));
  CHECK(!mem.IsMapped(0x2000000));
  CHECK(mem.ReadU32(0x1FFFFFC) == 0xCAFEBABEu);
  CHECK(mem.Peek(0x1FFFFFC, 4) == 0xCAFEBABEu);
  CHECK(mem.ReadU8(0x1FFFFFC) == 0xBE);

  std::vector<uint8_t> scratchpad(0x4000);
  scratchpad[0x10] = 9;
  GuestMemory with_spr = GuestMemory::PS2Recomp(ee.data(), 32u * 1024 * 1024, scratchpad.data());
  CHECK(with_spr.ReadU8(0x02000010) == 9);
  CHECK(!with_spr.IsMapped(0x02004000));
}

MEM_TEST(guest_memory_psp) {
  // psprecomp's psp_mem.ram starts at guest 0x08000000; user RAM is 0x08800000.
  std::vector<uint8_t> ram(32u * 1024 * 1024);
  const uint32_t value = 0x12345678u;
  std::memcpy(&ram[0x00800010], &value, 4);
  GuestMemory mem = GuestMemory::Psp(ram.data());
  CHECK(mem.ReadU32(0x00800010) == 0x12345678u);
  CHECK(mem.ReadU8(0x00800010) == 0x78);
  CHECK(mem.IsMapped(0x01FFFFFF));
  CHECK(!mem.IsMapped(0x02000000));
  CHECK(mem.regions().front().endian == Endian::kLittle);
}

MEM_TEST(guest_memory_x360) {
  std::vector<uint8_t> membase(0x2000);
  membase[0x1000] = 0x00;
  membase[0x1001] = 0x00;
  membase[0x1002] = 0x01;
  membase[0x1003] = 0x2C;  // 300, big-endian.
  GuestMemory mem = GuestMemory::Xbox360(membase.data(), 0x1000, 0x1000);
  CHECK(!mem.IsMapped(0xFFF));
  CHECK(mem.IsMapped(0x1000));
  CHECK(mem.ReadU32(0x1000) == 300);
  CHECK(mem.ReadU16(0x1002) == 300);
  CHECK(mem.Peek(0x1000, 4) == 0x2C010000u);
}

MEM_TEST(guest_memory_peek_callback) {
  GuestMemory mem([](uint32_t address, uint32_t num_bytes) -> uint32_t {
    return address == 0x40 && num_bytes == 1 ? 7u : 0u;
  });
  CHECK(!mem.empty());
  CHECK(mem.ReadU8(0x40) == 7);
  CHECK(mem.Peek(0x41, 1) == 0);

  ConditionEvaluator eval;
  CHECK(eval.Add("cb", "0xH0040=7", nullptr, false));
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));
}

MEM_TEST(parse_error) {
  ConditionEvaluator eval;
  ConditionError error;
  CHECK(!eval.Add("game:bad", "0xH1234=", &error));
  CHECK(error.key == "game:bad");
  CHECK(error.condition == "0xH1234=");
  CHECK(error.code < 0);
  CHECK(!error.message.empty());
  CHECK(eval.size() == 0);

  CHECK(!eval.Add("game:empty", "", &error));
  CHECK(error.key == "game:empty");

  CHECK(!eval.Add("game:op", "0xH1234!!5", &error));
  CHECK(error.code < 0);

  std::string message;
  CHECK(ConditionEvaluator::Validate("0xH1234=5_0xX0010>d0xX0010") == 0);
  CHECK(ConditionEvaluator::Validate("R:", &message) < 0);
  CHECK(!message.empty());
}

MEM_TEST(simple_compare) {
  Ram ram;
  const GuestMemory mem = ram.memory();
  ConditionEvaluator eval;
  CHECK(eval.Add("a", "0xH0010=5"));
  CHECK(eval.state("a") == TriggerState::kWaiting);
  CHECK(Frame(eval, mem).empty());
  CHECK(eval.state("a") == TriggerState::kActive);
  ram.bytes[0x10] = 4;
  CHECK(Frame(eval, mem).empty());
  ram.bytes[0x10] = 5;
  auto events = Frame(eval, mem);
  CHECK(events.size() == 1 && events[0].kind == Kind::kTriggered && events[0].key == "a");
  CHECK(eval.state("a") == TriggerState::kTriggered);
  // Triggered conditions stay quiet.
  CHECK(Frame(eval, mem).empty());
  // 16- and 32-bit reads are little-endian on the RA side.
  ram.bytes[0x20] = 0x34;
  ram.bytes[0x21] = 0x12;
  CHECK(eval.Add("b", "0x 0020=h1234_0xX0020=4660", nullptr, false));
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));
}

MEM_TEST(waiting_state) {
  Ram ram;
  const GuestMemory mem = ram.memory();
  ram.bytes[0x10] = 1;
  ConditionEvaluator eval;
  CHECK(eval.Add("w", "0xH0010=1"));
  // Already true when added: must be false for a frame first.
  CHECK(Frame(eval, mem).empty());
  CHECK(Frame(eval, mem).empty());
  CHECK(eval.state("w") == TriggerState::kWaiting);
  ram.bytes[0x10] = 0;
  CHECK(Frame(eval, mem).empty());
  ram.bytes[0x10] = 1;
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));

  // Without require_false_first it fires at once.
  CHECK(eval.Add("now", "0xH0010=1", nullptr, false));
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));
}

MEM_TEST(hit_counts) {
  Ram ram;
  const GuestMemory mem = ram.memory();
  ConditionEvaluator eval;
  CHECK(eval.Add("h", "0xH0010=1.3."));
  Frame(eval, mem);  // Leaves the waiting state.
  ram.bytes[0x10] = 1;
  CHECK(Frame(eval, mem).empty());  // 1 hit
  ram.bytes[0x10] = 0;
  CHECK(Frame(eval, mem).empty());  // Hits are kept while false.
  ram.bytes[0x10] = 1;
  CHECK(Frame(eval, mem).empty());  // 2 hits
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));  // 3 hits
}

MEM_TEST(reset_if) {
  Ram ram;
  const GuestMemory mem = ram.memory();
  ConditionEvaluator eval;
  CHECK(eval.Add("r", "0xH0010=1.3._R:0xH0011=1"));
  Frame(eval, mem);
  ram.bytes[0x10] = 1;
  Frame(eval, mem);
  Frame(eval, mem);  // 2 hits
  ram.bytes[0x11] = 1;
  auto events = Frame(eval, mem);
  CHECK(Has(events, Kind::kReset));
  CHECK(!Has(events, Kind::kTriggered));
  ram.bytes[0x11] = 0;
  CHECK(Frame(eval, mem).empty());  // 1 hit
  CHECK(Frame(eval, mem).empty());  // 2 hits
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));
}

MEM_TEST(pause_if) {
  Ram ram;
  const GuestMemory mem = ram.memory();
  ConditionEvaluator eval;
  CHECK(eval.Add("p", "0xH0010=1.2._P:0xH0011=1"));
  Frame(eval, mem);
  ram.bytes[0x10] = 1;
  ram.bytes[0x11] = 1;
  CHECK(Frame(eval, mem).empty());
  CHECK(eval.state("p") == TriggerState::kPaused);
  CHECK(Frame(eval, mem).empty());  // No hits while paused.
  CHECK(Frame(eval, mem).empty());
  ram.bytes[0x11] = 0;
  CHECK(Frame(eval, mem).empty());  // 1 hit
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));
}

MEM_TEST(measured) {
  Ram ram;
  const GuestMemory mem = ram.memory();
  ConditionEvaluator eval;
  CHECK(eval.Add("m", "M:0xH0010>=10"));
  CHECK(eval.measured_target("m") == 10);
  CHECK(eval.measured_value("m") == 0);
  ram.bytes[0x10] = 3;
  auto events = Frame(eval, mem);
  CHECK(events.size() == 1 && events[0].kind == Kind::kMeasured);
  CHECK(events[0].value == 3 && events[0].target == 10 && !events[0].as_percent);
  CHECK(Frame(eval, mem).empty());  // Unchanged value: no event.
  ram.bytes[0x10] = 7;
  events = Frame(eval, mem);
  CHECK(events.size() == 1 && events[0].value == 7);
  CHECK(eval.measured_value("m") == 7);
  ram.bytes[0x10] = 12;
  events = Frame(eval, mem);
  CHECK(Has(events, Kind::kTriggered));
  for (const Recorded& e : events)
    if (e.kind == Kind::kMeasured) CHECK(e.value == 10);  // Clamped to the target.

  // Measured hit counts: progress from hits, cleared by ResetIf.
  CHECK(eval.Add("mh", "M:0xH0020=1.5._R:0xH0021=1"));
  Frame(eval, mem);
  ram.bytes[0x20] = 1;
  Frame(eval, mem);
  Frame(eval, mem);
  CHECK(eval.measured_value("mh") == 2);
  ram.bytes[0x21] = 1;
  Frame(eval, mem);
  CHECK(eval.measured_value("mh") == 0);
}

MEM_TEST(measured_percent) {
  Ram ram;
  const GuestMemory mem = ram.memory();
  ConditionEvaluator eval;
  CHECK(eval.Add("pct", "G:0xH0010>=200"));
  ram.bytes[0x10] = 50;
  auto events = Frame(eval, mem);
  CHECK(events.size() == 1 && events[0].as_percent && events[0].value == 50 &&
        events[0].target == 200);
}

MEM_TEST(delta_compare) {
  Ram ram;
  const GuestMemory mem = ram.memory();
  ConditionEvaluator eval;
  // Fires on the frame the value goes up, and only after going up twice.
  CHECK(eval.Add("d", "0xH0010>d0xH0010.2."));
  CHECK(Frame(eval, mem).empty());
  ram.bytes[0x10] = 4;
  CHECK(Frame(eval, mem).empty());  // 1 hit
  CHECK(Frame(eval, mem).empty());  // Unchanged.
  ram.bytes[0x10] = 3;
  CHECK(Frame(eval, mem).empty());  // Went down.
  CHECK(eval.state("d") == TriggerState::kActive);
  ram.bytes[0x10] = 9;
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));  // 2 hits
}

MEM_TEST(n64_condition) {
  std::vector<uint32_t> rdram(0x100);
  GuestMemory mem = GuestMemory::N64Recomp(reinterpret_cast<const uint8_t*>(rdram.data()),
                                           static_cast<uint32_t>(rdram.size() * 4));
  ConditionEvaluator eval;
  // RA N64 addresses are guest byte addresses; 0xI/0xG are 16/32-bit big-endian.
  CHECK(eval.Add("n64", "0xH0040=h11_0xI0042=h3344_0xG0040=h11223344"));
  CHECK(Frame(eval, mem).empty());
  N64Store32(rdram, 0x40, 0x11223300u);
  CHECK(Frame(eval, mem).empty());
  N64Store32(rdram, 0x40, 0x11223344u);
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));

  // Without the ^3 transform the same condition must not match.
  GuestMemory raw;
  raw.AddRegion(0, rdram.size() * 4, reinterpret_cast<const uint8_t*>(rdram.data()));
  ConditionEvaluator eval_raw;
  CHECK(eval_raw.Add("n64", "0xH0040=h11", nullptr, false));
  CHECK(Frame(eval_raw, raw).empty());
}

MEM_TEST(x360_condition) {
  std::vector<uint8_t> membase(0x10000);
  GuestMemory mem = GuestMemory::Xbox360(membase.data(), 0x8000, 0x8000);
  ConditionEvaluator eval;
  CHECK(eval.Add("x", "0xG8010>=1000"));
  Frame(eval, mem);
  membase[0x8012] = 0x03;
  membase[0x8013] = 0xE8;  // 1000, big-endian.
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));
}

MEM_TEST(reset_rearm) {
  Ram ram;
  const GuestMemory mem = ram.memory();
  ConditionEvaluator eval;
  CHECK(eval.Add("again", "0xH0010=1"));
  Frame(eval, mem);
  ram.bytes[0x10] = 1;
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));
  eval.Reset("again");
  CHECK(eval.state("again") == TriggerState::kWaiting);
  CHECK(Frame(eval, mem).empty());  // Still true: waits for false.
  ram.bytes[0x10] = 0;
  Frame(eval, mem);
  ram.bytes[0x10] = 1;
  CHECK(Has(Frame(eval, mem), Kind::kTriggered));

  // Replacing a key re-parses; removing it stops evaluation.
  CHECK(eval.Add("again", "0xH0011=1"));
  CHECK(eval.condition("again") == "0xH0011=1");
  CHECK(eval.size() == 1);
  eval.Remove("again");
  CHECK(!eval.Contains("again"));
  CHECK(eval.state("again") == TriggerState::kInactive);
}

MEM_TEST(primed) {
  Ram ram;
  const GuestMemory mem = ram.memory();
  ConditionEvaluator eval;
  CHECK(eval.Add("c", "0xH0010=1_T:0xH0011=1"));
  Frame(eval, mem);
  ram.bytes[0x10] = 1;
  CHECK(Has(Frame(eval, mem), Kind::kPrimed));
  CHECK(eval.state("c") == TriggerState::kPrimed);
  ram.bytes[0x10] = 0;
  CHECK(Has(Frame(eval, mem), Kind::kUnprimed));
  ram.bytes[0x10] = 1;
  CHECK(Has(Frame(eval, mem), Kind::kPrimed));
  ram.bytes[0x11] = 1;
  auto events = Frame(eval, mem);
  CHECK(events.size() == 2 && events[0].kind == Kind::kUnprimed &&
        events[1].kind == Kind::kTriggered);
}
