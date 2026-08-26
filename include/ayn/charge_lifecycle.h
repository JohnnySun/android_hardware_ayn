// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ayn/charge_policy.h"

#include <string>

namespace ayn::charge {

// Capacity moves slowly, so the loop can be lazy. The cost of a long interval
// is only overshooting the stop threshold by a fraction of a percent.
constexpr int kPollIntervalSeconds = 30;

enum class ApplyResult {
  kRestricted,
  kReleased,
  kUnchanged,
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
using SleepForSeconds = bool (*)(void* context, int seconds);

// Capacity does not come from sysfs. /sys/class/power_supply/battery/capacity
// carries vendor_sysfs_battery_supply, a vendor-private type that a coredomain
// cannot be granted from system_ext policy, so reading it directly only ever
// worked because the device was Permissive - and it was 460 of this daemon's
// denials. The health HAL publishes the same number over binder and is the
// route Treble intends, so the source is a callback rather than a path.
struct CapacitySource {
  bool (*read)(void* context, int* capacity_percent);
  void* context;
};

bool ParseRestrictFlag(const std::string& raw, bool* restricted);

// Clears the restriction whatever the settings say, and deliberately without a
// stop check: this is what runs on the way out, so honouring a stop request
// here would be the one case that leaves the battery unable to charge. Also run
// at startup, so a restriction left behind by a killed daemon cannot outlive
// it.
ApplyResult ReleaseRestriction(const std::string& product_device,
                               const SysfsPaths& paths, SysfsReader read_file,
                               void* reader_context, SysfsWriter write_file,
                               void* writer_context);

ApplyResult ApplyOnceUnlessStopped(const std::string& product_device,
                                   const SysfsPaths& paths,
                                   const LimitSettings& settings,
                                   const CapacitySource& capacity,
                                   StopRequested stop_requested,
                                   void* stop_context, SysfsReader read_file,
                                   void* reader_context, SysfsWriter write_file,
                                   void* writer_context);

LoopResult RunLimitLoopUnlessStopped(
    const std::string& product_device, const SysfsPaths& paths,
    const LimitSettings& settings, const CapacitySource& capacity,
    StopRequested stop_requested, void* stop_context, SysfsReader read_file,
    void* reader_context, SysfsWriter write_file, void* writer_context,
    SleepForSeconds sleep_for_seconds, void* sleep_context);

}  // namespace ayn::charge
