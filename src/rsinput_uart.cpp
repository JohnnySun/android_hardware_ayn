// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_uart.h"

#include <cerrno>
#include <fcntl.h>

namespace ayn::rsinput {

int RuntimeUartOpenFlags() {
  return O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK;
}

UartReadDisposition ClassifyUartRead(ssize_t result, int error) {
  if (result > 0) {
    return UartReadDisposition::kData;
  }
  if (result == 0) {
    return UartReadDisposition::kClosed;
  }
  if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) {
    return UartReadDisposition::kRetry;
  }
  return UartReadDisposition::kFailed;
}

}  // namespace ayn::rsinput
