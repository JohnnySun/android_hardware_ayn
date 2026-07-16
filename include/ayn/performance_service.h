// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace ayn::performance {

constexpr size_t kPerformanceNodeCount = 10;

enum class PerformanceMode {
  kSystemManaged = 0,
  kStockNormal = 1,
  kPerformance = 2,
  kHigh = 3,
};

enum class PerformanceResult {
  kOk = 0,
  kUnsupportedDevice = 1,
  kUnexpectedPaths = 2,
  kNotInitialized = 3,
  kAlreadyInitialized = 4,
  kInvalidMode = 5,
  kReadFailed = 6,
  kWriteFailed = 7,
  kReadbackFailed = 8,
  kRollbackFailed = 9,
  kModeUnavailable = 10,
};

struct ControlPolicy {
  static constexpr ControlPolicy ReadOnly() { return {false}; }
  static constexpr ControlPolicy StockNormalOnly() { return {true}; }

  bool stock_normal_write_enabled;
};

struct DeviceIdentity {
  std::string product_device;
  std::string product_name;
  std::string vendor_model;
};

struct SysfsPaths {
  std::array<std::string, kPerformanceNodeCount> nodes;
};

struct PerformanceResponse {
  PerformanceResult result = PerformanceResult::kNotInitialized;
  PerformanceMode requested_mode = PerformanceMode::kSystemManaged;
  PerformanceMode active_mode = PerformanceMode::kSystemManaged;
  bool initialized = false;
};

using SysfsReader = bool (*)(void* context, const std::string& path,
                             std::string* value);
using SysfsWriter = bool (*)(void* context, const std::string& path,
                             const std::string& value);

SysfsPaths StockSysfsPaths();

class PerformanceService {
 public:
  PerformanceService(DeviceIdentity identity, SysfsPaths paths,
                     SysfsReader read_file, void* reader_context,
                     SysfsWriter write_file, void* writer_context,
                     ControlPolicy policy = ControlPolicy::ReadOnly());

  PerformanceResponse Initialize();
  PerformanceResponse GetStatus();
  PerformanceResponse SetMode(PerformanceMode mode);
  PerformanceResponse BeginShutdown();

 private:
  using Snapshot = std::array<uint64_t, kPerformanceNodeCount>;

  PerformanceResponse ResponseLocked(PerformanceResult result,
                                     PerformanceMode requested_mode) const;
  PerformanceResult GateResultLocked() const;
  bool ReadCompleteSnapshotLocked(Snapshot* snapshot);
  bool ReadValueLocked(size_t index, uint64_t* value);
  bool WriteValueLocked(size_t index, uint64_t value);
  bool RollbackLocked(const Snapshot& snapshot,
                      const std::array<size_t, kPerformanceNodeCount>& touched,
                      size_t touched_count);
  PerformanceResponse RestoreBaselineFromLatchLocked();
  PerformanceResponse ApplySnapshotLocked(PerformanceMode mode,
                                          const Snapshot& target);
  Snapshot TargetForModeLocked(PerformanceMode mode) const;

  const DeviceIdentity identity_;
  const SysfsPaths paths_;
  const SysfsReader read_file_;
  void* const reader_context_;
  const SysfsWriter write_file_;
  void* const writer_context_;
  const ControlPolicy policy_;
  Snapshot baseline_{};
  PerformanceMode active_mode_ = PerformanceMode::kSystemManaged;
  bool initialized_ = false;
  bool rollback_failed_ = false;
  bool shutdown_started_ = false;
  std::mutex mutex_;
};

}  // namespace ayn::performance
