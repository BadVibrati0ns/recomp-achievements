#include <cstring>

#include "test.h"

namespace ui_test {
std::vector<Case>& Registry() {
  static std::vector<Case> r;
  return r;
}
int g_failures = 0;
}  // namespace ui_test

// Optional argument: run only tests whose name contains it.
int main(int argc, char** argv) {
  int run = 0, failed = 0;
  for (const auto& c : ui_test::Registry()) {
    if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
    const int before = ui_test::g_failures;
    c.fn();
    ++run;
    const bool ok = ui_test::g_failures == before;
    if (!ok) ++failed;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
  }
  std::printf("%d tests, %d failed\n", run, failed);
  return failed ? 1 : 0;
}
