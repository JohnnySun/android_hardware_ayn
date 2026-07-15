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

void BoundedReadAdapterReadsWithoutAWriteSurface() {
  char path[] = "/tmp/ayn-performance-adapter.XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  const std::string expected = "680000000\n";
  CHECK(write(fd, expected.data(), expected.size()) ==
        static_cast<ssize_t>(expected.size()));
  CHECK(close(fd) == 0);

  std::string observed;
  CHECK(ayn::performance::ReadPosixFile(nullptr, path, &observed));
  CHECK(observed == expected);
  CHECK(unlink(path) == 0);
}

void AdapterRejectsInvalidBuffersAndMissingFiles() {
  std::string observed;
  CHECK(!ayn::performance::ReadPosixFile(nullptr, "/does/not/exist",
                                        &observed));
  CHECK(!ayn::performance::ReadPosixFile(nullptr, "/does/not/exist",
                                        nullptr));
}

}  // namespace

int main() {
  try {
    BoundedReadAdapterReadsWithoutAWriteSurface();
    AdapterRejectsInvalidBuffersAndMissingFiles();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "performance_adapter_test: PASS\n";
  return 0;
}
