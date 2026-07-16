// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_uart.h"

#include <cerrno>
#include <fcntl.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using ayn::rsinput::ClassifyUartRead;
using ayn::rsinput::RuntimeUartOpenFlags;
using ayn::rsinput::UartReadDisposition;

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" +
                             std::to_string(line) + ": CHECK failed: " +
                             expression);
  }
}

void RuntimeReadCannotBlockAfterPollOnPartialFrame() {
  const int flags = RuntimeUartOpenFlags();
  CHECK((flags & O_RDWR) == O_RDWR);
  CHECK((flags & O_NOCTTY) != 0);
  CHECK((flags & O_CLOEXEC) != 0);
  CHECK((flags & O_NONBLOCK) != 0);
}

void TransientReadResultsStayInTheCurrentSession() {
  CHECK(ClassifyUartRead(-1, EINTR) == UartReadDisposition::kRetry);
  CHECK(ClassifyUartRead(-1, EAGAIN) == UartReadDisposition::kRetry);
#if EWOULDBLOCK != EAGAIN
  CHECK(ClassifyUartRead(-1, EWOULDBLOCK) == UartReadDisposition::kRetry);
#endif
  CHECK(ClassifyUartRead(12, 0) == UartReadDisposition::kData);
}

void ClosedAndFailedReadsReconnect() {
  CHECK(ClassifyUartRead(0, 0) == UartReadDisposition::kClosed);
  CHECK(ClassifyUartRead(-1, EIO) == UartReadDisposition::kFailed);
  CHECK(ClassifyUartRead(-1, EBADF) == UartReadDisposition::kFailed);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"runtime read cannot block after poll on partial frame",
       RuntimeReadCannotBlockAfterPollOnPartialFrame},
      {"transient read results stay in the current session",
       TransientReadResultsStayInTheCurrentSession},
      {"closed and failed reads reconnect", ClosedAndFailedReadsReconnect},
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
