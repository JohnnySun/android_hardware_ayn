// SPDX-License-Identifier: Apache-2.0
#include "ayn/lights_lifecycle.h"

#include <cstdlib>
#include <string>

namespace ayn::lights {
namespace {

bool ReadChannel(SysfsReader read_file, void* reader_context,
                 const std::string& path, int* value) {
  std::string raw;
  if (!read_file(reader_context, path, &raw)) {
    return false;
  }
  return ParseChannel(raw, value);
}

bool WriteChannel(SysfsWriter write_file, void* writer_context,
                  const std::string& path, int value) {
  return write_file(writer_context, path, std::to_string(value) + "\n");
}

}  // namespace

bool ParseChannel(const std::string& raw, int* value) {
  if (raw.empty()) {
    return false;
  }
  std::size_t index = 0;
  while (index < raw.size() && (raw[index] == ' ' || raw[index] == '\t')) {
    ++index;
  }
  if (index >= raw.size()) {
    return false;
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
  const long parsed = std::strtol(raw.substr(index, digits - index).c_str(),
                                  nullptr, 10);
  if (parsed < 0 || parsed > kChannelMaximum) {
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

ClampResult ClampOnceUnlessStopped(const std::string& product_device,
                                   const SysfsPaths& paths, int cap,
                                   StopRequested stop_requested,
                                   void* stop_context, SysfsReader read_file,
                                   void* reader_context, SysfsWriter write_file,
                                   void* writer_context) {
  if (!IsSupportedDevice(product_device) || !AreExpectedSysfsPaths(paths) ||
      !IsValidCap(cap)) {
    return ClampResult::kFailedClosed;
  }
  if (stop_requested(stop_context)) {
    return ClampResult::kStopped;
  }

  Channels observed = {0, 0, 0};
  if (!ReadChannel(read_file, reader_context, paths.red_brightness,
                   &observed.red) ||
      !ReadChannel(read_file, reader_context, paths.green_brightness,
                   &observed.green) ||
      !ReadChannel(read_file, reader_context, paths.blue_brightness,
                   &observed.blue)) {
    return ClampResult::kFailedClosed;
  }

  const ClampDecision decision = Clamp(observed, cap);
  if (!decision.valid) {
    return ClampResult::kFailedClosed;
  }
  if (!decision.write_required) {
    return ClampResult::kAlreadyWithinCap;
  }
  if (stop_requested(stop_context)) {
    return ClampResult::kStopped;
  }

  // Only the channels that actually change are written, so a partially dim LED
  // does not get three writes when it needs one.
  if (decision.channels.red != observed.red &&
      !WriteChannel(write_file, writer_context, paths.red_brightness,
                    decision.channels.red)) {
    return ClampResult::kFailedClosed;
  }
  if (decision.channels.green != observed.green &&
      !WriteChannel(write_file, writer_context, paths.green_brightness,
                    decision.channels.green)) {
    return ClampResult::kFailedClosed;
  }
  if (decision.channels.blue != observed.blue &&
      !WriteChannel(write_file, writer_context, paths.blue_brightness,
                    decision.channels.blue)) {
    return ClampResult::kFailedClosed;
  }
  return ClampResult::kClamped;
}

LoopResult RunClampLoopUnlessStopped(
    const std::string& product_device, const SysfsPaths& paths, int cap,
    StopRequested stop_requested, void* stop_context, SysfsReader read_file,
    void* reader_context, SysfsWriter write_file, void* writer_context,
    SleepForMilliseconds sleep_for_milliseconds, void* sleep_context) {
  while (true) {
    const ClampResult result = ClampOnceUnlessStopped(
        product_device, paths, cap, stop_requested, stop_context, read_file,
        reader_context, write_file, writer_context);
    if (result == ClampResult::kFailedClosed) {
      return LoopResult::kFailedClosed;
    }
    if (result == ClampResult::kStopped) {
      return LoopResult::kStopped;
    }
    if (!sleep_for_milliseconds(sleep_context, kPollIntervalMilliseconds)) {
      return LoopResult::kStopped;
    }
  }
}

}  // namespace ayn::lights
