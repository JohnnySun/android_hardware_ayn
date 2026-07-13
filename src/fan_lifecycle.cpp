// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_lifecycle.h"

#include <array>
#include <climits>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ayn::fan {

namespace {

bool ParseSysfsInteger(const std::string& value, int* parsed) {
  if (value.empty()) {
    return false;
  }

  size_t digit_count = value.size();
  if (value.back() == '\n') {
    --digit_count;
  }
  if (digit_count == 0) {
    return false;
  }

  int result = 0;
  for (size_t index = 0; index < digit_count; ++index) {
    const char digit = value[index];
    if (digit < '0' || digit > '9') {
      return false;
    }
    const int numeric_digit = digit - '0';
    if (result > (INT_MAX - numeric_digit) / 10) {
      return false;
    }
    result = result * 10 + numeric_digit;
  }
  *parsed = result;
  return true;
}

bool ConfirmDisabledAfterInterruptedWrite(
    const SysfsPaths& paths, SysfsReader read_file, void* reader_context,
    SysfsWriter write_file, void* writer_context) {
  if (!write_file(writer_context, paths.state, "0")) {
    return false;
  }
  std::string state;
  int parsed_state = -1;
  return read_file(reader_context, paths.state, &state) &&
         ParseSysfsInteger(state, &parsed_state) && parsed_state == 0;
}

bool StateMatches(const SysfsPaths& paths, int expected_state,
                  SysfsReader read_file, void* reader_context) {
  std::string state;
  int parsed_state = -1;
  return read_file(reader_context, paths.state, &state) &&
         ParseSysfsInteger(state, &parsed_state) &&
         parsed_state == expected_state;
}

}  // namespace

ApplyResult ApplyDutyUnlessStopped(
    const std::string& product_device, const SysfsPaths& paths, int duty_ns,
    StopRequested stop_requested, void* stop_context, SysfsReader read_file,
    void* reader_context, SysfsWriter write_file, void* writer_context) {
  if (!IsSupportedDevice(product_device) || !AreExpectedSysfsPaths(paths) ||
      duty_ns < 0 || duty_ns > kCustomMaximumDutyNs ||
      stop_requested == nullptr || read_file == nullptr ||
      write_file == nullptr) {
    return ApplyResult::kFailedClosed;
  }
  if (stop_requested(stop_context)) {
    return ApplyResult::kStopped;
  }

  std::array<int, 3> snapshot{};
  int observed_state = -1;
  const auto fail_closed_from_observed_state = [&]() {
    if (observed_state == 0) {
      return ApplyResult::kFailedClosed;
    }
    return ConfirmDisabledAfterInterruptedWrite(
               paths, read_file, reader_context, write_file, writer_context)
               ? ApplyResult::kFailedClosed
               : ApplyResult::kDisableUnconfirmed;
  };
  const std::array<std::string, 3> snapshot_paths = {
      paths.state,
      paths.duty,
      paths.speed,
  };
  for (size_t index = 0; index < snapshot_paths.size(); ++index) {
    if (stop_requested(stop_context)) {
      return ApplyResult::kStopped;
    }
    std::string value;
    if (!read_file(reader_context, snapshot_paths[index], &value) ||
        !ParseSysfsInteger(value, &snapshot[index])) {
      return fail_closed_from_observed_state();
    }
    if (index == 0) {
      if (snapshot[0] != 0 && snapshot[0] != 1) {
        return fail_closed_from_observed_state();
      }
      observed_state = snapshot[0];
    }
    if (stop_requested(stop_context)) {
      return ApplyResult::kStopped;
    }
  }
  if (snapshot[2] <= 0 || snapshot[1] > snapshot[2]) {
    return fail_closed_from_observed_state();
  }

  std::vector<std::pair<std::string, std::string>> writes = {
      {paths.state, "0"},
  };
  if (duty_ns == 0) {
    writes.emplace_back(paths.duty, "0");
  } else {
    writes.emplace_back(paths.speed, std::to_string(kPwmPeriodNs));
    writes.emplace_back(paths.duty, std::to_string(duty_ns));
    writes.emplace_back(paths.state, "1");
  }

  for (size_t index = 0; index < writes.size(); ++index) {
    if (stop_requested(stop_context)) {
      if (index != 0 && !ConfirmDisabledAfterInterruptedWrite(
                            paths, read_file, reader_context, write_file,
                            writer_context)) {
        return ApplyResult::kDisableUnconfirmed;
      }
      return ApplyResult::kStopped;
    }
    if (!write_file(writer_context, writes[index].first,
                    writes[index].second)) {
      return ConfirmDisabledAfterInterruptedWrite(
                 paths, read_file, reader_context, write_file, writer_context)
                 ? ApplyResult::kFailedClosed
                 : ApplyResult::kDisableUnconfirmed;
    }
    if (stop_requested(stop_context)) {
      return ConfirmDisabledAfterInterruptedWrite(
                 paths, read_file, reader_context, write_file, writer_context)
                 ? ApplyResult::kStopped
                 : ApplyResult::kDisableUnconfirmed;
    }
    if (writes[index].first == paths.state) {
      const int expected_state = writes[index].second == "0" ? 0 : 1;
      if (!StateMatches(paths, expected_state, read_file, reader_context)) {
        return ConfirmDisabledAfterInterruptedWrite(
                   paths, read_file, reader_context, write_file,
                   writer_context)
                   ? ApplyResult::kFailedClosed
                   : ApplyResult::kDisableUnconfirmed;
      }
    }
  }
  return ApplyResult::kApplied;
}

SmartLoopResult RunSmartLoopUnlessStopped(
    StopRequested stop_requested, void* stop_context,
    TemperatureReader read_temperature, void* temperature_context,
    DutyApplier apply_duty, void* apply_context,
    SleepForSeconds sleep_for_seconds, void* sleep_context) {
  if (stop_requested == nullptr || read_temperature == nullptr ||
      apply_duty == nullptr || sleep_for_seconds == nullptr) {
    return SmartLoopResult::kFailedClosed;
  }

  while (true) {
    if (stop_requested(stop_context)) {
      return SmartLoopResult::kStopped;
    }

    int temperature_c = 0;
    if (!read_temperature(temperature_context, &temperature_c)) {
      return SmartLoopResult::kFailedClosed;
    }
    if (stop_requested(stop_context)) {
      return SmartLoopResult::kStopped;
    }

    const PolicyResult policy =
        ResolveDuty({FanMode::kSmart, 0}, temperature_c);
    if (!policy.valid || !apply_duty(apply_context, policy.duty_ns)) {
      return SmartLoopResult::kFailedClosed;
    }
    if (stop_requested(stop_context)) {
      return SmartLoopResult::kStopped;
    }
    if (!sleep_for_seconds(sleep_context, kSmartPollIntervalSeconds)) {
      return SmartLoopResult::kFailedClosed;
    }
  }
}

}  // namespace ayn::fan
