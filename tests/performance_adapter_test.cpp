// SPDX-License-Identifier: Apache-2.0

#include "ayn/performance_adapter.h"

#include <fcntl.h>
#include <unistd.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}

void BoundedAdapterRoundTripsWithoutSysfs() {
  char path[] = "/tmp/ayn-performance-adapter.XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  CHECK(close(fd) == 0);

  CHECK(ayn::performance::WritePosixFile(nullptr, path, "680000000"));
  std::string observed;
  CHECK(ayn::performance::ReadPosixFile(nullptr, path, &observed));
  CHECK(observed == "680000000");
  CHECK(unlink(path) == 0);
}

void AdapterRejectsInvalidBuffersAndMissingFiles() {
  std::string observed;
  CHECK(!ayn::performance::ReadPosixFile(nullptr, "/does/not/exist",
                                        &observed));
  CHECK(!ayn::performance::ReadPosixFile(nullptr, "/does/not/exist",
                                        nullptr));
  CHECK(!ayn::performance::WritePosixFile(nullptr, "/does/not/exist", ""));
  CHECK(!ayn::performance::WritePosixFile(nullptr, "/does/not/exist",
                                         std::string(65, '1')));
}

}  // namespace

int main() {
  try {
    BoundedAdapterRoundTripsWithoutSysfs();
    AdapterRejectsInvalidBuffersAndMissingFiles();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "performance_adapter_test: PASS\n";
  return 0;
}
