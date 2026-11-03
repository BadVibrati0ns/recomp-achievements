// Tiny test runner: `recomp_achievements_memory_tests [case]` runs one case
// (as CTest does) or every case.
#include "test_util.h"

#include <cstdio>
#include <cstring>

namespace memtest {

std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

int failures = 0;

void Fail(const char* file, int line, const char* expr) {
  std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, expr);
  ++failures;
}

}  // namespace memtest

int main(int argc, char** argv) {
  int run = 0, failed = 0;
  for (const memtest::Case& c : memtest::Registry()) {
    if (argc > 1 && std::strcmp(argv[1], c.name) != 0) continue;
    const int before = memtest::failures;
    c.fn();
    ++run;
    const bool ok = memtest::failures == before;
    if (!ok) ++failed;
    std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
  }
  if (run == 0) {
    std::fprintf(stderr, "no test named %s\n", argc > 1 ? argv[1] : "(any)");
    return 2;
  }
  std::printf("%d/%d passed\n", run - failed, run);
  return failed ? 1 : 0;
}
