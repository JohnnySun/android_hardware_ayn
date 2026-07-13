// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ayn/fan_policy.h"

#include <string>

namespace ayn::fan {

enum class ApplyResult {
  kApplied,
  kStopped,
  kFailedClosed,
  kDisableUnconfirmed,
};

enum class SmartLoopResult {
  kStopped,
  kFailedClosed,
};

using StopRequested = bool (*)(void* context);
using SysfsReader = bool (*)(void* context, const std::string& path,
                             std::string* value);
using SysfsWriter = bool (*)(void* context, const std::string& path,
                             const std::string& value);
using TemperatureReader = bool (*)(void* context, int* temperature_c);
using DutyApplier = bool (*)(void* context, int duty_ns);
using SleepForSeconds = bool (*)(void* context, int seconds);

ApplyResult ApplyDutyUnlessStopped(
    const std::string& product_device, const SysfsPaths& paths, int duty_ns,
    StopRequested stop_requested, void* stop_context, SysfsReader read_file,
    void* reader_context, SysfsWriter write_file, void* writer_context);
SmartLoopResult RunSmartLoopUnlessStopped(
    StopRequested stop_requested, void* stop_context,
    TemperatureReader read_temperature, void* temperature_context,
    DutyApplier apply_duty, void* apply_context,
    SleepForSeconds sleep_for_seconds, void* sleep_context);

}  // namespace ayn::fan
