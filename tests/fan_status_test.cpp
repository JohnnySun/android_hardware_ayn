// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_status.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}

using ayn::fan::FanStatusRead;
using ayn::fan::FanStatusIdentity;
using ayn::fan::FanStatusPaths;
using ayn::fan::FanStatusReader;
using ayn::fan::FanStatusResult;

using ReadSignature = FanStatusRead (*)(const FanStatusIdentity&,
                                        const FanStatusPaths&, FanStatusReader,
                                        void*);
static_assert(
    std::is_same_v<decltype(&ayn::fan::ReadFanStatus), ReadSignature>);

const FanStatusIdentity kExpectedIdentity = {"odin2_mini", "Q9", ""};
const FanStatusIdentity kExpectedLineageIdentity = {
    "odin2_mini", "", "Odin2 Mini"};

const FanStatusPaths kExpectedPaths = {
    "/sys/class/gpio5_pwm2/state",
    "/sys/class/gpio5_pwm2/duty",
};

struct Harness {
  size_t fail_at = 0;
  std::map<std::string, std::string> files = {
      {kExpectedPaths.state, "1\n"},
      {kExpectedPaths.duty, "25000\n"},
  };
  std::vector<std::string> reads;
};

bool ReadFile(void* context, const std::string& path, std::string* value) {
  auto* harness = static_cast<Harness*>(context);
  harness->reads.push_back(path);
  if (harness->fail_at == harness->reads.size()) {
    return false;
  }
  const auto found = harness->files.find(path);
  if (found == harness->files.end()) {
    return false;
  }
  *value = found->second;
  return true;
}

FanStatusRead Read(Harness* harness,
                   const FanStatusIdentity& identity = kExpectedIdentity,
                   const FanStatusPaths& paths = kExpectedPaths) {
  return ayn::fan::ReadFanStatus(identity, paths, ReadFile, harness);
}

void ValidSnapshotPreservesRawValuesAndReadOrder() {
  Harness harness;
  harness.files[kExpectedPaths.duty] = "0";

  const FanStatusRead status = Read(&harness);

  CHECK(status.result == FanStatusResult::kAvailable);
  CHECK(status.snapshot.has_value());
  CHECK(status.snapshot->state == 1);
  CHECK(status.snapshot->duty == 0);
  CHECK(harness.reads == std::vector<std::string>(
                             {kExpectedPaths.state, kExpectedPaths.duty}));

  Harness disabled;
  disabled.files[kExpectedPaths.state] = "0";
  const FanStatusRead disabled_status = Read(&disabled);
  CHECK(disabled_status.result == FanStatusResult::kAvailable);
  CHECK(disabled_status.snapshot.has_value());
  CHECK(disabled_status.snapshot->state == 0);

  Harness lineage;
  const FanStatusRead lineage_status = Read(&lineage, kExpectedLineageIdentity);
  CHECK(lineage_status.result == FanStatusResult::kAvailable);
  CHECK(lineage_status.snapshot.has_value());
}

void IdentityAndEveryPathMustMatchBeforeReading() {
  for (const FanStatusIdentity& identity : {
           FanStatusIdentity{"kalama", "Q9", ""},
           FanStatusIdentity{"odin2_mini", "", ""},
           FanStatusIdentity{"odin2_mini", "", "Odin2"},
           FanStatusIdentity{"odin2_mini", "q9", "Odin2 Mini"},
           FanStatusIdentity{"odin2_mini", "Q9 ", "Odin2 Mini"},
       }) {
    Harness unsupported;
    const FanStatusRead unsupported_status = Read(&unsupported, identity);
    CHECK(unsupported_status.result == FanStatusResult::kUnsupportedDevice);
    CHECK(!unsupported_status.snapshot.has_value());
    CHECK(unsupported.reads.empty());
  }

  for (size_t changed_path = 0; changed_path < 2; ++changed_path) {
    Harness harness;
    FanStatusPaths paths = kExpectedPaths;
    std::string* fields[] = {&paths.state, &paths.duty};
    *fields[changed_path] += ".unexpected";

    const FanStatusRead status = Read(&harness, kExpectedIdentity, paths);

    CHECK(status.result == FanStatusResult::kUnexpectedPaths);
    CHECK(!status.snapshot.has_value());
    CHECK(harness.reads.empty());
  }
}

void MissingReadsStopWithoutReturningPartialStatus() {
  for (size_t fail_at = 1; fail_at <= 2; ++fail_at) {
    Harness harness;
    harness.fail_at = fail_at;

    const FanStatusRead status = Read(&harness);

    CHECK(status.result == FanStatusResult::kUnavailableRead);
    CHECK(!status.snapshot.has_value());
    CHECK(harness.reads.size() == fail_at);
  }

  const FanStatusRead missing_reader =
      ayn::fan::ReadFanStatus(kExpectedIdentity, kExpectedPaths, nullptr,
                              nullptr);
  CHECK(missing_reader.result == FanStatusResult::kUnavailableRead);
  CHECK(!missing_reader.snapshot.has_value());
}

void MalformedValuesStopWithoutReturningPartialStatus() {
  struct Case {
    size_t path_index;
    std::string value;
  };
  const std::vector<Case> cases = {
      {0, "2"},
      {0, ""},
      {1, "-1"},
      {1, "+1"},
      {1, " 1"},
      {1, "1 "},
      {1, "1\t"},
      {1, "1\n\n"},
      {1, "1x"},
      {1, "2147483648"},
      {1, "000000000000000000000000000000000"},
  };

  for (const Case& test_case : cases) {
    Harness harness;
    const std::string paths[] = {kExpectedPaths.state, kExpectedPaths.duty};
    harness.files[paths[test_case.path_index]] = test_case.value;

    const FanStatusRead status = Read(&harness);

    CHECK(status.result == FanStatusResult::kMalformedValue);
    CHECK(!status.snapshot.has_value());
    CHECK(harness.reads.size() == test_case.path_index + 1);
  }
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"valid snapshot preserves raw values and read order",
       ValidSnapshotPreservesRawValuesAndReadOrder},
      {"identity and every path must match before reading",
       IdentityAndEveryPathMustMatchBeforeReading},
      {"missing reads stop without returning partial status",
       MissingReadsStopWithoutReturningPartialStatus},
      {"malformed values stop without returning partial status",
       MalformedValuesStopWithoutReturningPartialStatus},
  };

  size_t passed = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      ++passed;
      std::cout << "[PASS] " << test.first << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n';
      return 1;
    }
  }
  std::cout << passed << " tests passed\n";
  return 0;
}
