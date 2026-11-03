#pragma once

// A minimal test harness: TEST() registers a case, CHECK*() records a failure
// and ends the case. No external framework so the SDK builds anywhere.

#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace test {

struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& Registry();

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

struct Failure {
  std::string message;
};

void CountCheck();
[[noreturn]] void Fail(const char* file, int line, const std::string& message);

template <typename T>
std::string Show(const T& value) {
  if constexpr (requires(std::ostream& os) { os << value; }) {
    std::ostringstream os;
    os << value;
    return os.str();
  } else {
    return "<value>";
  }
}

// A fresh, empty directory under the build tree, named after the test.
std::filesystem::path TempDir(std::string_view name);
std::filesystem::path DataDir();
void WriteFile(const std::filesystem::path& path, std::string_view text);
std::string ReadFile(const std::filesystem::path& path);
nlohmann::json ReadJson(const std::filesystem::path& path);

}  // namespace test

#define TEST(name)                                          \
  static void name();                                       \
  static const ::test::Registrar name##_registrar(#name, name); \
  static void name()

#define CHECK(cond)                                                          \
  do {                                                                       \
    ::test::CountCheck();                                                    \
    if (!(cond)) ::test::Fail(__FILE__, __LINE__, "CHECK(" #cond ")");       \
  } while (0)

#define CHECK_EQ(a, b)                                                                    \
  do {                                                                                    \
    ::test::CountCheck();                                                                 \
    const auto check_a_ = (a);                                                            \
    const auto check_b_ = (b);                                                            \
    if (!(check_a_ == check_b_)) {                                                        \
      ::test::Fail(__FILE__, __LINE__,                                                    \
                   "CHECK_EQ(" #a ", " #b "): " + ::test::Show(check_a_) + " != " +       \
                       ::test::Show(check_b_));                                           \
    }                                                                                     \
  } while (0)

// Checks that text contains every fragment, for error messages.
#define CHECK_CONTAINS(text, fragment)                                                         \
  do {                                                                                         \
    ::test::CountCheck();                                                                      \
    const std::string check_text_ = (text);                                                    \
    if (check_text_.find(fragment) == std::string::npos) {                                     \
      ::test::Fail(__FILE__, __LINE__,                                                         \
                   "CHECK_CONTAINS(" #text ", " #fragment "): \"" + check_text_ + "\"");       \
    }                                                                                          \
  } while (0)
