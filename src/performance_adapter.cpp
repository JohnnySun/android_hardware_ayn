// SPDX-License-Identifier: Apache-2.0

#include "ayn/performance_adapter.h"

#include <cstring>

// The host tests compile this file without an Android classpath, so the
// diagnostics are optional rather than a build dependency. Without them a
// failed mode change reports nothing but "write failed" and every diagnosis
// has to be made from outside, by guessing which of ten nodes refused.
#ifdef __ANDROID__
#include <android-base/logging.h>
#define AYN_PERF_LOG(expr) LOG(ERROR) << expr
#else
#define AYN_PERF_LOG(expr) ((void)0)
#endif

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace ayn::performance {

namespace {

constexpr size_t kMaximumSysfsValueBytes = 64;

constexpr std::array<std::string_view, 10> kStockWriterPaths = {{
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
    "/sys/devices/system/cpu/cpu3/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu3/cpufreq/scaling_max_freq",
    "/sys/devices/system/cpu/cpu7/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu7/cpufreq/scaling_max_freq",
    "/sys/class/kgsl/kgsl-3d0/devfreq/min_freq",
    "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq",
    "/sys/devices/system/cpu/bus_dcvs/DDR/hw_min_freq",
    "/sys/devices/system/cpu/bus_dcvs/DDR/soc:qcom,memlat:ddr:gold/min_freq",
}};

bool CloseSuccessfully(int fd) {
  return close(fd) == 0;
}

bool IsAllowedWriterPath(const std::string& path) {
  for (const std::string_view allowed : kStockWriterPaths) {
    if (path == allowed) {
      return true;
    }
  }
  return false;
}

bool IsDecimalValue(const std::string& value) {
  if (value.empty() || value.size() > kMaximumSysfsValueBytes) {
    return false;
  }
  for (const char character : value) {
    if (character < '0' || character > '9') {
      return false;
    }
  }
  return true;
}

int OpenWriter(void*, const char* path) {
  return open(path, O_WRONLY | O_CLOEXEC);
}

ssize_t WriteWriter(void*, int fd, const void* data, size_t size) {
  return write(fd, data, size);
}

int CloseWriter(void*, int fd) {
  return close(fd);
}

bool Readback(void*, const std::string& path, std::string* value) {
  return ReadPosixFile(nullptr, path, value);
}

const PosixWriterOperations kDefaultWriterOperations = {
    nullptr, OpenWriter, WriteWriter, CloseWriter, Readback};

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

bool WritePosixFile(void* context, const std::string& path,
                    const std::string& value) {
  if (!IsAllowedWriterPath(path)) {
    AYN_PERF_LOG("performance write refused: " << path
                 << " is not an allowed writer path");
    return false;
  }
  if (!IsDecimalValue(value)) {
    AYN_PERF_LOG("performance write refused: " << value
                 << " is not a decimal value for " << path);
    return false;
  }
  const auto* operations = context == nullptr
                               ? &kDefaultWriterOperations
                               : static_cast<const PosixWriterOperations*>(
                                     context);
  if (operations->open_writer == nullptr ||
      operations->write_writer == nullptr ||
      operations->close_writer == nullptr || operations->readback == nullptr) {
    return false;
  }

  int fd = -1;
  do {
    fd = operations->open_writer(operations->context, path.c_str());
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    AYN_PERF_LOG("performance write could not open " << path << ": "
                 << strerror(errno));
    return false;
  }

  size_t offset = 0;
  bool write_complete = true;
  while (offset < value.size()) {
    const ssize_t count = operations->write_writer(
        operations->context, fd, value.data() + offset,
        value.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      write_complete = false;
      break;
    }
    if (count == 0) {
      write_complete = false;
      break;
    }
    offset += static_cast<size_t>(count);
  }

  const bool close_complete =
      operations->close_writer(operations->context, fd) == 0;
  if (!write_complete || !close_complete) {
    AYN_PERF_LOG("performance write of " << value << " to " << path
                 << " failed: " << strerror(errno));
    return false;
  }

  std::string observed;
  if (!operations->readback(operations->context, path, &observed)) {
    AYN_PERF_LOG("performance write could not read back " << path);
    return false;
  }
  if (!observed.empty() && observed.back() == '\n') {
    observed.pop_back();
  }
  if (observed != value) {
    // Not a failure. The write landed and something else moved the value,
    // which on this device is the QTI perf stack doing its job. Deciding what
    // that means belongs to the service, which reads the node again itself;
    // this layer only reports whether the write happened.
    //
    // A readback that cannot be performed at all is still fail-closed above,
    // because then nothing is known.
    AYN_PERF_LOG("performance write to " << path << " asked for " << value
                 << " and read back " << observed);
  }
  return true;
}

}  // namespace ayn::performance
