#include <atomic>
#include <cstdio>
#include <exception>
#include <fstream>

#include "test.h"

namespace test {
namespace {
std::atomic<size_t> g_checks{0};
}

std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

void CountCheck() { ++g_checks; }

void Fail(const char* file, int line, const std::string& message) {
  throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + message};
}

std::filesystem::path DataDir() { return RECOMP_ACH_TEST_DATA; }

std::filesystem::path TempDir(std::string_view name) {
  const auto dir = std::filesystem::path(RECOMP_ACH_TEST_TMP) / std::string(name);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

void WriteFile(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

nlohmann::json ReadJson(const std::filesystem::path& path) {
  return nlohmann::json::parse(ReadFile(path), nullptr, false);
}

}  // namespace test

int main(int argc, char** argv) {
  const std::string_view filter = argc > 1 ? argv[1] : "";
  size_t passed = 0, failed = 0;
  for (const auto& c : test::Registry()) {
    if (!filter.empty() && std::string_view(c.name).find(filter) == std::string_view::npos) continue;
    try {
      c.fn();
      ++passed;
      std::printf("[ PASS ] %s\n", c.name);
    } catch (const test::Failure& f) {
      ++failed;
      std::printf("[ FAIL ] %s\n         %s\n", c.name, f.message.c_str());
    } catch (const std::exception& e) {
      ++failed;
      std::printf("[ FAIL ] %s\n         unexpected exception: %s\n", c.name, e.what());
    }
  }
  std::printf("\n%zu passed, %zu failed, %zu checks\n", passed, failed, test::g_checks.load());
  return failed == 0 ? 0 : 1;
}
