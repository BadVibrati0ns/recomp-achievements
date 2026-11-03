#pragma once

#include <vector>

namespace memtest {

struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& Registry();
void Fail(const char* file, int line, const char* expr);

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

}  // namespace memtest

#define MEM_TEST(name)                                          \
  static void test_##name();                                    \
  static memtest::Registrar registrar_##name(#name, test_##name); \
  static void test_##name()

#define CHECK(expr) \
  do {              \
    if (!(expr)) memtest::Fail(__FILE__, __LINE__, #expr); \
  } while (0)
