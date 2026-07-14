// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_lifecycle.h"
#include "ayn/rsinput_parser.h"
#include "ayn/rsinput_protocol.h"

#include <algorithm>
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
  bool fail_power_on = false;
  bool fail_power_off = false;
  size_t power_on_calls = 0;
  size_t power_off_calls = 0;
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

struct ControlWrite {
  std::string path;
  std::vector<uint8_t> bytes;
};

struct PowerHarness {
  bool result = true;
  std::vector<ControlWrite> writes;
};

struct HandshakeRead {
  ayn::rsinput::HandshakeReadResult result;
  std::vector<uint8_t> bytes;
  uint32_t elapsed_ms;
};

struct HandshakeHarness {
  bool stop_requested = false;
  uint64_t now_ms = 0;
  size_t read_index = 0;
  std::vector<std::vector<uint8_t>> writes;
  std::vector<HandshakeRead> reads;
  std::vector<size_t> writes_seen_by_read;
  std::vector<uint32_t> requested_timeouts_ms;
};

bool StopRequested(void* context) {
  return static_cast<Harness*>(context)->stop_requested;
}

bool PowerOn(void* context) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->power_on_calls;
  return !harness->fail_power_on;
}

bool PowerOff(void* context) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->power_off_calls;
  return !harness->fail_power_off;
}

bool CaptureControlWrite(void* context, const char* path, const uint8_t* data,
                         size_t size) {
  auto* harness = static_cast<PowerHarness*>(context);
  harness->writes.push_back({path, {data, data + size}});
  return harness->result;
}

bool HandshakeStopRequested(void* context) {
  return static_cast<HandshakeHarness*>(context)->stop_requested;
}

bool WriteHandshakeFrame(void* context, const uint8_t* data, size_t size) {
  auto* harness = static_cast<HandshakeHarness*>(context);
  harness->writes.emplace_back(data, data + size);
  return true;
}

ayn::rsinput::HandshakeReadResult ReadHandshakeBytes(
    void* context, uint8_t* data, size_t capacity, size_t* size,
    uint32_t timeout_ms) {
  auto* harness = static_cast<HandshakeHarness*>(context);
  harness->writes_seen_by_read.push_back(harness->writes.size());
  harness->requested_timeouts_ms.push_back(timeout_ms);
  if (harness->read_index == harness->reads.size()) {
    harness->now_ms += timeout_ms;
    *size = 0;
    return ayn::rsinput::HandshakeReadResult::kTimeout;
  }

  const HandshakeRead& read = harness->reads[harness->read_index++];
  harness->now_ms += std::min(read.elapsed_ms, timeout_ms);
  if (read.elapsed_ms > timeout_ms) {
    *size = 0;
    return ayn::rsinput::HandshakeReadResult::kTimeout;
  }
  CHECK(read.bytes.size() <= capacity);
  std::copy(read.bytes.begin(), read.bytes.end(), data);
  *size = read.bytes.size();
  return read.result;
}

uint64_t HandshakeNowMs(void* context) {
  return static_cast<HandshakeHarness*>(context)->now_ms;
}

std::vector<uint8_t> MakeResponseFrame(uint8_t sequence, uint8_t type,
                                       const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> frame(ayn::rsinput::Parser::kMagic.begin(),
                             ayn::rsinput::Parser::kMagic.end());
  frame.push_back(sequence);
  frame.push_back(type);
  frame.push_back(static_cast<uint8_t>(payload.size() & 0xff));
  frame.push_back(static_cast<uint8_t>(payload.size() >> 8));
  frame.insert(frame.end(), payload.begin(), payload.end());
  uint8_t checksum = 0;
  for (size_t index = 4; index < frame.size(); ++index) {
    checksum ^= frame[index];
  }
  frame.push_back(checksum);
  return frame;
}

ayn::rsinput::HandshakeCallbacks MakeHandshakeCallbacks(
    HandshakeHarness* harness) {
  return {HandshakeStopRequested, WriteHandshakeFrame, ReadHandshakeBytes,
          HandshakeNowMs, harness};
}

void CheckQ9HandshakeWrites(const HandshakeHarness& harness) {
  const auto expected = ayn::rsinput::BuildQ9HandshakeFrames();
  CHECK(harness.writes.size() == expected.size());
  CHECK(std::equal(harness.writes.begin(), harness.writes.end(),
                   expected.begin()));
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
      StopRequested,       PowerOn,            PowerOff,
      OpenSessionUart,     OpenSessionUinput,  CloseSessionUart,
      CloseSessionUinput,  InitializeSession,  ForwardSession,
      WaitBeforeRetry,     harness,
  };
}

void McuPowerControlUsesExactStockPathAndBytes() {
  PowerHarness harness;

  CHECK(ayn::rsinput::WriteMcuPowerState(CaptureControlWrite, &harness, true));
  CHECK(ayn::rsinput::WriteMcuPowerState(CaptureControlWrite, &harness,
                                         false));
  CHECK(harness.writes.size() == 2);
  CHECK(harness.writes[0].path ==
        "/sys/devices/platform/rsgpio/driver_ctl");
  CHECK(harness.writes[1].path == harness.writes[0].path);
  CHECK((harness.writes[0].bytes ==
         std::vector<uint8_t>{'m', 'c', 'u', 'p', 'o', 'w', 'e', 'r', ' ',
                              '1'}));
  CHECK((harness.writes[1].bytes ==
         std::vector<uint8_t>{'m', 'c', 'u', 'p', 'o', 'w', 'e', 'r', ' ',
                              '0'}));
}

void Q9HandshakeWaitsForEachResponseBeforeAdvancing() {
  HandshakeHarness harness;
  const auto type_one = MakeResponseFrame(0x40, 0x01, {0x01});
  const auto type_two = MakeResponseFrame(0x41, 0x02,
                                          std::vector<uint8_t>(14, 0));
  const std::vector<uint8_t> type_one_prefix(type_one.begin(),
                                              type_one.end() - 1);
  const std::vector<uint8_t> type_one_checksum(type_one.end() - 1,
                                                type_one.end());
  harness.reads = {
      {ayn::rsinput::HandshakeReadResult::kData, type_one_prefix, 10},
      {ayn::rsinput::HandshakeReadResult::kData, type_one_checksum, 10},
      {ayn::rsinput::HandshakeReadResult::kData, type_two, 10},
  };
  ayn::rsinput::HandshakeDiagnostics diagnostics;

  CHECK(ayn::rsinput::RunQ9Handshake(MakeHandshakeCallbacks(&harness), 500,
                                     &diagnostics) ==
        ayn::rsinput::StartupResult::kCompleted);
  CheckQ9HandshakeWrites(harness);
  CHECK((harness.writes_seen_by_read == std::vector<size_t>{1, 1, 3}));
  CHECK(diagnostics.state == ayn::rsinput::HandshakeState::kInitialized);
  CHECK(diagnostics.expected_response_type == 0);
}

void PreconfigurationTypeTwoCannotCompleteHandshake() {
  HandshakeHarness harness;
  auto responses = MakeResponseFrame(0x50, 0x01, {0x01});
  const auto early_type_two =
      MakeResponseFrame(0x51, 0x02, std::vector<uint8_t>(14, 0));
  responses.insert(responses.end(), early_type_two.begin(),
                   early_type_two.end());
  harness.reads = {
      {ayn::rsinput::HandshakeReadResult::kData, responses, 10},
      {ayn::rsinput::HandshakeReadResult::kData,
       MakeResponseFrame(0x52, 0x02, std::vector<uint8_t>(14, 0)), 10},
  };
  ayn::rsinput::HandshakeDiagnostics diagnostics;

  CHECK(ayn::rsinput::RunQ9Handshake(MakeHandshakeCallbacks(&harness), 500,
                                     &diagnostics) ==
        ayn::rsinput::StartupResult::kCompleted);
  CheckQ9HandshakeWrites(harness);
  CHECK((harness.writes_seen_by_read == std::vector<size_t>{1, 3}));
  CHECK(diagnostics.state == ayn::rsinput::HandshakeState::kInitialized);
}

void MalformedAndUnrelatedPacketsRemainUninitializedUntilTimeout() {
  HandshakeHarness harness;
  auto malformed = MakeResponseFrame(0x60, 0x01, {0x01});
  malformed.back() ^= 0x80;
  harness.reads = {
      {ayn::rsinput::HandshakeReadResult::kData, malformed, 10},
      {ayn::rsinput::HandshakeReadResult::kData,
       MakeResponseFrame(0x61, 0x02, std::vector<uint8_t>(14, 0)), 10},
      {ayn::rsinput::HandshakeReadResult::kTimeout, {}, 480},
  };
  ayn::rsinput::HandshakeDiagnostics diagnostics;

  CHECK(ayn::rsinput::RunQ9Handshake(MakeHandshakeCallbacks(&harness), 500,
                                     &diagnostics) ==
        ayn::rsinput::StartupResult::kFailed);
  CHECK(harness.writes.size() == 1);
  CHECK(diagnostics.state == ayn::rsinput::HandshakeState::kFailed);
  CHECK(diagnostics.expected_response_type == 0x01);
  CHECK(diagnostics.failure == ayn::rsinput::HandshakeFailure::kTimeout);
  CHECK(diagnostics.stats.malformed_frames >= 1);
  CHECK(diagnostics.stats.unrelated_packets == 1);
  CHECK((harness.requested_timeouts_ms ==
         std::vector<uint32_t>{500, 490, 480}));
}

void MissingTypeTwoResponseFailsClosedAfterConfiguration() {
  HandshakeHarness harness;
  harness.reads = {
      {ayn::rsinput::HandshakeReadResult::kData,
       MakeResponseFrame(0x70, 0x01, {0x01}), 10},
      {ayn::rsinput::HandshakeReadResult::kTimeout, {}, 500},
  };
  ayn::rsinput::HandshakeDiagnostics diagnostics;

  CHECK(ayn::rsinput::RunQ9Handshake(MakeHandshakeCallbacks(&harness), 500,
                                     &diagnostics) ==
        ayn::rsinput::StartupResult::kFailed);
  CheckQ9HandshakeWrites(harness);
  CHECK(diagnostics.state == ayn::rsinput::HandshakeState::kFailed);
  CHECK(diagnostics.expected_response_type == 0x02);
  CHECK(diagnostics.failure == ayn::rsinput::HandshakeFailure::kTimeout);
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
  CHECK(harness.power_on_calls == 1);
  CHECK(harness.power_off_calls == 1);
}

void PowerOnFailurePreventsUartOpenAndHandshake() {
  Harness harness;
  harness.fail_power_on = true;
  auto callbacks = MakeLifecycleCallbacks(&harness);
  callbacks.wait_before_retry = WaitAndRequestStop;

  CHECK(ayn::rsinput::RunReconnectLoop(callbacks, {250, 5000}) ==
        ayn::rsinput::StartupResult::kStopped);
  CHECK(harness.power_on_calls == 1);
  CHECK(harness.power_off_calls == 0);
  CHECK(harness.attempt == 0);
  CHECK(harness.uinput_open_calls == 0);
  CHECK(harness.initialization_calls == 0);
  CHECK((harness.retry_delays_ms == std::vector<uint32_t>{250}));
}

void OwnedPowerIsReleasedOnceWhenCleanupIsSafe() {
  Harness harness;
  harness.attempt = 5;
  const auto callbacks = MakeLifecycleCallbacks(&harness);

  CHECK(ayn::rsinput::RunReconnectLoop(callbacks, {250, 5000}) ==
        ayn::rsinput::StartupResult::kStopped);
  CHECK(harness.power_on_calls == 1);
  CHECK(harness.power_off_calls == 1);
  CHECK(harness.attempt == 6);
}

void PowerOffFailureMakesLifecycleFailureVisible() {
  Harness harness;
  harness.attempt = 5;
  harness.fail_power_off = true;
  const auto callbacks = MakeLifecycleCallbacks(&harness);

  CHECK(ayn::rsinput::RunReconnectLoop(callbacks, {250, 5000}) ==
        ayn::rsinput::StartupResult::kFailed);
  CHECK(harness.power_on_calls == 1);
  CHECK(harness.power_off_calls == 1);
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
      {"MCU power control uses exact stock path and bytes",
       McuPowerControlUsesExactStockPathAndBytes},
      {"Q9 handshake waits for each response before advancing",
       Q9HandshakeWaitsForEachResponseBeforeAdvancing},
      {"preconfiguration type two cannot complete handshake",
       PreconfigurationTypeTwoCannotCompleteHandshake},
      {"malformed and unrelated packets remain uninitialized until timeout",
       MalformedAndUnrelatedPacketsRemainUninitializedUntilTimeout},
      {"missing type two response fails closed after configuration",
       MissingTypeTwoResponseFailsClosedAfterConfiguration},
      {"open, initialization, read, and emit failures reconnect with bounded "
       "backoff",
       OpenInitializationReadAndEmitFailuresReconnectWithBoundedBackoff},
      {"power-on failure prevents UART open and handshake",
       PowerOnFailurePreventsUartOpenAndHandshake},
      {"owned power is released once when cleanup is safe",
       OwnedPowerIsReleasedOnceWhenCleanupIsSafe},
      {"power-off failure makes lifecycle failure visible",
       PowerOffFailureMakesLifecycleFailureVisible},
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
