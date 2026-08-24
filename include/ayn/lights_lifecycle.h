// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ayn/lights_policy.h"

#include <string>

namespace ayn::lights {

// sysfs attributes only raise inotify when the driver calls sysfs_notify, and
// not every write path does, so the clamp polls instead of watching. The
// interval bounds how long the LED can sit above the cap after the stock HAL
// writes it.
constexpr int kPollIntervalMilliseconds = 100;

enum class ClampResult {
  kClamped,
  kAlreadyWithinCap,
  kStopped,
  kFailedClosed,
};

enum class LoopResult {
  kStopped,
  kFailedClosed,
};

using StopRequested = bool (*)(void* context);
using SysfsReader = bool (*)(void* context, const std::string& path,
                             std::string* value);
using SysfsWriter = bool (*)(void* context, const std::string& path,
                             const std::string& value);
using SleepForMilliseconds = bool (*)(void* context, int milliseconds);

// Reads the three channels, clamps them, and writes back only what changed.
// Any unreadable channel, unexpected path, or unknown device stops the pass
// without writing anything.
ClampResult ClampOnceUnlessStopped(const std::string& product_device,
                                   const SysfsPaths& paths, int cap,
                                   StopRequested stop_requested,
                                   void* stop_context, SysfsReader read_file,
                                   void* reader_context, SysfsWriter write_file,
                                   void* writer_context);

LoopResult RunClampLoopUnlessStopped(
    const std::string& product_device, const SysfsPaths& paths, int cap,
    StopRequested stop_requested, void* stop_context, SysfsReader read_file,
    void* reader_context, SysfsWriter write_file, void* writer_context,
    SleepForMilliseconds sleep_for_milliseconds, void* sleep_context);

bool ParseChannel(const std::string& raw, int* value);

}  // namespace ayn::lights
