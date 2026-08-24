// SPDX-License-Identifier: Apache-2.0
#include "ayn/charge_lifecycle.h"

#include <cstdlib>
#include <string>

namespace ayn::charge {
namespace {

bool ReadTrimmed(SysfsReader read_file, void* reader_context,
                 const std::string& path, std::string* value) {
  return read_file(reader_context, path, value);
}

bool ParseBoundedInteger(const std::string& raw, int low, int high,
                         int* value) {
  std::size_t index = 0;
  while (index < raw.size() && (raw[index] == ' ' || raw[index] == '\t')) {
    ++index;
  }
  std::size_t digits = index;
  while (digits < raw.size() && raw[digits] >= '0' && raw[digits] <= '9') {
    ++digits;
  }
  if (digits == index) {
    return false;
  }
  for (std::size_t tail = digits; tail < raw.size(); ++tail) {
    const char character = raw[tail];
    if (character != '\n' && character != '\r' && character != ' ' &&
        character != '\t' && character != '\0') {
      return false;
    }
  }
  const long parsed =
      std::strtol(raw.substr(index, digits - index).c_str(), nullptr, 10);
  if (parsed < low || parsed > high) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

bool WriteRestriction(SysfsWriter write_file, void* writer_context,
                      const SysfsPaths& paths, bool restrict_charging) {
  if (restrict_charging) {
    // The current has to be in place before the restriction is enabled, or the
    // charger runs at the previous limit for the gap between the two writes.
    if (!write_file(writer_context, paths.restrict_cur,
                    std::to_string(kRestrictedCurrentUa) + "\n")) {
      return false;
    }
    return write_file(writer_context, paths.restrict_chg, "1\n");
  }
  return write_file(writer_context, paths.restrict_chg, "0\n");
}

bool ReadRestricted(SysfsReader read_file, void* reader_context,
                    const SysfsPaths& paths, bool* restricted) {
  std::string raw;
  if (!ReadTrimmed(read_file, reader_context, paths.restrict_chg, &raw)) {
    return false;
  }
  return ParseRestrictFlag(raw, restricted);
}

}  // namespace

bool ParseCapacity(const std::string& raw, int* capacity_percent) {
  return ParseBoundedInteger(raw, 0, 100, capacity_percent);
}

bool ParseRestrictFlag(const std::string& raw, bool* restricted) {
  int value = 0;
  if (!ParseBoundedInteger(raw, 0, 1, &value)) {
    return false;
  }
  *restricted = value == 1;
  return true;
}

ApplyResult ReleaseRestriction(const std::string& product_device,
                               const SysfsPaths& paths, SysfsReader read_file,
                               void* reader_context, SysfsWriter write_file,
                               void* writer_context) {
  if (!IsSupportedDevice(product_device) || !AreExpectedSysfsPaths(paths)) {
    return ApplyResult::kFailedClosed;
  }
  bool restricted = false;
  if (!ReadRestricted(read_file, reader_context, paths, &restricted)) {
    return ApplyResult::kFailedClosed;
  }
  if (!restricted) {
    return ApplyResult::kUnchanged;
  }
  if (!WriteRestriction(write_file, writer_context, paths, false)) {
    return ApplyResult::kFailedClosed;
  }
  return ApplyResult::kReleased;
}

ApplyResult ApplyOnceUnlessStopped(const std::string& product_device,
                                   const SysfsPaths& paths,
                                   const LimitSettings& settings,
                                   StopRequested stop_requested,
                                   void* stop_context, SysfsReader read_file,
                                   void* reader_context, SysfsWriter write_file,
                                   void* writer_context) {
  if (!IsSupportedDevice(product_device) || !AreExpectedSysfsPaths(paths)) {
    return ApplyResult::kFailedClosed;
  }
  if (stop_requested(stop_context)) {
    return ApplyResult::kStopped;
  }

  bool restricted = false;
  if (!ReadRestricted(read_file, reader_context, paths, &restricted)) {
    return ApplyResult::kFailedClosed;
  }

  std::string raw_capacity;
  int capacity_percent = 0;
  if (!ReadTrimmed(read_file, reader_context, paths.capacity, &raw_capacity) ||
      !ParseCapacity(raw_capacity, &capacity_percent)) {
    // An unreadable capacity must not leave charging held off.
    if (restricted &&
        !WriteRestriction(write_file, writer_context, paths, false)) {
      return ApplyResult::kFailedClosed;
    }
    return ApplyResult::kFailedClosed;
  }

  const PolicyDecision decision = Decide(settings, capacity_percent);
  if (!decision.valid || decision.action == ChargeAction::kInvalid) {
    if (restricted &&
        !WriteRestriction(write_file, writer_context, paths, false)) {
      return ApplyResult::kFailedClosed;
    }
    return ApplyResult::kFailedClosed;
  }

  const bool want_restricted = decision.action == ChargeAction::kRestrict;
  if (decision.action == ChargeAction::kHold || want_restricted == restricted) {
    return ApplyResult::kUnchanged;
  }
  if (stop_requested(stop_context)) {
    return ApplyResult::kStopped;
  }
  if (!WriteRestriction(write_file, writer_context, paths, want_restricted)) {
    return ApplyResult::kFailedClosed;
  }
  return want_restricted ? ApplyResult::kRestricted : ApplyResult::kReleased;
}

LoopResult RunLimitLoopUnlessStopped(
    const std::string& product_device, const SysfsPaths& paths,
    const LimitSettings& settings, StopRequested stop_requested,
    void* stop_context, SysfsReader read_file, void* reader_context,
    SysfsWriter write_file, void* writer_context,
    SleepForSeconds sleep_for_seconds, void* sleep_context) {
  while (true) {
    const ApplyResult result = ApplyOnceUnlessStopped(
        product_device, paths, settings, stop_requested, stop_context,
        read_file, reader_context, write_file, writer_context);
    if (result == ApplyResult::kFailedClosed) {
      return LoopResult::kFailedClosed;
    }
    if (result == ApplyResult::kStopped) {
      break;
    }
    if (!sleep_for_seconds(sleep_context, kPollIntervalSeconds)) {
      break;
    }
  }
  // Stopping must never leave the battery unable to charge.
  ReleaseRestriction(product_device, paths, read_file, reader_context,
                     write_file, writer_context);
  return LoopResult::kStopped;
}

}  // namespace ayn::charge
