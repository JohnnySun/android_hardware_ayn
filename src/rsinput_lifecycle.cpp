// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_lifecycle.h"

#include "ayn/rsinput_protocol.h"

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

}  // namespace ayn::rsinput
