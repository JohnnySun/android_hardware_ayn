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
  size_t attempt = 0;
  size_t uart_close_calls = 0;
  size_t uinput_open_calls = 0;
  size_t uinput_close_calls = 0;
  size_t initialization_calls = 0;
  size_t forward_calls = 0;
  size_t read_failures = 0;
  size_t emit_failures = 0;
  std::vector<uint32_t> retry_delays_ms;
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

int OpenSessionUart(void* context) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->attempt;
  return harness->attempt == 1 ? -1 : static_cast<int>(100 + harness->attempt);
}

int OpenSessionUinput(void* context) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->uinput_open_calls;
  return harness->attempt == 2 ? -1 : static_cast<int>(200 + harness->attempt);
}

void CloseSessionUart(void* context, int) {
  ++static_cast<Harness*>(context)->uart_close_calls;
}

void CloseSessionUinput(void* context, int) {
  ++static_cast<Harness*>(context)->uinput_close_calls;
}

ayn::rsinput::StartupResult InitializeSession(void* context, int) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->initialization_calls;
  return harness->attempt == 3 ? ayn::rsinput::StartupResult::kFailed
                               : ayn::rsinput::StartupResult::kCompleted;
}

ayn::rsinput::StartupResult ForwardSession(void* context, int, int) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->forward_calls;
  if (harness->attempt == 4) {
    ++harness->read_failures;
    return ayn::rsinput::StartupResult::kFailed;
  }
  if (harness->attempt == 5) {
    ++harness->emit_failures;
    return ayn::rsinput::StartupResult::kFailed;
  }
  harness->stop_requested = true;
  return ayn::rsinput::StartupResult::kStopped;
}

void WaitBeforeRetry(void* context, uint32_t delay_ms) {
  static_cast<Harness*>(context)->retry_delays_ms.push_back(delay_ms);
}

void WaitAndRequestStop(void* context, uint32_t delay_ms) {
  auto* harness = static_cast<Harness*>(context);
  harness->retry_delays_ms.push_back(delay_ms);
  harness->stop_requested = true;
}

ayn::rsinput::LifecycleCallbacks MakeLifecycleCallbacks(Harness* harness) {
  return {
      StopRequested,       OpenSessionUart,   OpenSessionUinput,
      CloseSessionUart,    CloseSessionUinput, InitializeSession,
      ForwardSession,      WaitBeforeRetry,   harness,
  };
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

void OpenInitializationReadAndEmitFailuresReconnectWithBoundedBackoff() {
  Harness harness;
  const auto callbacks = MakeLifecycleCallbacks(&harness);

  CHECK(ayn::rsinput::RunReconnectLoop(callbacks, {100, 400}) ==
        ayn::rsinput::StartupResult::kStopped);
  CHECK(harness.attempt == 6);
  CHECK(harness.uinput_open_calls == 5);
  CHECK(harness.initialization_calls == 4);
  CHECK(harness.forward_calls == 3);
  CHECK(harness.read_failures == 1);
  CHECK(harness.emit_failures == 1);
  CHECK(harness.uart_close_calls == 5);
  CHECK(harness.uinput_close_calls == 4);
  CHECK((harness.retry_delays_ms ==
         std::vector<uint32_t>{100, 200, 400, 400, 400}));
}

void StopDuringBackoffInterruptsBeforeAnotherAttempt() {
  Harness harness;
  auto callbacks = MakeLifecycleCallbacks(&harness);
  callbacks.wait_before_retry = WaitAndRequestStop;

  CHECK(ayn::rsinput::RunReconnectLoop(callbacks, {250, 5000}) ==
        ayn::rsinput::StartupResult::kStopped);
  CHECK(harness.attempt == 1);
  CHECK((harness.retry_delays_ms == std::vector<uint32_t>{250}));
}

void InvalidRetryPolicyCannotEnterRuntimeIo() {
  Harness harness;
  const auto callbacks = MakeLifecycleCallbacks(&harness);

  CHECK(ayn::rsinput::RunReconnectLoop(callbacks, {0, 5000}) ==
        ayn::rsinput::StartupResult::kFailed);
  CHECK(harness.attempt == 0);
  CHECK(harness.retry_delays_ms.empty());
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
      {"open, initialization, read, and emit failures reconnect with bounded "
       "backoff",
       OpenInitializationReadAndEmitFailuresReconnectWithBoundedBackoff},
      {"stop during backoff interrupts before another attempt",
       StopDuringBackoffInterruptsBeforeAnotherAttempt},
      {"invalid retry policy cannot enter runtime I/O",
       InvalidRetryPolicyCannotEnterRuntimeIo},
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
