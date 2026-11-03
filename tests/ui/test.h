#pragma once

// A minimal test harness so the UI tests need nothing beyond the SDK.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ui_test {

struct Case {
  const char* name;
  std::function<void()> fn;
};
std::vector<Case>& Registry();
extern int g_failures;

struct Register {
  Register(const char* name, std::function<void()> fn) { Registry().push_back({name, std::move(fn)}); }
};

}  // namespace ui_test

#define UI_CAT2(a, b) a##b
#define UI_CAT(a, b) UI_CAT2(a, b)
#define TEST(name)                                                      \
  static void UI_CAT(test_, name)();                                    \
  static ::ui_test::Register UI_CAT(reg_, name)(#name, UI_CAT(test_, name)); \
  static void UI_CAT(test_, name)()

#define CHECK(cond)                                                                 \
  do {                                                                              \
    if (!(cond)) {                                                                  \
      std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);        \
      ++::ui_test::g_failures;                                                      \
    }                                                                               \
  } while (0)

#define CHECK_EQ(a, b)                                                              \
  do {                                                                              \
    if (!((a) == (b))) {                                                            \
      std::printf("  %s:%d: CHECK_EQ(%s, %s) failed\n", __FILE__, __LINE__, #a, #b); \
      ++::ui_test::g_failures;                                                      \
    }                                                                               \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                       \
  do {                                                                              \
    const double ui_a_ = (a), ui_b_ = (b);                                          \
    if (!(std::fabs(ui_a_ - ui_b_) <= (eps))) {                                     \
      std::printf("  %s:%d: CHECK_NEAR(%s, %s): %g vs %g\n", __FILE__, __LINE__, #a, #b, ui_a_, ui_b_); \
      ++::ui_test::g_failures;                                                      \
    }                                                                               \
  } while (0)
