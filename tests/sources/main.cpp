#include <cstdlib>
#include <cstring>
#include <random>

#include "test_util.h"

namespace test {

namespace {
int g_failures = 0;
const char* g_current = "";
}  // namespace

std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

void Fail(const char* file, int line, const std::string& message) {
  ++g_failures;
  std::fprintf(stderr, "  FAIL %s (%s:%d): %s\n", g_current, file, line, message.c_str());
}

TempDir::TempDir() {
  std::random_device rd;
  path = std::filesystem::temp_directory_path() /
         ("recomp_ach_sources_" + std::to_string(rd()) + std::to_string(rd()));
  std::filesystem::create_directories(path);
}
TempDir::~TempDir() {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  std::filesystem::create_directories(path.parent_path());
  FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) return;
  if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
}

void XdbfBuilder::AddString(uint32_t language, uint16_t id, std::string text) {
  for (auto& [lang, list] : strings) {
    if (lang == language) {
      list.emplace_back(id, std::move(text));
      return;
    }
  }
  strings.push_back({language, {{id, std::move(text)}}});
}

std::vector<uint8_t> XdbfBuilder::Build() const {
  struct Block {
    uint16_t section;
    uint64_t id;
    std::vector<uint8_t> data;
  };
  std::vector<Block> blocks;
  if (!achievements.empty()) {
    Bytes b;
    b.str("XACH");
    b.u32(1);
    b.u32(uint32_t(2 + achievements.size() * 0x24));
    b.u16(uint16_t(achievements.size()));
    for (const Ach& a : achievements) {
      b.u16(a.id), b.u16(a.label), b.u16(a.desc), b.u16(a.unach);
      b.u32(a.image), b.u16(a.gamerscore), b.u16(0), b.u32(a.flags);
      b.zeros(16);
    }
    blocks.push_back({1, 0x58414348, b.v});
  }
  if (with_xstc) {
    Bytes b;
    b.str("XSTC"), b.u32(1), b.u32(4), b.u32(default_language);
    blocks.push_back({1, 0x58535443, b.v});
  }
  if (with_xthd) {
    Bytes b;
    b.str("XTHD"), b.u32(1), b.u32(0), b.u32(title_id), b.u32(1);
    for (uint16_t part : version) b.u16(part);
    b.zeros(16);
    blocks.push_back({1, 0x58544844, b.v});
  }
  for (const auto& [lang, list] : strings) {
    Bytes body;
    for (const auto& [id, text] : list) {
      body.u16(id), body.u16(uint16_t(text.size())), body.str(text);
    }
    Bytes b;
    b.str("XSTR"), b.u32(1), b.u32(uint32_t(body.v.size() + 2)), b.u16(uint16_t(list.size()));
    b.append(body.v);
    blocks.push_back({3, lang, b.v});
  }
  for (const auto& [id, data] : images) blocks.push_back({2, id, data});

  // Spare entry slots and one free record, like real files.
  const uint32_t entry_count = uint32_t(blocks.size() + 3);
  const uint32_t free_count = 2;
  Bytes out;
  out.str("XDBF"), out.u32(0x10000), out.u32(entry_count), out.u32(uint32_t(blocks.size()));
  out.u32(free_count), out.u32(1);
  Bytes content;
  for (const Block& blk : blocks) {
    out.u16(blk.section), out.u64(blk.id), out.u32(uint32_t(content.v.size()));
    out.u32(uint32_t(blk.data.size()));
    content.append(blk.data);
  }
  out.zeros((entry_count - blocks.size()) * 18);
  out.u32(uint32_t(content.v.size())), out.u32(0);
  out.zeros((free_count - 1) * 8);
  out.append(content.v);
  return out.v;
}

std::vector<uint8_t> TrpBuilder::Build() const {
  const size_t header = version >= 3 ? 0x60 : 0x40;
  const size_t table = header + files.size() * 0x40;
  Bytes body;
  std::vector<std::pair<uint64_t, uint64_t>> places;
  for (const auto& [name, data] : files) {
    while (body.v.size() % 16) body.u8(0);
    places.emplace_back(table + body.v.size(), data.size());
    body.append(data);
  }
  Bytes out;
  out.u32(0xDCA24D00), out.u32(version), out.u64(table + body.v.size());
  out.u32(uint32_t(files.size())), out.u32(0x40), out.u32(0);
  out.zeros(20);  // SHA-1, not checked
  out.zeros(header - out.v.size());
  for (size_t i = 0; i < files.size(); ++i) {
    std::string name = files[i].first;
    name.resize(32, '\0');
    out.str(name);
    out.u64(places[i].first), out.u64(places[i].second);
    out.u32(name.find(".PNG") != std::string::npos ? 0 : 2);
    out.zeros(12);
  }
  out.append(body.v);
  return out.v;
}

}  // namespace test

int RunRealSpaTest(const char* path);  // xdbf_tests.cpp

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "--real-spa") == 0) {
    const char* path = argc >= 3 && argv[2][0] ? argv[2] : std::getenv("RECOMP_ACH_TEST_SPA");
    return RunRealSpaTest(path);
  }
  int failed_cases = 0;
  for (const test::Case& c : test::Registry()) {
    test::g_current = c.name;
    const int before = test::g_failures;
    c.fn();
    const bool ok = test::g_failures == before;
    failed_cases += !ok;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
  }
  std::printf("%zu tests, %d failed\n", test::Registry().size(), failed_cases);
  return failed_cases == 0 ? 0 : 1;
}
