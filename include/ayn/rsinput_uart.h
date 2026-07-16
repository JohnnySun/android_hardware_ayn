// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sys/types.h>

namespace ayn::rsinput {

enum class UartReadDisposition {
  kData,
  kRetry,
  kClosed,
  kFailed,
};

int RuntimeUartOpenFlags();
UartReadDisposition ClassifyUartRead(ssize_t result, int error);

}  // namespace ayn::rsinput
