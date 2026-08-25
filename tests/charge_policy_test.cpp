// SPDX-License-Identifier: Apache-2.0

#include "ayn/charge_policy.h"

#include <iostream>
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

using ayn::charge::AreExpectedSysfsPaths;
using ayn::charge::AreValidSettings;
using ayn::charge::ChargeMode;
using ayn::charge::IsKnownMode;
using ayn::charge::ChargeAction;
using ayn::charge::Decide;
using ayn::charge::IsSupportedDevice;
using ayn::charge::kDefaultResumePercent;
using ayn::charge::kDefaultStopPercent;
using ayn::charge::kNeverRestrictBelowPercent;
using ayn::charge::LimitSettings;
using ayn::charge::StockSysfsPaths;
using ayn::charge::SysfsPaths;

LimitSettings Defaults() {
  return {ChargeMode::kLimit, kDefaultStopPercent, kDefaultResumePercent};
}

LimitSettings Bypass() {
  return {ChargeMode::kBypass, kDefaultStopPercent, kDefaultResumePercent};
}

ChargeAction ActionAt(const LimitSettings& settings, int capacity) {
  const auto decision = Decide(settings, capacity);
  CHECK(decision.valid);
  return decision.action;
}

void FullBatteryStopsCharging() {
  CHECK(ActionAt(Defaults(), 100) == ChargeAction::kRestrict);
  CHECK(ActionAt(Defaults(), 80) == ChargeAction::kRestrict);
}

void BelowResumeThresholdChargesAgain() {
  CHECK(ActionAt(Defaults(), 75) == ChargeAction::kAllow);
  CHECK(ActionAt(Defaults(), 74) == ChargeAction::kAllow);
}

void HysteresisBandLeavesTheChargerAlone() {
  CHECK(ActionAt(Defaults(), 76) == ChargeAction::kHold);
  CHECK(ActionAt(Defaults(), 79) == ChargeAction::kHold);
}

void LowBatteryIsNeverHeldOffTheCharger() {
  // Even a policy that would otherwise restrict must charge a low battery.
  const LimitSettings aggressive = {ChargeMode::kLimit, 50,
                                    kNeverRestrictBelowPercent};
  CHECK(ActionAt(aggressive, kNeverRestrictBelowPercent - 1) ==
        ChargeAction::kAllow);
  CHECK(ActionAt(aggressive, 0) == ChargeAction::kAllow);
}

void DisabledPolicyAlwaysReleasesTheRestriction() {
  const LimitSettings off = {ChargeMode::kOff, kDefaultStopPercent,
                             kDefaultResumePercent};
  CHECK(ActionAt(off, 100) == ChargeAction::kAllow);
  CHECK(ActionAt(off, 0) == ChargeAction::kAllow);
}

void UntrustworthyInputFailsClosed() {
  CHECK(!Decide(Defaults(), 101).valid);
  CHECK(!Decide(Defaults(), -1).valid);
  CHECK(Decide(Defaults(), 101).action == ChargeAction::kInvalid);
}

void ImpossibleThresholdsAreRejected() {
  const auto limit = [](int stop, int resume) {
    return LimitSettings{ChargeMode::kLimit, stop, resume};
  };
  CHECK(!AreValidSettings(limit(80, 80)));   // no band
  CHECK(!AreValidSettings(limit(80, 81)));   // inverted
  CHECK(!AreValidSettings(limit(80, 79)));   // band too narrow
  CHECK(!AreValidSettings(limit(40, 38)));   // stop below the floor
  CHECK(!AreValidSettings(limit(100, 90)));  // not a limit
  CHECK(!AreValidSettings(limit(60, 39)));   // resume under the safety floor
  CHECK(AreValidSettings(limit(80, 75)));
  CHECK(AreValidSettings(limit(50, 40)));
  // Off and bypass are valid whatever the thresholds say, because neither
  // consults them.
  CHECK(AreValidSettings({ChargeMode::kOff, 0, 0}));
  CHECK(AreValidSettings({ChargeMode::kBypass, 0, 0}));
  CHECK(!AreValidSettings({static_cast<ChargeMode>(7), 80, 75}));
}

void RejectedSettingsNeverProduceARestriction() {
  const LimitSettings inverted = {ChargeMode::kLimit, 80, 81};
  const auto decision = Decide(inverted, 100);
  CHECK(!decision.valid);
  CHECK(decision.action != ChargeAction::kRestrict);
}

void UnknownDeviceAndPathsAreRefused() {
  CHECK(IsSupportedDevice("odin2_mini"));
  CHECK(!IsSupportedDevice("odin2"));
  CHECK(!IsSupportedDevice(""));
  CHECK(AreExpectedSysfsPaths(StockSysfsPaths()));

  SysfsPaths moved = StockSysfsPaths();
  moved.restrict_chg = "/data/local/tmp/restrict_chg";
  CHECK(!AreExpectedSysfsPaths(moved));
}

void ThresholdsSweepMonotonically() {
  // Walking the whole range must never produce Restrict below Allow.
  const LimitSettings settings = Defaults();
  bool seen_restrict = false;
  for (int capacity = 100; capacity >= 0; --capacity) {
    const ChargeAction action = ActionAt(settings, capacity);
    if (action == ChargeAction::kRestrict) {
      seen_restrict = true;
      CHECK(capacity >= settings.stop_percent);
    }
    if (action == ChargeAction::kAllow && capacity > settings.stop_percent) {
      throw std::runtime_error("charging allowed above the stop threshold");
    }
  }
  CHECK(seen_restrict);
}

void BypassHoldsChargingOffAtAnyCapacityAboveTheFloor() {
  CHECK(ActionAt(Bypass(), 100) == ChargeAction::kRestrict);
  CHECK(ActionAt(Bypass(), 90) == ChargeAction::kRestrict);
  CHECK(ActionAt(Bypass(), kNeverRestrictBelowPercent) ==
        ChargeAction::kRestrict);
  // Bypass ignores the limit thresholds entirely: 50 sits inside the default
  // band where limit mode would hold.
  CHECK(ActionAt(Bypass(), 50) == ChargeAction::kRestrict);
}

void TheFloorOutranksBypass() {
  // If the load outruns the adapter the pack drains, and bypass must let go
  // before the device dies rather than holding charging off to the end.
  CHECK(ActionAt(Bypass(), kNeverRestrictBelowPercent - 1) ==
        ChargeAction::kAllow);
  CHECK(ActionAt(Bypass(), 5) == ChargeAction::kAllow);
  CHECK(ActionAt(Bypass(), 0) == ChargeAction::kAllow);
}

void OnlyTheThreeModesExist() {
  CHECK(IsKnownMode(0));
  CHECK(IsKnownMode(1));
  CHECK(IsKnownMode(2));
  CHECK(!IsKnownMode(-1));
  CHECK(!IsKnownMode(3));
  CHECK(!Decide({static_cast<ChargeMode>(3), 80, 75}, 90).valid);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"a full battery stops charging", FullBatteryStopsCharging},
      {"below the resume threshold charging restarts",
       BelowResumeThresholdChargesAgain},
      {"the hysteresis band leaves the charger alone",
       HysteresisBandLeavesTheChargerAlone},
      {"a low battery is never held off the charger",
       LowBatteryIsNeverHeldOffTheCharger},
      {"a disabled policy always releases the restriction",
       DisabledPolicyAlwaysReleasesTheRestriction},
      {"untrustworthy input fails closed", UntrustworthyInputFailsClosed},
      {"impossible thresholds are rejected", ImpossibleThresholdsAreRejected},
      {"rejected settings never produce a restriction",
       RejectedSettingsNeverProduceARestriction},
      {"unknown device and moved paths are refused",
       UnknownDeviceAndPathsAreRefused},
      {"thresholds sweep monotonically", ThresholdsSweepMonotonically},
      {"bypass holds charging off at any capacity above the floor",
       BypassHoldsChargingOffAtAnyCapacityAboveTheFloor},
      {"the floor outranks bypass", TheFloorOutranksBypass},
      {"only the three modes exist", OnlyTheThreeModesExist},
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
