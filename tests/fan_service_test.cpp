// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_lifecycle.h"
#include "ayn/fan_policy.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
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

using ayn::fan::ApplyResult;
using ayn::fan::CurveDecision;
using ayn::fan::FanCurveController;
using ayn::fan::FanMode;
using ayn::fan::FanSettings;
using ayn::fan::PolicyResult;
using ayn::fan::SysfsPaths;

const SysfsPaths kExpectedPaths = {
    "/sys/class/gpio5_pwm2/state",
    "/sys/class/gpio5_pwm2/duty",
    "/sys/class/gpio5_pwm2/period",
    "/sys/class/gpio5_pwm2/speed",
};

struct Harness {
  bool stop_requested = false;
  size_t stop_after_writes = 0;
  size_t fail_write_at = 0;
  bool fail_all_writes = false;
  size_t read_calls = 0;
  size_t fail_read_at = 0;
  std::map<std::string, std::string> files = {
      {kExpectedPaths.state, "0\n"},
      {kExpectedPaths.duty, "0\n"},
      {kExpectedPaths.period, "50000\n"},
      {kExpectedPaths.speed, "0\n"},
  };
  std::vector<std::pair<std::string, std::string>> writes;
};

struct SmartHarness {
  bool stop_requested = false;
  bool temperature_valid = true;
  int temperature_c = 40;
  size_t temperature_reads = 0;
  std::vector<int> applied_duties;
  std::vector<int> sleeps;
};

bool StopRequested(void* context) {
  return static_cast<Harness*>(context)->stop_requested;
}

bool ReadFile(void* context, const std::string& path, std::string* value) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->read_calls;
  if (harness->fail_read_at == harness->read_calls) {
    return false;
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
  harness->writes.emplace_back(path, value);
  if (harness->fail_all_writes ||
      harness->fail_write_at == harness->writes.size()) {
    return false;
  }
  harness->files[path] = value + "\n";
  if (harness->stop_after_writes == harness->writes.size()) {
    harness->stop_requested = true;
  }
  return true;
}

bool SmartStopRequested(void* context) {
  return static_cast<SmartHarness*>(context)->stop_requested;
}

bool ReadTemperature(void* context, int* temperature_c) {
  auto* harness = static_cast<SmartHarness*>(context);
  ++harness->temperature_reads;
  if (!harness->temperature_valid) {
    return false;
  }
  *temperature_c = harness->temperature_c;
  return true;
}

bool ApplySmartDuty(void* context, int duty_ns) {
  auto* harness = static_cast<SmartHarness*>(context);
  harness->applied_duties.push_back(duty_ns);
  return true;
}

bool SleepAndStop(void* context, int seconds) {
  auto* harness = static_cast<SmartHarness*>(context);
  harness->sleeps.push_back(seconds);
  harness->stop_requested = true;
  return true;
}

PolicyResult Resolve(FanMode mode, int custom_duty_ns, int temperature_c) {
  return ayn::fan::ResolveDuty({mode, custom_duty_ns}, temperature_c);
}

void SmartFormulaBoundariesMatchObservedContract() {
  CHECK(ayn::fan::kPwmPeriodNs == 50000);
  CHECK(ayn::fan::kSmartPollIntervalSeconds == 5);
  CHECK((Resolve(FanMode::kSmart, 0, 4) == PolicyResult{true, 0}));
  CHECK((Resolve(FanMode::kSmart, 0, 5) == PolicyResult{true, 8000}));
  CHECK((Resolve(FanMode::kSmart, 0, 39) == PolicyResult{true, 8000}));
  CHECK((Resolve(FanMode::kSmart, 0, 40) == PolicyResult{true, 8100}));
  CHECK((Resolve(FanMode::kSmart, 0, 84) == PolicyResult{true, 24710}));
  CHECK((Resolve(FanMode::kSmart, 0, 85) == PolicyResult{true, 25000}));
  CHECK(!Resolve(FanMode::kSmart, 0,
                 ayn::fan::kMinimumTemperatureC - 1)
             .valid);
  CHECK(!Resolve(FanMode::kSmart, 0,
                 ayn::fan::kMaximumTemperatureC + 1)
             .valid);
}

void AutomaticCurvesInterpolateDeterministically() {
  CHECK((Resolve(FanMode::kDisabled, 0, 50) == PolicyResult{true, 0}));
  CHECK((Resolve(FanMode::kQuiet, 0, 39) == PolicyResult{true, 5000}));
  CHECK((Resolve(FanMode::kQuiet, 0, 40) == PolicyResult{true, 5000}));
  CHECK((Resolve(FanMode::kQuiet, 0, 45) == PolicyResult{true, 6500}));
  CHECK((Resolve(FanMode::kQuiet, 0, 55) == PolicyResult{true, 10000}));
  CHECK((Resolve(FanMode::kQuiet, 0, 65) == PolicyResult{true, 14500}));
  CHECK((Resolve(FanMode::kQuiet, 0, 75) == PolicyResult{true, 19500}));
  CHECK((Resolve(FanMode::kQuiet, 0, 85) == PolicyResult{true, 25000}));
  CHECK((Resolve(FanMode::kQuiet, 0, 120) == PolicyResult{true, 25000}));

  CHECK((Resolve(FanMode::kSport, 0, 34) == PolicyResult{true, 8000}));
  CHECK((Resolve(FanMode::kSport, 0, 35) == PolicyResult{true, 8000}));
  CHECK((Resolve(FanMode::kSport, 0, 40) == PolicyResult{true, 10000}));
  CHECK((Resolve(FanMode::kSport, 0, 50) == PolicyResult{true, 14500}));
  CHECK((Resolve(FanMode::kSport, 0, 60) == PolicyResult{true, 19000}));
  CHECK((Resolve(FanMode::kSport, 0, 70) == PolicyResult{true, 23000}));
  CHECK((Resolve(FanMode::kSport, 0, 75) == PolicyResult{true, 25000}));
  CHECK((Resolve(FanMode::kSport, 0, 100) == PolicyResult{true, 25000}));

  for (FanMode mode : {FanMode::kQuiet, FanMode::kSport}) {
    CHECK(!Resolve(mode, 0, ayn::fan::kMinimumTemperatureC - 1).valid);
    CHECK(!Resolve(mode, 0, ayn::fan::kMaximumTemperatureC + 1).valid);
  }
}

void CustomModeRemainsExplicitAndBounded() {
  CHECK((Resolve(FanMode::kCustom, 24999, 50) ==
         PolicyResult{true, 25000}));
  CHECK((Resolve(FanMode::kCustom, 25099, 50) ==
         PolicyResult{true, 25000}));
  CHECK((Resolve(FanMode::kCustom, 25100, 50) ==
         PolicyResult{true, 25100}));
  CHECK((Resolve(FanMode::kCustom, 35099, 50) ==
         PolicyResult{true, 35000}));
}

void CurveControllerUsesHysteresisAndDebounce() {
  FanCurveController controller;
  CHECK((controller.Observe(FanMode::kQuiet, 50) ==
         CurveDecision{true, true, 8000}));
  CHECK((controller.Observe(FanMode::kQuiet, 51) ==
         CurveDecision{true, false, 8000}));
  CHECK((controller.Observe(FanMode::kQuiet, 52) ==
         CurveDecision{true, false, 8000}));
  CHECK((controller.Observe(FanMode::kQuiet, 53) ==
         CurveDecision{true, true, 9200}));

  CHECK((controller.Observe(FanMode::kQuiet, 51) ==
         CurveDecision{true, false, 9200}));
  CHECK((controller.Observe(FanMode::kQuiet, 50) ==
         CurveDecision{true, true, 8000}));
}

void HighTemperatureBypassesDebounceAtBoundedMaximum() {
  FanCurveController controller;
  CHECK((controller.Observe(FanMode::kQuiet, 50) ==
         CurveDecision{true, true, 8000}));
  CHECK((controller.Observe(FanMode::kQuiet,
                            ayn::fan::kHighTemperatureC) ==
         CurveDecision{true, true, ayn::fan::kSafeMaximumDutyNs}));
  CHECK((controller.Observe(FanMode::kQuiet,
                            ayn::fan::kHighTemperatureC + 10) ==
         CurveDecision{true, false, ayn::fan::kSafeMaximumDutyNs}));
  CHECK(ayn::fan::kSafeMaximumDutyNs <= ayn::fan::kCustomMaximumDutyNs);
}

void InvalidSettingsFailClosed() {
  CHECK(!Resolve(static_cast<FanMode>(99), 0, 50).valid);
  CHECK(!Resolve(FanMode::kQuiet, 1, 50).valid);
  CHECK(!Resolve(FanMode::kSmart, 25000, 50).valid);
}

void IdentityAndPathsMustMatchExactly() {
  CHECK(ayn::fan::IsSupportedDevice("odin2_mini"));
  CHECK(!ayn::fan::IsSupportedDevice(""));
  CHECK(!ayn::fan::IsSupportedDevice("kalama"));
  CHECK(!ayn::fan::IsSupportedDevice("Odin2_Mini"));
  CHECK(!ayn::fan::IsSupportedDevice("odin2_mini "));
  CHECK(ayn::fan::AreExpectedSysfsPaths(kExpectedPaths));

  SysfsPaths wrong = kExpectedPaths;
  wrong.duty += ".bak";
  CHECK(!ayn::fan::AreExpectedSysfsPaths(wrong));
}

ApplyResult Apply(Harness* harness, int duty_ns,
                  const std::string& device = "odin2_mini",
                  const SysfsPaths& paths = kExpectedPaths) {
  return ayn::fan::ApplyDutyUnlessStopped(
      device, paths, duty_ns, StopRequested, harness, ReadFile, harness,
      WriteFile, harness);
}

void SnapshotFailuresNeverRiskAnUnknownEnabledState() {
  for (const std::string& path : {kExpectedPaths.state, kExpectedPaths.duty,
                                  kExpectedPaths.period,
                                  kExpectedPaths.speed}) {
    Harness missing;
    missing.files.erase(path);
    CHECK(Apply(&missing, 25000) == ApplyResult::kFailedClosed);
    if (path == kExpectedPaths.state) {
      const std::vector<std::pair<std::string, std::string>> expected = {
          {kExpectedPaths.state, "0"}};
      CHECK(missing.writes == expected);
      CHECK(missing.files[kExpectedPaths.state] == "0\n");
    } else {
      CHECK(missing.writes.empty());
    }
  }

  for (const auto& malformed :
       std::vector<std::pair<std::string, std::string>>{
           {kExpectedPaths.state, "enabled\n"},
           {kExpectedPaths.duty, "12x\n"},
           {kExpectedPaths.period, "0\n"},
           {kExpectedPaths.speed, "stopped\n"},
       }) {
    Harness harness;
    harness.files[malformed.first] = malformed.second;
    CHECK(Apply(&harness, 25000) == ApplyResult::kFailedClosed);
    if (malformed.first == kExpectedPaths.state) {
      const std::vector<std::pair<std::string, std::string>> expected = {
          {kExpectedPaths.state, "0"}};
      CHECK(harness.writes == expected);
      CHECK(harness.files[kExpectedPaths.state] == "0\n");
    } else {
      CHECK(harness.writes.empty());
    }
  }

  Harness enabled_missing_duty;
  enabled_missing_duty.files[kExpectedPaths.state] = "1\n";
  enabled_missing_duty.files.erase(kExpectedPaths.duty);
  CHECK(Apply(&enabled_missing_duty, 25000) == ApplyResult::kFailedClosed);
  CHECK(enabled_missing_duty.writes.back() ==
        std::make_pair(kExpectedPaths.state, std::string("0")));
  CHECK(enabled_missing_duty.files[kExpectedPaths.state] == "0\n");

  Harness unknown_and_unwritable;
  unknown_and_unwritable.files.erase(kExpectedPaths.state);
  unknown_and_unwritable.fail_all_writes = true;
  CHECK(Apply(&unknown_and_unwritable, 25000) ==
        ApplyResult::kDisableUnconfirmed);
}

void UnsupportedIdentityOrPathCannotReachSysfs() {
  Harness identity;
  CHECK(Apply(&identity, 25000, "kalama") == ApplyResult::kFailedClosed);
  CHECK(identity.read_calls == 0);
  CHECK(identity.writes.empty());

  Harness path;
  SysfsPaths wrong = kExpectedPaths;
  wrong.state = "/tmp/state";
  CHECK(Apply(&path, 25000, "odin2_mini", wrong) ==
        ApplyResult::kFailedClosed);
  CHECK(path.read_calls == 0);
  CHECK(path.writes.empty());
}

void InvalidDutyCannotReachSysfs() {
  for (int duty_ns : {-1, ayn::fan::kCustomMaximumDutyNs + 1}) {
    Harness harness;
    CHECK(Apply(&harness, duty_ns) == ApplyResult::kFailedClosed);
    CHECK(harness.read_calls == 0);
    CHECK(harness.writes.empty());
  }
}

void EnableWritesDisabledPeriodDutyThenEnabled() {
  Harness harness;
  CHECK(Apply(&harness, 25000) == ApplyResult::kApplied);
  const std::vector<std::pair<std::string, std::string>> expected = {
      {kExpectedPaths.state, "0"},
      {kExpectedPaths.period, "50000"},
      {kExpectedPaths.duty, "25000"},
      {kExpectedPaths.state, "1"},
  };
  CHECK(harness.writes == expected);
}

void DisableTurnsStateOffBeforeClearingDuty() {
  Harness harness;
  harness.files[kExpectedPaths.state] = "1\n";
  harness.files[kExpectedPaths.duty] = "25000\n";
  CHECK(Apply(&harness, 0) == ApplyResult::kApplied);
  const std::vector<std::pair<std::string, std::string>> expected = {
      {kExpectedPaths.state, "0"},
      {kExpectedPaths.duty, "0"},
  };
  CHECK(harness.writes == expected);
}

void WriteFailureDisablesTheFanFailClosed() {
  for (size_t fail_write_at = 1; fail_write_at <= 4; ++fail_write_at) {
    Harness harness;
    harness.fail_write_at = fail_write_at;
    CHECK(Apply(&harness, 25000) == ApplyResult::kFailedClosed);
    CHECK(harness.writes.size() == fail_write_at + 1);
    CHECK(harness.writes.back() ==
          std::make_pair(kExpectedPaths.state, std::string("0")));
    CHECK(harness.files[kExpectedPaths.state] == "0\n");
  }
}

void FailedDisableCompensationIsNeverReportedFailClosed() {
  Harness harness;
  harness.files[kExpectedPaths.state] = "1\n";
  harness.fail_all_writes = true;
  CHECK(Apply(&harness, 25000) == ApplyResult::kDisableUnconfirmed);
  CHECK(harness.writes.size() == 2);
  CHECK(harness.writes.back() ==
        std::make_pair(kExpectedPaths.state, std::string("0")));
}

void StateReadbackFailureDisablesTheFan() {
  Harness disabling;
  disabling.fail_read_at = 5;
  CHECK(Apply(&disabling, 25000) == ApplyResult::kFailedClosed);
  CHECK(disabling.writes.size() == 2);
  CHECK(disabling.writes.back() ==
        std::make_pair(kExpectedPaths.state, std::string("0")));
  CHECK(disabling.files[kExpectedPaths.state] == "0\n");

  Harness enabling;
  enabling.fail_read_at = 6;
  CHECK(Apply(&enabling, 25000) == ApplyResult::kFailedClosed);
  CHECK(enabling.writes.size() == 5);
  CHECK(enabling.writes.back() ==
        std::make_pair(kExpectedPaths.state, std::string("0")));
  CHECK(enabling.files[kExpectedPaths.state] == "0\n");
}

void StopBehaviorNeverEnablesAfterStop() {
  Harness stopped;
  stopped.stop_requested = true;
  CHECK(Apply(&stopped, 25000) == ApplyResult::kStopped);
  CHECK(stopped.writes.empty());

  Harness during_enable;
  during_enable.stop_after_writes = 3;
  CHECK(Apply(&during_enable, 25000) == ApplyResult::kStopped);
  CHECK(during_enable.writes.size() == 4);
  CHECK(during_enable.writes.back() ==
        std::make_pair(kExpectedPaths.state, std::string("0")));
  CHECK(during_enable.files[kExpectedPaths.state] == "0\n");

  Harness after_enable;
  after_enable.stop_after_writes = 4;
  CHECK(Apply(&after_enable, 25000) == ApplyResult::kStopped);
  CHECK(after_enable.writes.size() == 5);
  CHECK(after_enable.writes.back() ==
        std::make_pair(kExpectedPaths.state, std::string("0")));
  CHECK(after_enable.files[kExpectedPaths.state] == "0\n");
}

void SmartLoopPollsEveryFiveSecondsAndStopsPromptly() {
  SmartHarness harness;
  CHECK(ayn::fan::RunSmartLoopUnlessStopped(
            SmartStopRequested, &harness, ReadTemperature, &harness,
            ApplySmartDuty, &harness, SleepAndStop, &harness) ==
        ayn::fan::SmartLoopResult::kStopped);
  CHECK(harness.temperature_reads == 1);
  CHECK(harness.applied_duties == std::vector<int>{8100});
  CHECK(harness.sleeps == std::vector<int>{5});
}

void SmartLoopFailsClosedOnMissingTemperature() {
  SmartHarness malformed;
  malformed.temperature_valid = false;
  CHECK(ayn::fan::RunSmartLoopUnlessStopped(
            SmartStopRequested, &malformed, ReadTemperature, &malformed,
            ApplySmartDuty, &malformed, SleepAndStop, &malformed) ==
        ayn::fan::SmartLoopResult::kFailedClosed);
  CHECK(malformed.temperature_reads == 1);
  CHECK(malformed.applied_duties.empty());
  CHECK(malformed.sleeps.empty());

  SmartHarness stopped;
  stopped.stop_requested = true;
  CHECK(ayn::fan::RunSmartLoopUnlessStopped(
            SmartStopRequested, &stopped, ReadTemperature, &stopped,
            ApplySmartDuty, &stopped, SleepAndStop, &stopped) ==
        ayn::fan::SmartLoopResult::kStopped);
  CHECK(stopped.temperature_reads == 0);
  CHECK(stopped.applied_duties.empty());

  SmartHarness out_of_range;
  out_of_range.temperature_c = ayn::fan::kMaximumTemperatureC + 1;
  CHECK(ayn::fan::RunSmartLoopUnlessStopped(
            SmartStopRequested, &out_of_range, ReadTemperature, &out_of_range,
            ApplySmartDuty, &out_of_range, SleepAndStop, &out_of_range) ==
        ayn::fan::SmartLoopResult::kFailedClosed);
  CHECK(out_of_range.temperature_reads == 1);
  CHECK(out_of_range.applied_duties.empty());
  CHECK(out_of_range.sleeps.empty());
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"smart formula boundaries match observed contract",
       SmartFormulaBoundariesMatchObservedContract},
      {"automatic curves interpolate deterministically",
       AutomaticCurvesInterpolateDeterministically},
      {"custom mode remains explicit and bounded",
       CustomModeRemainsExplicitAndBounded},
      {"curve controller uses hysteresis and debounce",
       CurveControllerUsesHysteresisAndDebounce},
      {"high temperature bypasses debounce at bounded maximum",
       HighTemperatureBypassesDebounceAtBoundedMaximum},
      {"invalid settings fail closed", InvalidSettingsFailClosed},
      {"identity and paths must match exactly",
       IdentityAndPathsMustMatchExactly},
      {"snapshot failures never risk an unknown enabled state",
       SnapshotFailuresNeverRiskAnUnknownEnabledState},
      {"unsupported identity or path cannot reach sysfs",
       UnsupportedIdentityOrPathCannotReachSysfs},
      {"invalid duty cannot reach sysfs", InvalidDutyCannotReachSysfs},
      {"enable writes disabled, period, duty, then enabled",
       EnableWritesDisabledPeriodDutyThenEnabled},
      {"disable turns state off before clearing duty",
       DisableTurnsStateOffBeforeClearingDuty},
      {"write failure disables the fan fail closed",
       WriteFailureDisablesTheFanFailClosed},
      {"failed disable compensation is never reported fail closed",
       FailedDisableCompensationIsNeverReportedFailClosed},
      {"state readback failure disables the fan",
       StateReadbackFailureDisablesTheFan},
      {"stop behavior never enables after stop",
       StopBehaviorNeverEnablesAfterStop},
      {"smart loop polls every five seconds and stops promptly",
       SmartLoopPollsEveryFiveSecondsAndStopsPromptly},
      {"smart loop fails closed on missing temperature",
       SmartLoopFailsClosedOnMissingTemperature},
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
