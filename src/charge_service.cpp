// SPDX-License-Identifier: Apache-2.0
#include "ayn/charge_service.h"

#include <utility>

namespace ayn::charge {
namespace {

LimitSettings SettingsFor(ChargeMode mode) {
  return {mode, kDefaultStopPercent, kDefaultResumePercent};
}

}  // namespace

std::string SerialiseMode(ChargeMode mode) {
  switch (mode) {
    case ChargeMode::kOff:
      return "off";
    case ChargeMode::kLimit:
      return "limit";
    case ChargeMode::kBypass:
      return "bypass";
  }
  return "limit";
}

bool ParseMode(const std::string& raw, ChargeMode* mode) {
  std::string trimmed;
  for (const char character : raw) {
    if (character != '\n' && character != '\r' && character != ' ' &&
        character != '\t' && character != '\0') {
      trimmed.push_back(character);
    }
  }
  if (trimmed == "off") {
    *mode = ChargeMode::kOff;
    return true;
  }
  if (trimmed == "limit") {
    *mode = ChargeMode::kLimit;
    return true;
  }
  if (trimmed == "bypass") {
    *mode = ChargeMode::kBypass;
    return true;
  }
  return false;
}

ChargeService::ChargeService(std::string product_device, SysfsPaths paths,
                             SysfsReader read_file, void* reader_context,
                             SysfsWriter write_file, void* writer_context,
                             StateReader read_state, void* state_reader_context,
                             StateWriter write_state,
                             void* state_writer_context)
    : product_device_(std::move(product_device)),
      paths_(std::move(paths)),
      read_file_(read_file),
      reader_context_(reader_context),
      write_file_(write_file),
      writer_context_(writer_context),
      read_state_(read_state),
      state_reader_context_(state_reader_context),
      write_state_(write_state),
      state_writer_context_(state_writer_context) {}

ServiceResponse ChargeService::SnapshotLocked(ServiceResult result) {
  ServiceResponse response;
  response.result = result;
  response.mode = mode_;

  std::string raw_capacity;
  int capacity = -1;
  const bool capacity_ok =
      read_file_(reader_context_, paths_.capacity, &raw_capacity) &&
      ParseCapacity(raw_capacity, &capacity);

  std::string raw_restricted;
  bool restricted = false;
  const bool restricted_ok =
      read_file_(reader_context_, paths_.restrict_chg, &raw_restricted) &&
      ParseRestrictFlag(raw_restricted, &restricted);

  if (capacity_ok && restricted_ok) {
    response.snapshot_valid = true;
    response.snapshot = {mode_, capacity, restricted, kDefaultStopPercent,
                         kDefaultResumePercent};
  }
  return response;
}

ServiceResponse ChargeService::ApplyLocked() {
  if (!IsSupportedDevice(product_device_)) {
    return SnapshotLocked(ServiceResult::kUnsupportedDevice);
  }
  if (!AreExpectedSysfsPaths(paths_)) {
    return SnapshotLocked(ServiceResult::kUnexpectedPaths);
  }

  auto stop_never = [](void*) { return false; };
  const ApplyResult applied = ApplyOnceUnlessStopped(
      product_device_, paths_, SettingsFor(mode_), stop_never, nullptr,
      read_file_, reader_context_, write_file_, writer_context_);

  switch (applied) {
    case ApplyResult::kRestricted:
    case ApplyResult::kReleased:
    case ApplyResult::kUnchanged:
      return SnapshotLocked(ServiceResult::kOk);
    case ApplyResult::kStopped:
    case ApplyResult::kFailedClosed:
      break;
  }
  return SnapshotLocked(ServiceResult::kIoError);
}

ServiceResponse ChargeService::Start() {
  std::lock_guard<std::mutex> guard(mutex_);
  std::string stored;
  ChargeMode parsed = ChargeMode::kLimit;
  if (read_state_ != nullptr &&
      read_state_(state_reader_context_, &stored) &&
      ParseMode(stored, &parsed)) {
    mode_ = parsed;
  }
  return ApplyLocked();
}

ServiceResponse ChargeService::GetStatus() {
  std::lock_guard<std::mutex> guard(mutex_);
  return SnapshotLocked(ServiceResult::kOk);
}

ServiceResponse ChargeService::SetMode(ChargeMode mode) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!IsKnownMode(static_cast<int>(mode))) {
    return SnapshotLocked(ServiceResult::kInvalidMode);
  }
  const ChargeMode previous = mode_;
  mode_ = mode;
  const ServiceResponse response = ApplyLocked();
  if (response.result != ServiceResult::kOk) {
    mode_ = previous;
    return response;
  }
  // Persisting after the charger agreed keeps a stored mode the hardware
  // refused from coming back on the next boot.
  if (write_state_ != nullptr) {
    write_state_(state_writer_context_, SerialiseMode(mode_));
  }
  return response;
}

ServiceResponse ChargeService::Refresh() {
  std::lock_guard<std::mutex> guard(mutex_);
  return ApplyLocked();
}

ServiceResponse ChargeService::Release() {
  std::lock_guard<std::mutex> guard(mutex_);
  ReleaseRestriction(product_device_, paths_, read_file_, reader_context_,
                     write_file_, writer_context_);
  return SnapshotLocked(ServiceResult::kOk);
}

}  // namespace ayn::charge
