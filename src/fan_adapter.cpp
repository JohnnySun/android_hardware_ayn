// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_adapter.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <string>
#include <system_error>

namespace ayn::fan {

namespace {

constexpr size_t kMaximumSysfsValueBytes = 64;
constexpr int kMaximumThermalZones = 128;
constexpr char kThermalRoot[] = "/sys/class/thermal/thermal_zone";
constexpr char kCpuThermalType[] = "cpu-0-0";

bool AutomaticModeNeedsTemperature(FanMode mode) {
  return mode == FanMode::kQuiet || mode == FanMode::kSport ||
         mode == FanMode::kSmart;
}

bool ParseSignedInteger(const std::string& raw, int* value) {
  if (value == nullptr || raw.empty() || raw.size() > kMaximumSysfsValueBytes) {
    return false;
  }
  size_t size = raw.size();
  if (raw.back() == '\n') {
    --size;
  }
  if (size == 0) {
    return false;
  }
  int parsed = 0;
  const char* begin = raw.data();
  const char* end = begin + size;
  const auto converted = std::from_chars(begin, end, parsed);
  if (converted.ec != std::errc() || converted.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

bool NormalizeTemperature(int raw, int* temperature_c) {
  if (temperature_c == nullptr) {
    return false;
  }
  if (raw >= kMinimumTemperatureC && raw <= kMaximumTemperatureC) {
    *temperature_c = raw;
    return true;
  }
  if (raw > -1000 && raw < 1000) {
    return false;
  }
  if (raw < kMinimumTemperatureC * 1000 ||
      raw > kMaximumTemperatureC * 1000) {
    return false;
  }
  const int normalized = raw / 1000;
  if (normalized < kMinimumTemperatureC ||
      normalized > kMaximumTemperatureC) {
    return false;
  }
  *temperature_c = normalized;
  return true;
}

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

bool ReadCpuTemperatureFromZones(SysfsReader read_file, void* reader_context,
                                 int* temperature_c) {
  if (read_file == nullptr || temperature_c == nullptr) {
    return false;
  }

  bool found = false;
  int observed_temperature_c = 0;
  for (int zone = 0; zone < kMaximumThermalZones; ++zone) {
    const std::string base =
        std::string(kThermalRoot) + std::to_string(zone) + "/";
    std::string type;
    if (!read_file(reader_context, base + "type", &type)) {
      continue;
    }
    if (!type.empty() && type.back() == '\n') {
      type.pop_back();
    }
    if (type != kCpuThermalType) {
      continue;
    }
    if (found) {
      return false;
    }

    std::string raw_temperature;
    int raw = 0;
    if (!read_file(reader_context, base + "temp", &raw_temperature) ||
        !ParseSignedInteger(raw_temperature, &raw) ||
        !NormalizeTemperature(raw, &observed_temperature_c)) {
      return false;
    }
    found = true;
  }
  if (!found) {
    return false;
  }
  *temperature_c = observed_temperature_c;
  return true;
}

bool ReadCpuTemperature(void*, int* temperature_c) {
  return ReadCpuTemperatureFromZones(ReadPosixFile, nullptr, temperature_c);
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
  if (AutomaticModeNeedsTemperature(settings.mode)) {
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
