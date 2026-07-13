// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_adapter.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <string>

namespace ayn::fan {

namespace {

constexpr size_t kMaximumSysfsValueBytes = 64;

bool RuntimeSettingsAreValid(const FanSettings& settings) {
  switch (settings.mode) {
    case FanMode::kDisabled:
    case FanMode::kQuiet:
    case FanMode::kSport:
    case FanMode::kSmart:
      return settings.custom_duty_ns == 0;
    case FanMode::kCustom:
      return settings.custom_duty_ns >= kCustomMinimumDutyNs &&
             settings.custom_duty_ns <= kCustomMaximumDutyNs;
  }
  return false;
}

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

bool WritePosixFile(void*, const std::string& path,
                    const std::string& value) {
  if (value.empty() || value.size() > kMaximumSysfsValueBytes) {
    return false;
  }
  const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }

  size_t offset = 0;
  while (offset < value.size()) {
    const ssize_t count =
        write(fd, value.data() + offset, value.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      return false;
    }
    if (count == 0) {
      close(fd);
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  return CloseSuccessfully(fd);
}

AdapterResult ApplyCurrentSettingsUnlessStopped(
    const std::string& product_device, const SysfsPaths& paths,
    StopRequested stop_requested, void* stop_context,
    FanSettingsReader read_settings, void* settings_context,
    TemperatureReader read_temperature, void* temperature_context,
    SysfsReader read_file, void* reader_context, SysfsWriter write_file,
    void* writer_context) {
  if (!IsSupportedDevice(product_device) || !AreExpectedSysfsPaths(paths) ||
      stop_requested == nullptr || read_settings == nullptr ||
      read_temperature == nullptr || read_file == nullptr ||
      write_file == nullptr) {
    return AdapterResult::kFailedClosed;
  }
  if (stop_requested(stop_context)) {
    return AdapterResult::kStopped;
  }

  FanSettings settings{};
  if (!read_settings(settings_context, &settings) ||
      !RuntimeSettingsAreValid(settings)) {
    return AdapterResult::kFailedClosed;
  }
  if (stop_requested(stop_context)) {
    return AdapterResult::kStopped;
  }

  int temperature_c = 0;
  if (settings.mode == FanMode::kSmart) {
    if (!read_temperature(temperature_context, &temperature_c) ||
        temperature_c < kMinimumTemperatureC ||
        temperature_c > kMaximumTemperatureC) {
      return AdapterResult::kFailedClosed;
    }
    if (stop_requested(stop_context)) {
      return AdapterResult::kStopped;
    }
  }

  const PolicyResult policy = ResolveDuty(settings, temperature_c);
  if (!policy.valid) {
    return AdapterResult::kFailedClosed;
  }
  switch (ApplyDutyUnlessStopped(
      product_device, paths, policy.duty_ns, stop_requested, stop_context,
      read_file, reader_context, write_file, writer_context)) {
    case ApplyResult::kApplied:
      return AdapterResult::kApplied;
    case ApplyResult::kStopped:
      return AdapterResult::kStopped;
    case ApplyResult::kFailedClosed:
      return AdapterResult::kFailedClosed;
    case ApplyResult::kDisableUnconfirmed:
      return AdapterResult::kDisableUnconfirmed;
  }
  return AdapterResult::kFailedClosed;
}

}  // namespace ayn::fan
