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

StartupResult OpenUartUnlessStopped(StopRequested stop_requested,
                                    void* stop_context,
                                    UartOpener open_uart,
                                    void* opener_context,
                                    int* uart_fd);
StartupResult SendInitializationFramesUnlessStopped(
    StopRequested stop_requested, void* stop_context, FrameWriter write_frame,
    void* writer_context);

}  // namespace ayn::rsinput
