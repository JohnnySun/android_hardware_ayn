// SPDX-License-Identifier: Apache-2.0

#include "ayn/performance_service.h"

#include <charconv>
#include <system_error>
#include <utility>

namespace ayn::performance {

namespace {

constexpr size_t kMaximumSysfsValueBytes = 64;

constexpr std::array<uint64_t, kPerformanceNodeCount> kStockNormalValues = {
    902400, 2016000, 1651200, 2803200, 1843200,
    3187200, 401000000, 680000000, 4224000, 4224000,
};

constexpr std::array<uint64_t, kPerformanceNodeCount> kPerformanceValues = {
    1228800, 2016000, 2054400, 2803200, 2476800,
    3187200, 680000000, 680000000, 4224000, 4224000,
};

constexpr std::array<uint64_t, kPerformanceNodeCount> kHighValues = {
    2016000, 2016000, 2803200, 2803200, 3187200,
    3187200, 680000000, 680000000, 4224000, 4224000,
};

bool IsSupportedIdentity(const DeviceIdentity& identity) {
  return identity.product_device == "odin2_mini" &&
         identity.product_name == "lineage_odin2_mini" &&
         identity.vendor_model == "Odin2 Mini";
}

bool IsSupportedMode(PerformanceMode mode) {
  return mode == PerformanceMode::kSystemManaged ||
         mode == PerformanceMode::kStockNormal ||
         mode == PerformanceMode::kPerformance ||
         mode == PerformanceMode::kHigh;
}

bool ParseUnsignedInteger(const std::string& raw, uint64_t* value) {
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
  uint64_t parsed = 0;
  const char* const begin = raw.data();
  const char* const end = begin + size;
  const auto converted = std::from_chars(begin, end, parsed);
  if (converted.ec != std::errc() || converted.ptr != end) {
    return false;
  }
  *value = parsed;
  return true;
}

}  // namespace

SysfsPaths StockSysfsPaths() {
  return {{
      "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq",
      "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
      "/sys/devices/system/cpu/cpu3/cpufreq/scaling_min_freq",
      "/sys/devices/system/cpu/cpu3/cpufreq/scaling_max_freq",
      "/sys/devices/system/cpu/cpu7/cpufreq/scaling_min_freq",
      "/sys/devices/system/cpu/cpu7/cpufreq/scaling_max_freq",
      "/sys/class/kgsl/kgsl-3d0/devfreq/min_freq",
      "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq",
      "/sys/devices/system/cpu/bus_dcvs/DDR/hw_min_freq",
      "/sys/devices/system/cpu/bus_dcvs/DDR/soc:qcom,memlat:ddr:gold/min_freq",
  }};
}

PerformanceService::PerformanceService(DeviceIdentity identity,
                                       SysfsPaths paths,
                                       SysfsReader read_file,
                                       void* reader_context,
                                       SysfsWriter write_file,
                                       void* writer_context,
                                       ControlPolicy policy)
    : identity_(std::move(identity)),
      paths_(std::move(paths)),
      read_file_(read_file),
      reader_context_(reader_context),
      write_file_(write_file),
      writer_context_(writer_context),
      policy_(policy) {}

PerformanceResponse PerformanceService::ResponseLocked(
    PerformanceResult result, PerformanceMode requested_mode) const {
  return {result, requested_mode, active_mode_, initialized_};
}

PerformanceResult PerformanceService::GateResultLocked() const {
  if (!IsSupportedIdentity(identity_)) {
    return PerformanceResult::kUnsupportedDevice;
  }
  if (paths_.nodes != StockSysfsPaths().nodes) {
    return PerformanceResult::kUnexpectedPaths;
  }
  if (read_file_ == nullptr) {
    return PerformanceResult::kReadFailed;
  }
  return PerformanceResult::kOk;
}

bool PerformanceService::ReadValueLocked(size_t index, uint64_t* value) {
  if (index >= kPerformanceNodeCount) {
    return false;
  }
  std::string raw;
  return read_file_(reader_context_, paths_.nodes[index], &raw) &&
         ParseUnsignedInteger(raw, value);
}

bool PerformanceService::ReadCompleteSnapshotLocked(Snapshot* snapshot) {
  if (snapshot == nullptr) {
    return false;
  }
  Snapshot candidate{};
  for (size_t index = 0; index < kPerformanceNodeCount; ++index) {
    if (!ReadValueLocked(index, &candidate[index])) {
      return false;
    }
  }
  *snapshot = candidate;
  return true;
}

bool PerformanceService::WriteValueLocked(size_t index, uint64_t value) {
  if (write_file_ == nullptr || index >= kPerformanceNodeCount ||
      !write_file_(writer_context_, paths_.nodes[index],
                   std::to_string(value))) {
    return false;
  }
  uint64_t observed = 0;
  return ReadValueLocked(index, &observed) && observed == value;
}

bool PerformanceService::RollbackLocked(
    const Snapshot& snapshot,
    const std::array<size_t, kPerformanceNodeCount>& touched,
    size_t touched_count) {
  bool restored = true;
  while (touched_count > 0) {
    const size_t index = touched[--touched_count];
    restored = WriteValueLocked(index, snapshot[index]) && restored;
  }
  return restored;
}

PerformanceResponse PerformanceService::RestoreBaselineFromLatchLocked() {
  bool restored = true;
  for (size_t index = 0; index < kPerformanceNodeCount; ++index) {
    if (!WriteValueLocked(index, baseline_[index])) {
      restored = false;
    }
  }
  if (!restored) {
    return ResponseLocked(PerformanceResult::kRollbackFailed,
                          PerformanceMode::kSystemManaged);
  }
  rollback_failed_ = false;
  active_mode_ = PerformanceMode::kSystemManaged;
  return ResponseLocked(PerformanceResult::kOk,
                        PerformanceMode::kSystemManaged);
}

PerformanceService::Snapshot PerformanceService::TargetForModeLocked(
    PerformanceMode mode) const {
  switch (mode) {
    case PerformanceMode::kSystemManaged:
      return baseline_;
    case PerformanceMode::kStockNormal:
      return kStockNormalValues;
    case PerformanceMode::kPerformance:
      return kPerformanceValues;
    case PerformanceMode::kHigh:
      return kHighValues;
  }
  return baseline_;
}

PerformanceResponse PerformanceService::Initialize() {
  std::lock_guard<std::mutex> lock(mutex_);
  const PerformanceResult gate = GateResultLocked();
  if (gate != PerformanceResult::kOk) {
    return ResponseLocked(gate, PerformanceMode::kSystemManaged);
  }
  if (initialized_) {
    return ResponseLocked(PerformanceResult::kAlreadyInitialized,
                          active_mode_);
  }
  if (!ReadCompleteSnapshotLocked(&baseline_)) {
    return ResponseLocked(PerformanceResult::kReadFailed,
                          PerformanceMode::kSystemManaged);
  }
  active_mode_ = PerformanceMode::kSystemManaged;
  initialized_ = true;
  return ResponseLocked(PerformanceResult::kOk,
                        PerformanceMode::kSystemManaged);
}

PerformanceResponse PerformanceService::GetStatus() {
  std::lock_guard<std::mutex> lock(mutex_);
  const PerformanceResult result =
      !initialized_ ? PerformanceResult::kNotInitialized
                    : rollback_failed_ ? PerformanceResult::kRollbackFailed
                                       : PerformanceResult::kOk;
  return ResponseLocked(result,
                        active_mode_);
}

PerformanceResponse PerformanceService::ApplySnapshotLocked(
    PerformanceMode mode, const Snapshot& target) {
  Snapshot before{};
  if (!ReadCompleteSnapshotLocked(&before)) {
    return ResponseLocked(PerformanceResult::kReadFailed, mode);
  }
  std::array<size_t, kPerformanceNodeCount> touched{};
  size_t touched_count = 0;

  for (size_t index = 0; index < kPerformanceNodeCount; ++index) {
    touched[touched_count++] = index;
    if (!write_file_(writer_context_, paths_.nodes[index],
                     std::to_string(target[index]))) {
      if (!RollbackLocked(before, touched, touched_count)) {
        rollback_failed_ = true;
        return ResponseLocked(PerformanceResult::kRollbackFailed, mode);
      }
      return ResponseLocked(PerformanceResult::kWriteFailed, mode);
    }
    uint64_t observed = 0;
    if (!ReadValueLocked(index, &observed) || observed != target[index]) {
      if (!RollbackLocked(before, touched, touched_count)) {
        rollback_failed_ = true;
        return ResponseLocked(PerformanceResult::kRollbackFailed, mode);
      }
      return ResponseLocked(PerformanceResult::kReadbackFailed, mode);
    }
  }

  active_mode_ = mode;
  return ResponseLocked(PerformanceResult::kOk, mode);
}

PerformanceResponse PerformanceService::SetMode(PerformanceMode mode) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return ResponseLocked(PerformanceResult::kNotInitialized, mode);
  }
  if (shutdown_started_) {
    return ResponseLocked(PerformanceResult::kModeUnavailable, mode);
  }
  if (rollback_failed_) {
    if (mode == PerformanceMode::kSystemManaged) {
      return RestoreBaselineFromLatchLocked();
    }
    return ResponseLocked(PerformanceResult::kRollbackFailed, mode);
  }
  if (!IsSupportedMode(mode)) {
    return ResponseLocked(PerformanceResult::kInvalidMode, mode);
  }
  const bool write_enabled =
      mode == PerformanceMode::kSystemManaged ||
      (mode == PerformanceMode::kStockNormal &&
       policy_.stock_normal_write_enabled) ||
      (mode == PerformanceMode::kPerformance &&
       policy_.performance_write_enabled) ||
      (mode == PerformanceMode::kHigh && policy_.high_write_enabled);
  if (!write_enabled ||
      (mode != PerformanceMode::kSystemManaged && write_file_ == nullptr)) {
    return ResponseLocked(PerformanceResult::kModeUnavailable, mode);
  }
  if (mode == active_mode_) {
    return ResponseLocked(PerformanceResult::kOk, mode);
  }
  return ApplySnapshotLocked(mode, TargetForModeLocked(mode));
}

PerformanceResponse PerformanceService::BeginShutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_) {
    return ResponseLocked(PerformanceResult::kNotInitialized,
                          PerformanceMode::kSystemManaged);
  }
  shutdown_started_ = true;
  if (rollback_failed_) {
    return RestoreBaselineFromLatchLocked();
  }
  if (active_mode_ == PerformanceMode::kSystemManaged) {
    return ResponseLocked(PerformanceResult::kOk,
                          PerformanceMode::kSystemManaged);
  }
  return ApplySnapshotLocked(PerformanceMode::kSystemManaged, baseline_);
}

}  // namespace ayn::performance
