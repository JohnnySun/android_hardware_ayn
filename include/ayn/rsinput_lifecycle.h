// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ayn/rsinput_protocol.h"

#include <cstddef>
#include <cstdint>

namespace ayn::rsinput {

enum class StartupResult {
  kCompleted,
  kStopped,
  kFailed,
};

using StopRequested = bool (*)(void* context);
using UartOpener = int (*)(void* context);
using FrameWriter = bool (*)(void* context, const uint8_t* data, size_t size);
using McuPowerWriter = bool (*)(void* context, const char* path,
                                const uint8_t* data, size_t size);
using PowerController = bool (*)(void* context);
using ResourceCloser = void (*)(void* context, int fd);
using SessionInitializer = StartupResult (*)(void* context, int uart_fd);
using SessionForwarder = StartupResult (*)(void* context, int uart_fd,
                                           int uinput_fd);
using RetryWaiter = void (*)(void* context, uint32_t delay_ms);

enum class HandshakeReadResult {
  kData,
  kTimeout,
  kFailed,
};

using HandshakeReader = HandshakeReadResult (*)(
    void* context, uint8_t* data, size_t capacity, size_t* size,
    uint32_t timeout_ms);
using MonotonicClock = uint64_t (*)(void* context);

struct HandshakeCallbacks {
  StopRequested stop_requested;
  FrameWriter write_frame;
  HandshakeReader read_bytes;
  MonotonicClock monotonic_ms;
  void* context;
};

enum class HandshakeFailure {
  kNone,
  kInvalidConfiguration,
  kWrite,
  kRead,
  kTimeout,
  kProtocol,
};

struct HandshakeDiagnostics {
  HandshakeState state = HandshakeState::kNotStarted;
  HandshakeStats stats{};
  uint8_t expected_response_type = 0;
  HandshakeFailure failure = HandshakeFailure::kNone;
};

struct LifecycleCallbacks {
  StopRequested stop_requested;
  // RunReconnectLoop owns one successful power_on across all reconnects and
  // calls power_off once on exit. Failed power_on calls do not earn ownership.
  PowerController power_on;
  PowerController power_off;
  UartOpener open_uart;
  UartOpener open_uinput;
  ResourceCloser close_uart;
  ResourceCloser close_uinput;
  SessionInitializer initialize_session;
  SessionForwarder forward_session;
  RetryWaiter wait_before_retry;
  void* context;
};

struct RetryPolicy {
  uint32_t initial_delay_ms;
  uint32_t max_delay_ms;
};

StartupResult OpenUartUnlessStopped(StopRequested stop_requested,
                                    void* stop_context,
                                    UartOpener open_uart,
                                    void* opener_context,
                                    int* uart_fd);
bool WriteMcuPowerState(McuPowerWriter writer, void* context, bool enabled);
StartupResult RunQ9Handshake(const HandshakeCallbacks& callbacks,
                             uint32_t response_timeout_ms,
                             HandshakeDiagnostics* diagnostics);
StartupResult RunReconnectLoop(const LifecycleCallbacks& callbacks,
                               RetryPolicy retry_policy);

}  // namespace ayn::rsinput
