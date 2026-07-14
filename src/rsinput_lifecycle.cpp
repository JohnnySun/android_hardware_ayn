// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_lifecycle.h"

#include "ayn/rsinput_protocol.h"

#include <algorithm>
#include <vector>

namespace ayn::rsinput {

StartupResult OpenUartUnlessStopped(StopRequested stop_requested,
                                    void* stop_context,
                                    UartOpener open_uart,
                                    void* opener_context,
                                    int* uart_fd) {
  if (stop_requested(stop_context)) {
    return StartupResult::kStopped;
  }
  *uart_fd = open_uart(opener_context);
  if (*uart_fd < 0) {
    return StartupResult::kFailed;
  }
  return stop_requested(stop_context) ? StartupResult::kStopped
                                      : StartupResult::kCompleted;
}

StartupResult SendInitializationFramesUnlessStopped(
    StopRequested stop_requested, void* stop_context, FrameWriter write_frame,
    void* writer_context) {
  const auto frames = BuildInitializationFrames();
  for (const std::vector<uint8_t>& frame : frames) {
    if (stop_requested(stop_context)) {
      return StartupResult::kStopped;
    }
    if (!write_frame(writer_context, frame.data(), frame.size())) {
      return StartupResult::kFailed;
    }
  }
  return StartupResult::kCompleted;
}

StartupResult RunReconnectLoop(const LifecycleCallbacks& callbacks,
                               RetryPolicy retry_policy) {
  if (callbacks.stop_requested == nullptr || callbacks.open_uart == nullptr ||
      callbacks.open_uinput == nullptr || callbacks.close_uart == nullptr ||
      callbacks.close_uinput == nullptr ||
      callbacks.initialize_session == nullptr ||
      callbacks.forward_session == nullptr ||
      callbacks.wait_before_retry == nullptr ||
      retry_policy.initial_delay_ms == 0 ||
      retry_policy.max_delay_ms < retry_policy.initial_delay_ms) {
    return StartupResult::kFailed;
  }

  uint32_t retry_delay_ms = retry_policy.initial_delay_ms;
  while (!callbacks.stop_requested(callbacks.context)) {
    int uart_fd = -1;
    StartupResult attempt_result = OpenUartUnlessStopped(
        callbacks.stop_requested, callbacks.context, callbacks.open_uart,
        callbacks.context, &uart_fd);

    int uinput_fd = -1;
    if (attempt_result == StartupResult::kCompleted) {
      attempt_result = OpenUartUnlessStopped(
          callbacks.stop_requested, callbacks.context, callbacks.open_uinput,
          callbacks.context, &uinput_fd);
    }
    if (attempt_result == StartupResult::kCompleted) {
      attempt_result =
          callbacks.initialize_session(callbacks.context, uart_fd);
    }
    if (attempt_result == StartupResult::kCompleted) {
      attempt_result = callbacks.forward_session(callbacks.context, uart_fd,
                                                  uinput_fd);
    }

    if (uinput_fd >= 0) {
      callbacks.close_uinput(callbacks.context, uinput_fd);
    }
    if (uart_fd >= 0) {
      callbacks.close_uart(callbacks.context, uart_fd);
    }

    if (attempt_result == StartupResult::kStopped ||
        callbacks.stop_requested(callbacks.context)) {
      return StartupResult::kStopped;
    }

    callbacks.wait_before_retry(callbacks.context, retry_delay_ms);
    if (callbacks.stop_requested(callbacks.context)) {
      return StartupResult::kStopped;
    }
    retry_delay_ms = std::min(
        retry_policy.max_delay_ms,
        retry_delay_ms > retry_policy.max_delay_ms / 2
            ? retry_policy.max_delay_ms
            : retry_delay_ms * 2);
  }
  return StartupResult::kStopped;
}

}  // namespace ayn::rsinput
