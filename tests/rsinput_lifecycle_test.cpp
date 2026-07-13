// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_lifecycle.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
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

struct Harness {
  bool stop_requested = false;
  size_t open_calls = 0;
  std::vector<std::vector<uint8_t>> writes;
  size_t request_stop_after_writes = 0;
};

bool StopRequested(void* context) {
  return static_cast<Harness*>(context)->stop_requested;
}

int OpenUart(void* context) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->open_calls;
  return 42;
}

bool WriteFrame(void* context, const uint8_t* data, size_t size) {
  auto* harness = static_cast<Harness*>(context);
  harness->writes.emplace_back(data, data + size);
  if (harness->request_stop_after_writes == harness->writes.size()) {
    harness->stop_requested = true;
  }
  return true;
}

void RunningStartupOpensUartAndWritesBothInitializationFrames() {
  Harness harness;
  int uart_fd = -1;

  CHECK(ayn::rsinput::OpenUartUnlessStopped(
            StopRequested, &harness, OpenUart, &harness, &uart_fd) ==
        ayn::rsinput::StartupResult::kCompleted);
  CHECK(harness.open_calls == 1);
  CHECK(uart_fd == 42);
  CHECK(ayn::rsinput::SendInitializationFramesUnlessStopped(
            StopRequested, &harness, WriteFrame, &harness) ==
        ayn::rsinput::StartupResult::kCompleted);
  CHECK(harness.writes.size() == 2);
}

void StopBeforeStartupPreventsUartOpenAndInitializationWrites() {
  Harness harness;
  harness.stop_requested = true;
  int uart_fd = -1;

  CHECK(ayn::rsinput::OpenUartUnlessStopped(
            StopRequested, &harness, OpenUart, &harness, &uart_fd) ==
        ayn::rsinput::StartupResult::kStopped);
  CHECK(harness.open_calls == 0);
  CHECK(uart_fd == -1);
  CHECK(ayn::rsinput::SendInitializationFramesUnlessStopped(
            StopRequested, &harness, WriteFrame, &harness) ==
        ayn::rsinput::StartupResult::kStopped);
  CHECK(harness.writes.empty());
}

void StopBetweenInitializationFramesPreventsSecondWrite() {
  Harness harness;
  harness.request_stop_after_writes = 1;

  CHECK(ayn::rsinput::SendInitializationFramesUnlessStopped(
            StopRequested, &harness, WriteFrame, &harness) ==
        ayn::rsinput::StartupResult::kStopped);
  CHECK(harness.writes.size() == 1);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"running startup opens UART and writes both initialization frames",
       RunningStartupOpensUartAndWritesBothInitializationFrames},
      {"stop before startup prevents UART open and initialization writes",
       StopBeforeStartupPreventsUartOpenAndInitializationWrites},
      {"stop between initialization frames prevents second write",
       StopBetweenInitializationFramesPreventsSecondWrite},
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
