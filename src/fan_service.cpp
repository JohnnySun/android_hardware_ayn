// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_service.h"

#include <charconv>
#include <cstddef>
#include <system_error>
#include <utility>

namespace ayn::fan {

namespace {

constexpr size_t kMaximumSysfsValueBytes = 64;

bool IsAidlMode(FanMode mode) {
  return mode == FanMode::kOff || mode == FanMode::kQuiet ||
         mode == FanMode::kSport;
}

bool IsAutomaticMode(FanMode mode) {
  return mode == FanMode::kQuiet || mode == FanMode::kSport;
}

bool IsSupportedIdentity(const FanDeviceIdentity& identity) {
  return identity.product_device == "odin2_mini" &&
         identity.product_name == "lineage_odin2_mini" &&
         identity.vendor_model == "Odin2 Mini";
}

bool ParseNonNegativeInteger(const std::string& raw, int* value) {
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
  for (size_t index = 0; index < size; ++index) {
    if (raw[index] < '0' || raw[index] > '9') {
      return false;
    }
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

}  // namespace

FanService::FanService(FanDeviceIdentity identity, SysfsPaths paths,
                       SysfsReader read_file, void* reader_context,
                       SysfsWriter write_file, void* writer_context,
                       TemperatureReader read_temperature,
                       void* temperature_context,
                       SleepForMilliseconds sleep_for_milliseconds,
                       void* sleep_context)
    : identity_(std::move(identity)),
      paths_(std::move(paths)),
      read_file_(read_file),
      reader_context_(reader_context),
      write_file_(write_file),
      writer_context_(writer_context),
      read_temperature_(read_temperature),
      temperature_context_(temperature_context),
      sleep_for_milliseconds_(sleep_for_milliseconds),
      sleep_context_(sleep_context) {}

FanResult FanService::GateResultLocked() const {
  if (!IsSupportedIdentity(identity_)) {
    return FanResult::kUnsupportedDevice;
  }
  if (!AreExpectedSysfsPaths(paths_)) {
    return FanResult::kUnexpectedPaths;
  }
  if (read_file_ == nullptr || write_file_ == nullptr ||
      sleep_for_milliseconds_ == nullptr) {
    return FanResult::kIoError;
  }
  return FanResult::kOk;
}

bool FanService::ReadTemperatureLocked(int* temperature_c) {
  return read_temperature_ != nullptr && temperature_c != nullptr &&
         read_temperature_(temperature_context_, temperature_c) &&
         *temperature_c >= kMinimumTemperatureC &&
         *temperature_c <= kMaximumTemperatureC;
}

bool FanService::ReadIntegerLocked(const std::string& path, int* value) {
  std::string raw;
  return read_file_(reader_context_, path, &raw) &&
         ParseNonNegativeInteger(raw, value);
}

bool FanService::ReadCompleteSnapshotLocked(RawSnapshot* snapshot) {
  if (snapshot == nullptr ||
      !ReadIntegerLocked(paths_.state, &snapshot->state) ||
      !ReadIntegerLocked(paths_.duty, &snapshot->duty) ||
      !ReadIntegerLocked(paths_.period, &snapshot->period) ||
      !ReadIntegerLocked(paths_.speed, &snapshot->tach)) {
    return false;
  }
  return snapshot->state == 0 || snapshot->state == 1;
}

bool FanService::WriteAndConfirmLocked(const std::string& path, int value) {
  if (!write_file_(writer_context_, path, std::to_string(value))) {
    return false;
  }
  int observed = -1;
  return ReadIntegerLocked(path, &observed) && observed == value;
}

FanService::PollResult FanService::PollTachLocked(bool expect_positive,
                                                  int* tach) {
  for (int attempt = 0; attempt < kTachPollAttempts; ++attempt) {
    int observed = -1;
    if (!ReadIntegerLocked(paths_.speed, &observed)) {
      return PollResult::kIoError;
    }
    if ((expect_positive && observed > 0) ||
        (!expect_positive && observed == 0)) {
      if (tach != nullptr) {
        *tach = observed;
      }
      return PollResult::kMatched;
    }
    if (attempt + 1 < kTachPollAttempts &&
        !sleep_for_milliseconds_(sleep_context_,
                                 kTachPollIntervalMilliseconds)) {
      return PollResult::kIoError;
    }
  }
  return PollResult::kTimeout;
}

bool FanService::ApplyOffBestEffortLocked(FanSnapshot* snapshot) {
  curve_controller_.Reset();
  active_mode_ = FanMode::kOff;
  current_snapshot_.reset();
  bool confirmed = true;
  confirmed = WriteAndConfirmLocked(paths_.state, 0) && confirmed;
  int period = -1;
  confirmed = ReadIntegerLocked(paths_.period, &period) &&
              period == kPwmPeriod && confirmed;
  confirmed = WriteAndConfirmLocked(paths_.duty, kOffDuty) && confirmed;
  int tach = -1;
  confirmed = PollTachLocked(false, &tach) == PollResult::kMatched && confirmed;
  if (confirmed && snapshot != nullptr) {
    *snapshot = {FanMode::kOff, 0, kOffDuty, tach};
  }
  if (confirmed) {
    current_snapshot_ = FanSnapshot{FanMode::kOff, 0, kOffDuty, tach};
  }
  return confirmed;
}

FanResponse FanService::FailLocked(FanMode requested_mode, FanResult result) {
  return ApplyOffBestEffortLocked(nullptr)
             ? FanResponse{result, requested_mode, std::nullopt}
             : FanResponse{FanResult::kDisableUnconfirmed, requested_mode,
                           std::nullopt};
}

FanResponse FanService::ApplyModeLocked(FanMode mode, int duty) {
  if ((mode == FanMode::kOff && duty != kOffDuty) ||
      (IsAutomaticMode(mode) &&
       (duty < 0 || duty > kSafeMaximumDutyNs)) ||
      (!IsAutomaticMode(mode) && mode != FanMode::kOff)) {
    return FailLocked(mode, FanResult::kInvalidMode);
  }
  RawSnapshot initial{};
  if (!ReadCompleteSnapshotLocked(&initial)) {
    return FailLocked(mode, FanResult::kIoError);
  }

  int period = -1;
  if (!WriteAndConfirmLocked(paths_.state, 0) ||
      !ReadIntegerLocked(paths_.period, &period) || period != kPwmPeriod ||
      !WriteAndConfirmLocked(paths_.duty, duty)) {
    return FailLocked(mode, FanResult::kIoError);
  }
  if (mode != FanMode::kOff &&
      !WriteAndConfirmLocked(paths_.state, 1)) {
    return FailLocked(mode, FanResult::kIoError);
  }

  int tach = -1;
  const PollResult poll = PollTachLocked(mode != FanMode::kOff, &tach);
  if (poll != PollResult::kMatched) {
    return FailLocked(mode, poll == PollResult::kTimeout
                                ? FanResult::kTachTimeout
                                : FanResult::kIoError);
  }
  const int state = mode == FanMode::kOff ? 0 : 1;
  active_mode_ = mode;
  current_snapshot_ = FanSnapshot{mode, state, duty, tach};
  return {FanResult::kOk, mode, current_snapshot_};
}

FanResponse FanService::SetMode(FanMode mode) {
  std::lock_guard<std::mutex> lock(mutex_);
  const FanResult gate = GateResultLocked();
  if (gate != FanResult::kOk) {
    return {gate, mode, std::nullopt};
  }
  if (!IsAidlMode(mode)) {
    return FailLocked(mode, FanResult::kInvalidMode);
  }
  FanResponse response;
  if (mode == FanMode::kOff) {
    curve_controller_.Reset();
    response = ApplyModeLocked(mode, kOffDuty);
  } else {
    int temperature_c = 0;
    curve_controller_.Reset();
    if (!ReadTemperatureLocked(&temperature_c)) {
      return FailLocked(mode, FanResult::kTemperatureUnavailable);
    }
    const CurveDecision decision =
        curve_controller_.Observe(mode, temperature_c);
    if (!decision.valid || !decision.apply) {
      return FailLocked(mode, FanResult::kTemperatureUnavailable);
    }
    response = ApplyModeLocked(mode, decision.duty_ns);
  }
  return response;
}

FanResponse FanService::InitializeSafeDefault() {
  const FanResponse baseline = ForceOff();
  if (baseline.result != FanResult::kOk) {
    return baseline;
  }
  return SetMode(FanMode::kQuiet);
}

FanResponse FanService::Refresh() {
  std::lock_guard<std::mutex> lock(mutex_);
  const FanResult gate = GateResultLocked();
  if (gate != FanResult::kOk) {
    return {gate, active_mode_, std::nullopt};
  }
  if (!IsAutomaticMode(active_mode_)) {
    return {FanResult::kOk, active_mode_, current_snapshot_};
  }
  if (!current_snapshot_.has_value()) {
    return FailLocked(active_mode_, FanResult::kIoError);
  }

  const FanMode mode = active_mode_;
  int temperature_c = 0;
  if (!ReadTemperatureLocked(&temperature_c)) {
    return FailLocked(mode, FanResult::kTemperatureUnavailable);
  }
  const CurveDecision decision =
      curve_controller_.Observe(mode, temperature_c);
  if (!decision.valid) {
    return FailLocked(mode, FanResult::kTemperatureUnavailable);
  }
  if (!decision.apply) {
    RawSnapshot raw{};
    if (!ReadCompleteSnapshotLocked(&raw) || raw.state != 1 ||
        raw.period != kPwmPeriod || raw.duty != current_snapshot_->duty) {
      return FailLocked(mode, FanResult::kIoError);
    }
    int tach = raw.tach;
    if (tach <= 0) {
      const PollResult poll = PollTachLocked(true, &tach);
      if (poll != PollResult::kMatched) {
        return FailLocked(mode, poll == PollResult::kTimeout
                                    ? FanResult::kTachTimeout
                                    : FanResult::kIoError);
      }
    }
    current_snapshot_ = FanSnapshot{mode, 1, raw.duty, tach};
    return {FanResult::kOk, mode, current_snapshot_};
  }
  return ApplyModeLocked(mode, decision.duty_ns);
}

FanResponse FanService::GetStatus() {
  std::lock_guard<std::mutex> lock(mutex_);
  const FanResult gate = GateResultLocked();
  if (gate != FanResult::kOk) {
    return {gate, FanMode::kOff, std::nullopt};
  }

  RawSnapshot raw{};
  if (!ReadCompleteSnapshotLocked(&raw) || raw.period != kPwmPeriod) {
    return FailLocked(FanMode::kOff, FanResult::kIoError);
  }
  FanMode mode = FanMode::kOff;
  if (raw.state == 0 && raw.duty == kOffDuty) {
    mode = FanMode::kOff;
    curve_controller_.Reset();
    active_mode_ = FanMode::kOff;
  } else if (raw.state == 1 && IsAutomaticMode(active_mode_) &&
             current_snapshot_.has_value() &&
             raw.duty == current_snapshot_->duty) {
    mode = active_mode_;
  } else {
    return FailLocked(FanMode::kOff, FanResult::kIoError);
  }

  int tach = raw.tach;
  if ((mode == FanMode::kOff && tach != 0) ||
      (mode != FanMode::kOff && tach <= 0)) {
    const PollResult poll = PollTachLocked(mode != FanMode::kOff, &tach);
    if (poll != PollResult::kMatched) {
      return FailLocked(mode, poll == PollResult::kTimeout
                                  ? FanResult::kTachTimeout
                                  : FanResult::kIoError);
    }
  }
  current_snapshot_ = FanSnapshot{mode, raw.state, raw.duty, tach};
  return {FanResult::kOk, mode, current_snapshot_};
}

FanResponse FanService::ForceOff() {
  std::lock_guard<std::mutex> lock(mutex_);
  const FanResult gate = GateResultLocked();
  if (gate != FanResult::kOk) {
    return {gate, FanMode::kOff, std::nullopt};
  }
  FanSnapshot snapshot{};
  return ApplyOffBestEffortLocked(&snapshot)
             ? FanResponse{FanResult::kOk, FanMode::kOff, snapshot}
             : FanResponse{FanResult::kDisableUnconfirmed, FanMode::kOff,
                           std::nullopt};
}

}  // namespace ayn::fan
