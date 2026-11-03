#pragma once

// Minimal test harness plus builders for synthetic XDBF and TRP files, so the
// tests need no game data.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test {

struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& Registry();
struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};
void Fail(const char* file, int line, const std::string& message);

#define TEST(name)                                     \
  static void name();                                  \
  static ::test::Registrar name##_registrar(#name, name); \
  static void name()

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) ::test::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    if (!((a) == (b))) ::test::Fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")"); \
  } while (0)

// A temporary directory removed on destruction.
struct TempDir {
  TempDir();
  ~TempDir();
  std::filesystem::path path;
};

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes);

// ---- Big-endian byte writer -----------------------------------------------
struct Bytes {
  std::vector<uint8_t> v;
  void u8(uint8_t x) { v.push_back(x); }
  void u16(uint16_t x) { u8(uint8_t(x >> 8)), u8(uint8_t(x)); }
  void u32(uint32_t x) { u16(uint16_t(x >> 16)), u16(uint16_t(x)); }
  void u64(uint64_t x) { u32(uint32_t(x >> 32)), u32(uint32_t(x)); }
  void str(std::string_view s) { v.insert(v.end(), s.begin(), s.end()); }
  void zeros(size_t n) { v.insert(v.end(), n, 0); }
  void append(const std::vector<uint8_t>& b) { v.insert(v.end(), b.begin(), b.end()); }
};

// ---- XDBF -----------------------------------------------------------------
struct XdbfBuilder {
  struct Ach {
    uint16_t id, label, desc, unach;
    uint32_t image;
    uint16_t gamerscore;
    uint32_t flags;
  };
  uint32_t default_language = 1;
  bool with_xstc = true;
  std::vector<Ach> achievements;
  // language -> (string id, text)
  std::vector<std::pair<uint32_t, std::vector<std::pair<uint16_t, std::string>>>> strings;
  std::vector<std::pair<uint64_t, std::vector<uint8_t>>> images;
  bool with_xthd = false;
  uint32_t title_id = 0;
  uint16_t version[4] = {1, 0, 0, 0};

  void AddString(uint32_t language, uint16_t id, std::string text);
  std::vector<uint8_t> Build() const;
};

// ---- TRP ------------------------------------------------------------------
struct TrpBuilder {
  uint32_t version = 2;
  std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
  void Add(std::string name, std::string_view text) {
    files.emplace_back(std::move(name), std::vector<uint8_t>(text.begin(), text.end()));
  }
  void Add(std::string name, std::vector<uint8_t> bytes) { files.emplace_back(std::move(name), std::move(bytes)); }
  std::vector<uint8_t> Build() const;
};

}  // namespace test
