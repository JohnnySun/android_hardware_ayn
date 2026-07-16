// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ayn/fan_lifecycle.h"

#include <mutex>
#include <optional>
#include <string>

namespace ayn::fan {

constexpr int kTachPollAttempts = 20;
constexpr int kTachPollIntervalMilliseconds = 25;

enum class FanResult {
  kOk,
  kUnsupportedDevice,
  kUnexpectedPaths,
  kInvalidMode,
  kInvalidOwner,
  kIoError,
  kTachTimeout,
  kDisableUnconfirmed,
  kNotOwner,
  kTemperatureUnavailable,
};

struct FanSnapshot {
  FanMode mode;
  int state;
  int duty;
  int tach;
};

struct FanResponse {
  FanResult result = FanResult::kIoError;
  FanMode requested_mode = FanMode::kOff;
  std::optional<FanSnapshot> snapshot;
};

struct FanDeviceIdentity {
  std::string product_device;
  std::string product_name;
  std::string vendor_model;
};

using SleepForMilliseconds = bool (*)(void* context, int milliseconds);

class FanService {
 public:
  FanService(FanDeviceIdentity identity, SysfsPaths paths,
             SysfsReader read_file, void* reader_context,
             SysfsWriter write_file, void* writer_context,
             TemperatureReader read_temperature, void* temperature_context,
             SleepForMilliseconds sleep_for_milliseconds,
             void* sleep_context);

  FanResponse GetStatus();
  FanResponse SetMode(FanMode mode);
  FanResponse InitializeSafeDefault();
  FanResponse Refresh();
  FanResponse ForceOff();

 private:
  struct RawSnapshot {
    int state;
    int duty;
    int period;
    int tach;
  };

  enum class PollResult {
    kMatched,
    kTimeout,
    kIoError,
  };

  FanResponse ApplyModeLocked(FanMode mode, int duty);
  FanResponse FailLocked(FanMode requested_mode, FanResult result);
  bool ApplyOffBestEffortLocked(FanSnapshot* snapshot);
  bool ReadTemperatureLocked(int* temperature_c);
  bool ReadCompleteSnapshotLocked(RawSnapshot* snapshot);
  bool ReadIntegerLocked(const std::string& path, int* value);
  bool WriteAndConfirmLocked(const std::string& path, int value);
  PollResult PollTachLocked(bool expect_positive, int* tach);
  FanResult GateResultLocked() const;

  const FanDeviceIdentity identity_;
  const SysfsPaths paths_;
  const SysfsReader read_file_;
  void* const reader_context_;
  const SysfsWriter write_file_;
  void* const writer_context_;
  const TemperatureReader read_temperature_;
  void* const temperature_context_;
  const SleepForMilliseconds sleep_for_milliseconds_;
  void* const sleep_context_;
  FanCurveController curve_controller_;
  FanMode active_mode_ = FanMode::kOff;
  std::optional<FanSnapshot> current_snapshot_;
  std::mutex mutex_;
};

}  // namespace ayn::fan
