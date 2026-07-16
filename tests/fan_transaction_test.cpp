// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_service.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <iostream>
#include <iterator>
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
  bool temperature_valid = true;
  int temperature_c = 50;
  size_t temperature_reads = 0;
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

bool ReadTemperature(void* context, int* temperature_c) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->temperature_reads;
  if (!harness->temperature_valid) {
    return false;
  }
  *temperature_c = harness->temperature_c;
  return true;
}

FanService Service(Harness* harness,
                   const FanDeviceIdentity& identity = {
                       "odin2_mini", "lineage_odin2_mini", "Odin2 Mini"},
                   const SysfsPaths& paths = kExpectedPaths) {
  return FanService(identity, paths, ReadFile, harness, WriteFile,
                    harness, ReadTemperature, harness,
                    SleepForMilliseconds, harness);
}

FanService ServiceWithoutTemperature(Harness* harness) {
  return FanService(
      {"odin2_mini", "lineage_odin2_mini", "Odin2 Mini"}, kExpectedPaths,
      ReadFile, harness, WriteFile, harness, nullptr, nullptr,
      SleepForMilliseconds, harness);
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
    const FanResponse response = identity_service.SetMode(FanMode::kQuiet);
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
    const FanResponse response = path_service.SetMode(FanMode::kQuiet);
    CHECK(response.result == FanResult::kUnexpectedPaths);
    CheckNoSnapshot(response);
    CHECK(wrong_path.events.empty());
  }
}

void InvalidModeFailsClosed() {
  Harness harness;
  harness.files[kExpectedPaths.state] = "1\n";
  harness.files[kExpectedPaths.duty] = "25000\n";
  harness.files[kExpectedPaths.speed] = "3000\n";
  harness.tach_values = {"3000\n", "0\n"};
  FanService service = Service(&harness);
  const FanResponse response = service.SetMode(static_cast<FanMode>(99));
  CHECK(response.result == FanResult::kInvalidMode);
  CheckNoSnapshot(response);
  CHECK(harness.files[kExpectedPaths.state] == "0\n");
}

std::vector<std::string> ExpectedTransaction(FanMode mode, int temperature_c) {
  const int duty = mode == FanMode::kOff
                       ? ayn::fan::kOffDuty
                       : ayn::fan::ResolveDuty({mode, 0}, temperature_c)
                             .duty_ns;
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

void OffAndAutomaticModesUseExactWriteOrderAndCurveValues() {
  for (FanMode mode : {FanMode::kOff, FanMode::kQuiet, FanMode::kSport}) {
    Harness harness;
    if (mode != FanMode::kOff) {
      harness.enabled_tach = "1200\n";
    }
    FanService service = Service(&harness);
    const FanResponse response = service.SetMode(mode);
    CHECK(response.result == FanResult::kOk);
    CHECK(response.snapshot.has_value());
    CHECK(response.snapshot->mode == mode);
    CHECK(response.snapshot->state == (mode == FanMode::kOff ? 0 : 1));
    const int expected_duty =
        mode == FanMode::kOff
            ? ayn::fan::kOffDuty
            : ayn::fan::ResolveDuty({mode, 0}, harness.temperature_c).duty_ns;
    CHECK(response.snapshot->duty == expected_duty);
    CHECK(response.snapshot->tach == (mode == FanMode::kOff ? 0 : 1200));
    CHECK(harness.temperature_reads == (mode == FanMode::kOff ? 0 : 1));
    CHECK(harness.events == ExpectedTransaction(mode, harness.temperature_c));
  }
}

void OffDoesNotDependOnTemperatureAvailability() {
  Harness harness;
  FanService service = ServiceWithoutTemperature(&harness);
  const FanResponse off = service.SetMode(FanMode::kOff);
  CHECK(off.result == FanResult::kOk);
  CHECK(off.snapshot.has_value());
  CHECK(off.snapshot->state == 0);

  const FanResponse quiet = service.SetMode(FanMode::kQuiet);
  CHECK(quiet.result == FanResult::kTemperatureUnavailable);
  CHECK(harness.files[kExpectedPaths.state] == "0\n");
}

void PeriodIsReadOnlyAndMustAlreadyMatch() {
  Harness harness;
  harness.files[kExpectedPaths.period] = "40000\n";
  FanService service = Service(&harness);
  const FanResponse response = service.SetMode(FanMode::kQuiet);
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
  const FanResponse response = service.SetMode(FanMode::kQuiet);
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
  CHECK(baseline_service.SetMode(FanMode::kSport).result ==
        FanResult::kOk);
  const size_t normal_reads = baseline.read_count;
  const size_t normal_writes = baseline.write_count;

  for (size_t fail_at = 1; fail_at <= normal_reads; ++fail_at) {
    Harness harness;
    harness.enabled_tach = "900\n";
    harness.fail_read_at = fail_at;
    FanService service = Service(&harness);
    const FanResponse response = service.SetMode(FanMode::kSport);
    CHECK(response.result == FanResult::kIoError);
    CheckNoSnapshot(response);
    CHECK(harness.files[kExpectedPaths.state] == "0\n");
  }

  for (size_t fail_at = 1; fail_at <= normal_writes; ++fail_at) {
    Harness harness;
    harness.enabled_tach = "900\n";
    harness.fail_write_at = fail_at;
    FanService service = Service(&harness);
    const FanResponse response = service.SetMode(FanMode::kSport);
    CHECK(response.result == FanResult::kIoError);
    CheckNoSnapshot(response);
    CHECK(harness.files[kExpectedPaths.state] == "0\n");
  }
}

void TachPollingIsBoundedAndFailClosed() {
  Harness enabled_timeout;
  enabled_timeout.enabled_tach = "0\n";
  FanService enabled_service = Service(&enabled_timeout);
  const FanResponse enabled = enabled_service.SetMode(FanMode::kQuiet);
  CHECK(enabled.result == FanResult::kTachTimeout);
  CheckNoSnapshot(enabled);
  CHECK(enabled_timeout.files[kExpectedPaths.state] == "0\n");
  CHECK(enabled_timeout.tach_index <= ayn::fan::kTachPollAttempts * 2);

  Harness off_timeout;
  off_timeout.files[kExpectedPaths.speed] = "500\n";
  off_timeout.tach_follows_state = false;
  FanService off_service = Service(&off_timeout);
  const FanResponse off = off_service.SetMode(FanMode::kOff);
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
  const FanResponse response = service.SetMode(FanMode::kSport);
  CHECK(response.result == FanResult::kDisableUnconfirmed);
  CheckNoSnapshot(response);
}

void StatusIsCompleteOrCompensatesOff() {
  Harness valid;
  valid.enabled_tach = "800\n";
  FanService valid_service = Service(&valid);
  CHECK(valid_service.SetMode(FanMode::kQuiet).result ==
        FanResult::kOk);
  valid.events.clear();
  valid.write_count = 0;
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

void RefreshUsesHysteresisAndDebounceWithoutHunting() {
  Harness harness;
  harness.enabled_tach = "900\n";
  FanService service = Service(&harness);
  CHECK(service.SetMode(FanMode::kQuiet).snapshot->duty == 8000);

  harness.events.clear();
  harness.temperature_c = 51;
  CHECK(service.Refresh().snapshot->duty == 8000);
  for (const std::string& event : harness.events) {
    CHECK(event.rfind("W ", 0) != 0);
  }

  harness.events.clear();
  harness.temperature_c = 52;
  CHECK(service.Refresh().snapshot->duty == 8000);
  for (const std::string& event : harness.events) {
    CHECK(event.rfind("W ", 0) != 0);
  }

  harness.events.clear();
  harness.temperature_c = 53;
  const FanResponse changed = service.Refresh();
  CHECK(changed.result == FanResult::kOk);
  CHECK(changed.snapshot->duty == 9200);
  CHECK(std::find(harness.events.begin(), harness.events.end(),
                  "W " + kExpectedPaths.duty + "=9200") !=
        harness.events.end());
}

void MissingTemperatureFailsClosed() {
  Harness harness;
  harness.enabled_tach = "700\n";
  FanService service = Service(&harness);
  CHECK(service.SetMode(FanMode::kSport).result == FanResult::kOk);

  harness.temperature_valid = false;
  harness.files[kExpectedPaths.speed] = "0\n";
  const FanResponse failed = service.Refresh();
  CHECK(failed.result == FanResult::kTemperatureUnavailable);
  CheckNoSnapshot(failed);
  CHECK(harness.files[kExpectedPaths.state] == "0\n");
}

void SteadyTemperatureRefreshDetectsAStalledFan() {
  Harness harness;
  harness.enabled_tach = "700\n";
  FanService service = Service(&harness);
  CHECK(service.SetMode(FanMode::kSport).result == FanResult::kOk);

  harness.tach_follows_state = false;
  harness.files[kExpectedPaths.speed] = "0\n";
  const FanResponse failed = service.Refresh();
  CHECK(failed.result == FanResult::kTachTimeout);
  CheckNoSnapshot(failed);
  CHECK(harness.files[kExpectedPaths.state] == "0\n");
}

void HighTemperatureImmediatelyUsesSafeBoundedMaximum() {
  Harness harness;
  harness.enabled_tach = "1100\n";
  FanService service = Service(&harness);
  CHECK(service.SetMode(FanMode::kQuiet).result == FanResult::kOk);

  harness.events.clear();
  harness.temperature_c = ayn::fan::kHighTemperatureC;
  const FanResponse hot = service.Refresh();
  CHECK(hot.result == FanResult::kOk);
  CHECK(hot.snapshot->duty == ayn::fan::kSafeMaximumDutyNs);
  CHECK(hot.snapshot->duty <= ayn::fan::kCustomMaximumDutyNs);
  CHECK(std::find(harness.events.begin(), harness.events.end(),
                  "W " + kExpectedPaths.duty + "=" +
                      std::to_string(ayn::fan::kSafeMaximumDutyNs)) !=
        harness.events.end());
}

void ActiveModeRemainsDaemonOwnedAcrossRefresh() {
  Harness harness;
  harness.enabled_tach = "600\n";
  FanService service = Service(&harness);
  CHECK(service.SetMode(FanMode::kSport).result == FanResult::kOk);

  harness.events.clear();
  const FanResponse refreshed = service.Refresh();
  CHECK(refreshed.result == FanResult::kOk);
  CHECK(refreshed.snapshot.has_value());
  CHECK(refreshed.snapshot->mode == FanMode::kSport);
  CHECK(harness.files[kExpectedPaths.state] == "1\n");
}

void StartupEstablishesOffBaselineThenQuietDefault() {
  Harness harness;
  harness.files[kExpectedPaths.state] = "1\n";
  harness.files[kExpectedPaths.duty] = "25000\n";
  harness.files[kExpectedPaths.speed] = "3000\n";
  FanService service = Service(&harness);

  const FanResponse startup = service.InitializeSafeDefault();

  CHECK(startup.result == FanResult::kOk);
  CHECK(startup.snapshot.has_value());
  CHECK(startup.snapshot->mode == FanMode::kQuiet);
  const std::vector<std::string> expected_writes = {
      "W " + kExpectedPaths.state + "=0",
      "W " + kExpectedPaths.duty + "=10000",
      "W " + kExpectedPaths.state + "=0",
      "W " + kExpectedPaths.duty + "=8000",
      "W " + kExpectedPaths.state + "=1",
  };
  std::vector<std::string> writes;
  std::copy_if(harness.events.begin(), harness.events.end(),
               std::back_inserter(writes),
               [](const std::string& event) {
                 return event.rfind("W ", 0) == 0;
               });
  CHECK(writes == expected_writes);
}

void StartupFailureNeverReachesQuiet() {
  Harness harness;
  harness.files[kExpectedPaths.state] = "1\n";
  harness.files[kExpectedPaths.speed] = "3000\n";
  harness.fail_all_writes = true;
  FanService service = Service(&harness);

  const FanResponse startup = service.InitializeSafeDefault();

  CHECK(startup.result != FanResult::kOk);
  CHECK(std::find(harness.events.begin(), harness.events.end(),
                  "W " + kExpectedPaths.state + "=1") ==
        harness.events.end());
}

void StartupQuietFailureLeavesConfirmedOff() {
  Harness harness;
  harness.temperature_valid = false;
  FanService service = Service(&harness);

  const FanResponse startup = service.InitializeSafeDefault();

  CHECK(startup.result == FanResult::kTemperatureUnavailable);
  CHECK(!startup.snapshot.has_value());
  CHECK(harness.files[kExpectedPaths.state] == "0\n");
  CHECK(harness.files[kExpectedPaths.duty] == "10000\n");
  CHECK(harness.files[kExpectedPaths.speed] == "0\n");
  CHECK(std::find(harness.events.begin(), harness.events.end(),
                  "W " + kExpectedPaths.state + "=1") ==
        harness.events.end());
}

void TransactionsAreSerialized() {
  Harness harness;
  harness.enabled_tach = "500\n";
  harness.block_first_write = true;
  FanService service = Service(&harness);
  FanResponse first;
  FanResponse second;

  std::thread first_thread(
      [&] { first = service.SetMode(FanMode::kQuiet); });
  {
    std::unique_lock<std::mutex> lock(harness.block_mutex);
    harness.block_cv.wait(lock, [&] { return harness.first_write_entered; });
  }
  const size_t events_at_block = harness.events.size();
  std::atomic<bool> second_done = false;
  std::thread second_thread([&] {
    second = service.SetMode(FanMode::kSport);
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
      {"invalid mode fails closed", InvalidModeFailsClosed},
      {"off and automatic modes use exact write order and curve values",
       OffAndAutomaticModesUseExactWriteOrderAndCurveValues},
      {"off does not depend on temperature availability",
       OffDoesNotDependOnTemperatureAvailability},
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
      {"refresh uses hysteresis and debounce without hunting",
       RefreshUsesHysteresisAndDebounceWithoutHunting},
      {"missing temperature fails closed", MissingTemperatureFailsClosed},
      {"steady temperature refresh detects a stalled fan",
       SteadyTemperatureRefreshDetectsAStalledFan},
      {"high temperature immediately uses safe bounded maximum",
       HighTemperatureImmediatelyUsesSafeBoundedMaximum},
      {"active mode remains daemon-owned across refresh",
       ActiveModeRemainsDaemonOwnedAcrossRefresh},
      {"startup establishes Off baseline then Quiet default",
       StartupEstablishesOffBaselineThenQuietDefault},
      {"startup failure never reaches Quiet", StartupFailureNeverReachesQuiet},
      {"startup Quiet failure leaves confirmed Off",
       StartupQuietFailureLeavesConfirmedOff},
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
