// SPDX-License-Identifier: Apache-2.0

#include "ayn/performance_adapter.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>

namespace ayn::performance {

namespace {

constexpr size_t kMaximumSysfsValueBytes = 64;

bool CloseSuccessfully(int fd) {
  return close(fd) == 0;
}

}  // namespace

bool ReadPosixFile(void*, const std::string& path, std::string* value) {
  if (value == nullptr) {
    return false;
  }
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }

  std::string result;
  char buffer[kMaximumSysfsValueBytes + 1];
  while (true) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      return false;
    }
    if (count == 0) {
      break;
    }
    result.append(buffer, static_cast<size_t>(count));
    if (result.size() > kMaximumSysfsValueBytes) {
      close(fd);
      return false;
    }
  }
  if (!CloseSuccessfully(fd)) {
    return false;
  }
  *value = result;
  return true;
}

}  // namespace ayn::performance
