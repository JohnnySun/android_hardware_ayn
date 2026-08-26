// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ayn/charge_lifecycle.h"
#include "ayn/charge_policy.h"

#include <mutex>
#include <string>

namespace ayn::charge {

enum class ServiceResult {
  kOk,
  kUnsupportedDevice,
  kUnexpectedPaths,
  kInvalidMode,
  kIoError,
  kCapacityUnavailable,
};

struct ChargeSnapshot {
  ChargeMode mode;
  int capacity_percent;
  bool restricted;
  int stop_percent;
  int resume_percent;
};

struct ServiceResponse {
  ServiceResult result = ServiceResult::kIoError;
  ChargeMode mode = ChargeMode::kOff;
  // Absent when the charger could not be read.
  bool snapshot_valid = false;
  ChargeSnapshot snapshot = {ChargeMode::kOff, -1, false, -1, -1};
};

// The chosen mode outlives the daemon, so a reboot or a crash does not quietly
// put the battery back on a policy the owner turned off.
using StateReader = bool (*)(void* context, std::string* value);
using StateWriter = bool (*)(void* context, const std::string& value);

std::string SerialiseMode(ChargeMode mode);
bool ParseMode(const std::string& raw, ChargeMode* mode);

// The stored state is the mode on its own line, optionally followed by a line
// holding the stop and resume percentages. A file written before thresholds
// were settable holds only the mode, and reads back with the defaults, which is
// why the format grew a second line instead of changing the first.
std::string SerialiseState(ChargeMode mode, int stop_percent,
                           int resume_percent);
bool ParseState(const std::string& raw, ChargeMode* mode, int* stop_percent,
                int* resume_percent);

class ChargeService {
 public:
  ChargeService(std::string product_device, SysfsPaths paths,
                SysfsReader read_file, void* reader_context,
                SysfsWriter write_file, void* writer_context,
                StateReader read_state, void* state_reader_context,
                StateWriter write_state, void* state_writer_context);

  // Reads the persisted mode, falling back to the default when there is none or
  // it cannot be trusted, and applies it once.
  ServiceResponse Start();
  ServiceResponse GetStatus();
  ServiceResponse SetMode(ChargeMode mode);
  // Refused whole if the pair does not satisfy AreValidSettings, so a bad
  // threshold can never be half applied.
  ServiceResponse SetThresholds(int stop_percent, int resume_percent);
  // One pass of the policy against the current capacity.
  ServiceResponse Refresh();
  // Releases the restriction whatever the mode says. For shutdown.
  ServiceResponse Release();

 private:
  ServiceResponse ApplyLocked();
  ServiceResponse SnapshotLocked(ServiceResult result);

  const std::string product_device_;
  const SysfsPaths paths_;
  const SysfsReader read_file_;
  void* const reader_context_;
  const SysfsWriter write_file_;
  void* const writer_context_;
  const StateReader read_state_;
  void* const state_reader_context_;
  const StateWriter write_state_;
  void* const state_writer_context_;

  ChargeMode mode_ = ChargeMode::kLimit;
  int stop_percent_ = kDefaultStopPercent;
  int resume_percent_ = kDefaultResumePercent;
  std::mutex mutex_;
};

}  // namespace ayn::charge
