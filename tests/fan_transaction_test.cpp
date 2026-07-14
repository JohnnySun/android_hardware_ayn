// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_service.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}

using ayn::fan::FanMode;
using ayn::fan::FanDeviceIdentity;
using ayn::fan::FanResponse;
using ayn::fan::FanResult;
using ayn::fan::FanService;
using ayn::fan::SysfsPaths;

const SysfsPaths kExpectedPaths = {
    "/sys/class/gpio5_pwm2/state",
    "/sys/class/gpio5_pwm2/duty",
    "/sys/class/gpio5_pwm2/period",
    "/sys/class/gpio5_pwm2/speed",
};

struct Harness {
  std::map<std::string, std::string> files = {
      {kExpectedPaths.state, "0\n"},
      {kExpectedPaths.duty, "10000\n"},
      {kExpectedPaths.period, "50000\n"},
      {kExpectedPaths.speed, "0\n"},
  };
  std::vector<std::string> events;
  std::vector<std::string> tach_values;
  size_t tach_index = 0;
  size_t read_count = 0;
  size_t write_count = 0;
  size_t fail_read_at = 0;
  size_t fail_write_at = 0;
  bool fail_all_writes = false;
  bool tach_follows_state = true;
  std::string enabled_tach = "1000\n";
  bool block_first_write = false;
  bool first_write_entered = false;
  bool release_first_write = false;
  std::mutex block_mutex;
  std::condition_variable block_cv;
};

bool ReadFile(void* context, const std::string& path, std::string* value) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->read_count;
  harness->events.push_back("R " + path);
  if (harness->fail_read_at == harness->read_count) {
    return false;
  }
  if (path == kExpectedPaths.speed &&
      harness->tach_index < harness->tach_values.size()) {
    *value = harness->tach_values[harness->tach_index++];
    return true;
  }
  const auto found = harness->files.find(path);
  if (found == harness->files.end()) {
    return false;
  }
  *value = found->second;
  return true;
}

bool WriteFile(void* context, const std::string& path,
               const std::string& value) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->write_count;
  harness->events.push_back("W " + path + "=" + value);
  if (harness->block_first_write && harness->write_count == 1) {
    std::unique_lock<std::mutex> lock(harness->block_mutex);
    harness->first_write_entered = true;
    harness->block_cv.notify_all();
    harness->block_cv.wait(lock,
                           [&] { return harness->release_first_write; });
  }
  if (harness->fail_all_writes ||
      harness->fail_write_at == harness->write_count) {
    return false;
  }
  harness->files[path] = value + "\n";
  if (harness->tach_follows_state && path == kExpectedPaths.state) {
    harness->files[kExpectedPaths.speed] =
        value == "0" ? "0\n" : harness->enabled_tach;
  }
  return true;
}

bool SleepForMilliseconds(void*, int milliseconds) {
  return milliseconds == ayn::fan::kTachPollIntervalMilliseconds;
}

FanService Service(Harness* harness,
                   const FanDeviceIdentity& identity = {
                       "odin2_mini", "lineage_odin2_mini", "Odin2 Mini"},
                   const SysfsPaths& paths = kExpectedPaths) {
  return FanService(identity, paths, ReadFile, harness, WriteFile,
                    harness, SleepForMilliseconds, harness);
}

void CheckNoSnapshot(const FanResponse& response) {
  CHECK(!response.snapshot.has_value());
}

void ExactIdentityAndPathsGateAllIo() {
  for (size_t index = 0; index < 3; ++index) {
    Harness wrong_identity;
    FanDeviceIdentity identity = {
        "odin2_mini", "lineage_odin2_mini", "Odin2 Mini"};
    std::string* fields[] = {&identity.product_device, &identity.product_name,
                             &identity.vendor_model};
    *fields[index] += ".wrong";
    FanService identity_service = Service(&wrong_identity, identity);
    const FanResponse response =
        identity_service.SetMode(FanMode::kQuiet, 1, true);
    CHECK(response.result == FanResult::kUnsupportedDevice);
    CheckNoSnapshot(response);
    CHECK(wrong_identity.events.empty());
  }

  for (size_t index = 0; index < 4; ++index) {
    Harness wrong_path;
    SysfsPaths paths = kExpectedPaths;
    std::string* fields[] = {&paths.state, &paths.duty, &paths.period,
                             &paths.speed};
    *fields[index] += ".bak";
    FanService path_service = Service(
        &wrong_path, {"odin2_mini", "lineage_odin2_mini", "Odin2 Mini"},
        paths);
    const FanResponse response =
        path_service.SetMode(FanMode::kQuiet, 1, true);
    CHECK(response.result == FanResult::kUnexpectedPaths);
    CheckNoSnapshot(response);
    CHECK(wrong_path.events.empty());
  }
}

void InvalidModeAndOwnerFailClosed() {
  for (const auto& request : {
           std::pair<int, bool>{static_cast<int>(FanMode::kQuiet), false},
           std::pair<int, bool>{99, true},
       }) {
    Harness harness;
    harness.files[kExpectedPaths.state] = "1\n";
    harness.files[kExpectedPaths.duty] = "25000\n";
    harness.files[kExpectedPaths.speed] = "3000\n";
    harness.tach_values = {"3000\n", "0\n"};
    FanService service = Service(&harness);
    const uintptr_t token = request.second ? 7 : 0;
    const FanResponse response =
        service.SetMode(static_cast<FanMode>(request.first), token,
                        request.second);
    CHECK(response.result ==
          (request.second ? FanResult::kInvalidMode
                          : FanResult::kInvalidOwner));
    CheckNoSnapshot(response);
    CHECK(harness.files[kExpectedPaths.state] == "0\n");
  }

  Harness dead;
  dead.files[kExpectedPaths.state] = "1\n";
  dead.files[kExpectedPaths.speed] = "3000\n";
  dead.tach_values = {"3000\n", "0\n"};
  FanService dead_service = Service(&dead);
  const FanResponse response =
      dead_service.SetMode(FanMode::kSport, 9, false);
  CHECK(response.result == FanResult::kInvalidOwner);
  CheckNoSnapshot(response);
  CHECK(dead.files[kExpectedPaths.state] == "0\n");
}

std::vector<std::string> ExpectedTransaction(FanMode mode) {
  const int duty = mode == FanMode::kOff
                       ? ayn::fan::kOffDuty
                       : mode == FanMode::kQuiet ? ayn::fan::kQuietDuty
                                                 : ayn::fan::kSportDuty;
  std::vector<std::string> events = {
      "R " + kExpectedPaths.state,
      "R " + kExpectedPaths.duty,
      "R " + kExpectedPaths.period,
      "R " + kExpectedPaths.speed,
      "W " + kExpectedPaths.state + "=0",
      "R " + kExpectedPaths.state,
      "R " + kExpectedPaths.period,
      "W " + kExpectedPaths.duty + "=" + std::to_string(duty),
      "R " + kExpectedPaths.duty,
  };
  if (mode != FanMode::kOff) {
    events.push_back("W " + kExpectedPaths.state + "=1");
    events.push_back("R " + kExpectedPaths.state);
  }
  events.push_back("R " + kExpectedPaths.speed);
  return events;
}

void ThreeModesUseExactWriteOrderAndValues() {
  for (FanMode mode : {FanMode::kOff, FanMode::kQuiet, FanMode::kSport}) {
    Harness harness;
    if (mode != FanMode::kOff) {
      harness.enabled_tach = "1200\n";
    }
    FanService service = Service(&harness);
    const FanResponse response = service.SetMode(mode, 11, true);
    CHECK(response.result == FanResult::kOk);
    CHECK(response.snapshot.has_value());
    CHECK(response.snapshot->mode == mode);
    CHECK(response.snapshot->state == (mode == FanMode::kOff ? 0 : 1));
    CHECK(response.snapshot->duty ==
          (mode == FanMode::kOff
               ? ayn::fan::kOffDuty
               : mode == FanMode::kQuiet ? ayn::fan::kQuietDuty
                                         : ayn::fan::kSportDuty));
    CHECK(response.snapshot->tach == (mode == FanMode::kOff ? 0 : 1200));
    CHECK(harness.events == ExpectedTransaction(mode));
  }
}

void PeriodIsReadOnlyAndMustAlreadyMatch() {
  Harness harness;
  harness.files[kExpectedPaths.period] = "40000\n";
  FanService service = Service(&harness);
  const FanResponse response = service.SetMode(FanMode::kQuiet, 1, true);
  CHECK(response.result == FanResult::kDisableUnconfirmed);
  CheckNoSnapshot(response);
  for (const std::string& event : harness.events) {
    CHECK(event.rfind("W " + kExpectedPaths.period, 0) != 0);
  }
}

void PeriodAndTachAreSeparateNodes() {
  Harness harness;
  harness.enabled_tach = "700\n";
  FanService service = Service(&harness);
  const FanResponse response = service.SetMode(FanMode::kQuiet, 1, true);
  CHECK(response.result == FanResult::kOk);
  CHECK(harness.files[kExpectedPaths.period] == "50000\n");
  CHECK(harness.files[kExpectedPaths.speed] == "700\n");
  for (const std::string& event : harness.events) {
    CHECK(event.rfind("W " + kExpectedPaths.speed, 0) != 0);
  }
}

void EveryNormalReadAndWriteFailureCompensatesOff() {
  Harness baseline;
  baseline.enabled_tach = "900\n";
  FanService baseline_service = Service(&baseline);
  CHECK(baseline_service.SetMode(FanMode::kSport, 1, true).result ==
        FanResult::kOk);
  const size_t normal_reads = baseline.read_count;
  const size_t normal_writes = baseline.write_count;

  for (size_t fail_at = 1; fail_at <= normal_reads; ++fail_at) {
    Harness harness;
    harness.enabled_tach = "900\n";
    harness.fail_read_at = fail_at;
    FanService service = Service(&harness);
    const FanResponse response = service.SetMode(FanMode::kSport, 1, true);
    CHECK(response.result == FanResult::kIoError);
    CheckNoSnapshot(response);
    CHECK(harness.files[kExpectedPaths.state] == "0\n");
  }

  for (size_t fail_at = 1; fail_at <= normal_writes; ++fail_at) {
    Harness harness;
    harness.enabled_tach = "900\n";
    harness.fail_write_at = fail_at;
    FanService service = Service(&harness);
    const FanResponse response = service.SetMode(FanMode::kSport, 1, true);
    CHECK(response.result == FanResult::kIoError);
    CheckNoSnapshot(response);
    CHECK(harness.files[kExpectedPaths.state] == "0\n");
  }
}

void TachPollingIsBoundedAndFailClosed() {
  Harness enabled_timeout;
  enabled_timeout.enabled_tach = "0\n";
  FanService enabled_service = Service(&enabled_timeout);
  const FanResponse enabled =
      enabled_service.SetMode(FanMode::kQuiet, 1, true);
  CHECK(enabled.result == FanResult::kTachTimeout);
  CheckNoSnapshot(enabled);
  CHECK(enabled_timeout.files[kExpectedPaths.state] == "0\n");
  CHECK(enabled_timeout.tach_index <= ayn::fan::kTachPollAttempts * 2);

  Harness off_timeout;
  off_timeout.files[kExpectedPaths.speed] = "500\n";
  off_timeout.tach_follows_state = false;
  FanService off_service = Service(&off_timeout);
  const FanResponse off = off_service.SetMode(FanMode::kOff, 1, true);
  CHECK(off.result == FanResult::kDisableUnconfirmed);
  CheckNoSnapshot(off);
  CHECK(off_timeout.read_count < 100);
}

void UnconfirmedOffHasDedicatedResultAndNoPartialSnapshot() {
  Harness harness;
  harness.files[kExpectedPaths.state] = "1\n";
  harness.files[kExpectedPaths.speed] = "1000\n";
  harness.fail_all_writes = true;
  FanService service = Service(&harness);
  const FanResponse response = service.SetMode(FanMode::kSport, 1, true);
  CHECK(response.result == FanResult::kDisableUnconfirmed);
  CheckNoSnapshot(response);
}

void StatusIsCompleteOrCompensatesOff() {
  Harness valid;
  valid.files[kExpectedPaths.state] = "1\n";
  valid.files[kExpectedPaths.duty] = "5000\n";
  valid.files[kExpectedPaths.speed] = "800\n";
  FanService valid_service = Service(&valid);
  const FanResponse status = valid_service.GetStatus();
  CHECK(status.result == FanResult::kOk);
  CHECK(status.snapshot.has_value());
  CHECK(status.snapshot->mode == FanMode::kQuiet);
  CHECK(valid.write_count == 0);

  Harness malformed;
  malformed.files[kExpectedPaths.state] = "1\n";
  malformed.files[kExpectedPaths.duty] = "12345\n";
  malformed.files[kExpectedPaths.speed] = "800\n";
  malformed.tach_values = {"800\n", "0\n"};
  FanService malformed_service = Service(&malformed);
  const FanResponse failed = malformed_service.GetStatus();
  CHECK(failed.result == FanResult::kIoError);
  CheckNoSnapshot(failed);
  CHECK(malformed.files[kExpectedPaths.state] == "0\n");
}

void CurrentOwnerDeathTurnsOffAndStaleDeathDoesNothing() {
  Harness harness;
  harness.enabled_tach = "600\n";
  FanService service = Service(&harness);
  CHECK(service.SetMode(FanMode::kQuiet, 1, true).result == FanResult::kOk);
  CHECK(service.SetMode(FanMode::kSport, 2, true).result == FanResult::kOk);

  harness.events.clear();
  const FanResponse stale = service.OwnerDied(1);
  CHECK(stale.result == FanResult::kNotOwner);
  CHECK(harness.events.empty());

  harness.files[kExpectedPaths.speed] = "0\n";
  const FanResponse current = service.OwnerDied(2);
  CHECK(current.result == FanResult::kOk);
  CHECK(current.snapshot.has_value());
  CHECK(current.snapshot->mode == FanMode::kOff);
  CHECK(harness.files[kExpectedPaths.state] == "0\n");
}

void TransactionsAreSerialized() {
  Harness harness;
  harness.enabled_tach = "500\n";
  harness.block_first_write = true;
  FanService service = Service(&harness);
  FanResponse first;
  FanResponse second;

  std::thread first_thread(
      [&] { first = service.SetMode(FanMode::kQuiet, 1, true); });
  {
    std::unique_lock<std::mutex> lock(harness.block_mutex);
    harness.block_cv.wait(lock, [&] { return harness.first_write_entered; });
  }
  const size_t events_at_block = harness.events.size();
  std::atomic<bool> second_done = false;
  std::thread second_thread([&] {
    second = service.SetMode(FanMode::kSport, 2, true);
    second_done = true;
  });
  std::this_thread::yield();
  CHECK(!second_done.load());
  CHECK(harness.events.size() == events_at_block);
  {
    std::lock_guard<std::mutex> lock(harness.block_mutex);
    harness.release_first_write = true;
  }
  harness.block_cv.notify_all();
  first_thread.join();
  second_thread.join();
  CHECK(first.result == FanResult::kOk);
  CHECK(second.result == FanResult::kOk);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"exact identity and paths gate all I/O", ExactIdentityAndPathsGateAllIo},
      {"invalid mode and owner fail closed", InvalidModeAndOwnerFailClosed},
      {"three modes use exact write order and values",
       ThreeModesUseExactWriteOrderAndValues},
      {"period and tach are separate nodes", PeriodAndTachAreSeparateNodes},
      {"period is read only and must already match",
       PeriodIsReadOnlyAndMustAlreadyMatch},
      {"every normal read and write failure compensates off",
       EveryNormalReadAndWriteFailureCompensatesOff},
      {"tach polling is bounded and fail closed",
       TachPollingIsBoundedAndFailClosed},
      {"unconfirmed off has dedicated result and no partial snapshot",
       UnconfirmedOffHasDedicatedResultAndNoPartialSnapshot},
      {"status is complete or compensates off", StatusIsCompleteOrCompensatesOff},
      {"current owner death turns off and stale death does nothing",
       CurrentOwnerDeathTurnsOffAndStaleDeathDoesNothing},
      {"transactions are serialized", TransactionsAreSerialized},
  };

  size_t passed = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      ++passed;
      std::cout << "[PASS] " << test.first << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n';
      return 1;
    }
  }
  std::cout << passed << " tests passed\n";
  return 0;
}
