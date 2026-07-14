// SPDX-License-Identifier: Apache-2.0
#pragma once

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
using ResourceCloser = void (*)(void* context, int fd);
using SessionInitializer = StartupResult (*)(void* context, int uart_fd);
using SessionForwarder = StartupResult (*)(void* context, int uart_fd,
                                           int uinput_fd);
using RetryWaiter = void (*)(void* context, uint32_t delay_ms);

struct LifecycleCallbacks {
  StopRequested stop_requested;
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
StartupResult SendInitializationFramesUnlessStopped(
    StopRequested stop_requested, void* stop_context, FrameWriter write_frame,
    void* writer_context);
StartupResult RunReconnectLoop(const LifecycleCallbacks& callbacks,
                               RetryPolicy retry_policy);

}  // namespace ayn::rsinput
