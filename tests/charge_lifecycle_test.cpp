// SPDX-License-Identifier: Apache-2.0

#include "ayn/charge_lifecycle.h"

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

using ayn::charge::ApplyOnceUnlessStopped;
using ayn::charge::ApplyResult;
using ayn::charge::ChargeMode;
using ayn::charge::kDefaultResumePercent;
using ayn::charge::kDefaultStopPercent;
using ayn::charge::LimitSettings;
using ayn::charge::LoopResult;
using ayn::charge::ParseCapacity;
using ayn::charge::ReleaseRestriction;
using ayn::charge::RunLimitLoopUnlessStopped;
using ayn::charge::StockSysfsPaths;
using ayn::charge::SysfsPaths;

LimitSettings Defaults() {
  return {ChargeMode::kLimit, kDefaultStopPercent, kDefaultResumePercent};
}

struct Harness {
  std::map<std::string, std::string> files;
  std::vector<std::string> writes;
  bool stop = false;
  bool fail_reads = false;
  bool fail_writes = false;
  int sleeps = 0;
  int sleep_until_stop = -1;

  Harness() {
    const SysfsPaths& paths = StockSysfsPaths();
    files[paths.capacity] = "50\n";
    files[paths.restrict_chg] = "0\n";
    files[paths.restrict_cur] = "1000000\n";
  }

  void SetCapacity(int percent) {
    files[StockSysfsPaths().capacity] = std::to_string(percent) + "\n";
  }
  void SetRestricted(bool restricted) {
    files[StockSysfsPaths().restrict_chg] = restricted ? "1\n" : "0\n";
  }
  bool Restricted() const {
    return files.at(StockSysfsPaths().restrict_chg) == "1\n";
  }
};

bool StopRequested(void* context) {
  return static_cast<Harness*>(context)->stop;
}

bool ReadFile(void* context, const std::string& path, std::string* value) {
  Harness& harness = *static_cast<Harness*>(context);
  if (harness.fail_reads) {
    return false;
  }
  const auto found = harness.files.find(path);
  if (found == harness.files.end()) {
    return false;
  }
  *value = found->second;
  return true;
}

bool WriteFile(void* context, const std::string& path,
               const std::string& value) {
  Harness& harness = *static_cast<Harness*>(context);
  if (harness.fail_writes) {
    return false;
  }
  harness.writes.push_back(path + "=" + value);
  harness.files[path] = value;
  return true;
}

bool SleepForSeconds(void* context, int) {
  Harness& harness = *static_cast<Harness*>(context);
  ++harness.sleeps;
  if (harness.sleep_until_stop >= 0 &&
      harness.sleeps >= harness.sleep_until_stop) {
    harness.stop = true;
  }
  return true;
}

ApplyResult Apply(Harness& harness, const LimitSettings& settings = Defaults(),
                  const std::string& device = "odin2_mini") {
  return ApplyOnceUnlessStopped(device, StockSysfsPaths(), settings,
                                StopRequested, &harness, ReadFile, &harness,
                                WriteFile, &harness);
}

void ReachingTheStopThresholdRestrictsCharging() {
  Harness harness;
  harness.SetCapacity(80);
  CHECK(Apply(harness) == ApplyResult::kRestricted);
  CHECK(harness.Restricted());
}

void TheCurrentIsZeroedBeforeTheRestrictionIsEnabled() {
  // Enabling first would charge at the previous limit for the gap between the
  // two writes.
  Harness harness;
  harness.SetCapacity(85);
  CHECK(Apply(harness) == ApplyResult::kRestricted);
  CHECK(harness.writes.size() == 2);
  CHECK(harness.writes[0] == StockSysfsPaths().restrict_cur + "=0\n");
  CHECK(harness.writes[1] == StockSysfsPaths().restrict_chg + "=1\n");
}

void FallingBelowResumeReleasesCharging() {
  Harness harness;
  harness.SetCapacity(75);
  harness.SetRestricted(true);
  CHECK(Apply(harness) == ApplyResult::kReleased);
  CHECK(!harness.Restricted());
}

void TheHysteresisBandWritesNothing() {
  Harness harness;
  harness.SetCapacity(78);
  harness.SetRestricted(true);
  CHECK(Apply(harness) == ApplyResult::kUnchanged);
  CHECK(harness.writes.empty());
  CHECK(harness.Restricted());
}

void AnAlreadyCorrectStateWritesNothing() {
  Harness harness;
  harness.SetCapacity(90);
  harness.SetRestricted(true);
  CHECK(Apply(harness) == ApplyResult::kUnchanged);
  CHECK(harness.writes.empty());
}

void AnUnreadableCapacityReleasesTheRestriction() {
  // The failure must never be "battery stays uncharged".
  Harness harness;
  harness.SetRestricted(true);
  harness.files.erase(StockSysfsPaths().capacity);
  CHECK(Apply(harness) == ApplyResult::kFailedClosed);
  CHECK(!harness.Restricted());
}

void AMalformedCapacityReleasesTheRestriction() {
  Harness harness;
  harness.SetRestricted(true);
  harness.files[StockSysfsPaths().capacity] = "full\n";
  CHECK(Apply(harness) == ApplyResult::kFailedClosed);
  CHECK(!harness.Restricted());
}

void ImpossibleSettingsReleaseTheRestriction() {
  Harness harness;
  harness.SetCapacity(90);
  harness.SetRestricted(true);
  const LimitSettings inverted = {ChargeMode::kLimit, 80, 81};
  CHECK(Apply(harness, inverted) == ApplyResult::kFailedClosed);
  CHECK(!harness.Restricted());
}

void AnUnknownDeviceWritesNothing() {
  Harness harness;
  harness.SetCapacity(90);
  CHECK(Apply(harness, Defaults(), "odin2") == ApplyResult::kFailedClosed);
  CHECK(harness.writes.empty());
}

void StoppingIsHonouredBeforeAnyWrite() {
  Harness harness;
  harness.SetCapacity(90);
  harness.stop = true;
  CHECK(Apply(harness) == ApplyResult::kStopped);
  CHECK(harness.writes.empty());
}

void StartupClearsARestrictionLeftByAKilledDaemon() {
  Harness harness;
  harness.SetRestricted(true);
  const ApplyResult result =
      ReleaseRestriction("odin2_mini", StockSysfsPaths(), ReadFile, &harness,
                         WriteFile, &harness);
  CHECK(result == ApplyResult::kReleased);
  CHECK(!harness.Restricted());
}

void ReleasingAnUnrestrictedChargerWritesNothing() {
  Harness harness;
  const ApplyResult result =
      ReleaseRestriction("odin2_mini", StockSysfsPaths(), ReadFile, &harness,
                         WriteFile, &harness);
  CHECK(result == ApplyResult::kUnchanged);
  CHECK(harness.writes.empty());
}

void TheLoopNeverExitsLeavingChargingBlocked() {
  Harness harness;
  harness.SetCapacity(90);
  harness.sleep_until_stop = 2;
  const LoopResult result = RunLimitLoopUnlessStopped(
      "odin2_mini", StockSysfsPaths(), Defaults(), StopRequested, &harness,
      ReadFile, &harness, WriteFile, &harness, SleepForSeconds, &harness);
  CHECK(result == LoopResult::kStopped);
  CHECK(!harness.Restricted());
}

void CapacityParsingRejectsRubbish() {
  int value = -1;
  CHECK(ParseCapacity("0\n", &value) && value == 0);
  CHECK(ParseCapacity("100\n", &value) && value == 100);
  CHECK(!ParseCapacity("101\n", &value));
  CHECK(!ParseCapacity("-5\n", &value));
  CHECK(!ParseCapacity("", &value));
  CHECK(!ParseCapacity("full\n", &value));
  CHECK(!ParseCapacity("80%\n", &value));
}

void BypassRestrictsWithoutConsultingTheThresholds() {
  Harness harness;
  harness.SetCapacity(50);
  const LimitSettings bypass = {ChargeMode::kBypass, kDefaultStopPercent,
                                kDefaultResumePercent};
  CHECK(ApplyOnceUnlessStopped("odin2_mini", StockSysfsPaths(), bypass,
                               StopRequested, &harness, ReadFile, &harness,
                               WriteFile, &harness) == ApplyResult::kRestricted);
  CHECK(harness.Restricted());
}

void SwitchingToOffReleasesImmediately() {
  Harness harness;
  harness.SetCapacity(100);
  harness.SetRestricted(true);
  const LimitSettings off = {ChargeMode::kOff, kDefaultStopPercent,
                             kDefaultResumePercent};
  CHECK(ApplyOnceUnlessStopped("odin2_mini", StockSysfsPaths(), off,
                               StopRequested, &harness, ReadFile, &harness,
                               WriteFile, &harness) == ApplyResult::kReleased);
  CHECK(!harness.Restricted());
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"reaching the stop threshold restricts charging",
       ReachingTheStopThresholdRestrictsCharging},
      {"the current is zeroed before the restriction is enabled",
       TheCurrentIsZeroedBeforeTheRestrictionIsEnabled},
      {"falling below resume releases charging",
       FallingBelowResumeReleasesCharging},
      {"the hysteresis band writes nothing", TheHysteresisBandWritesNothing},
      {"an already correct state writes nothing",
       AnAlreadyCorrectStateWritesNothing},
      {"an unreadable capacity releases the restriction",
       AnUnreadableCapacityReleasesTheRestriction},
      {"a malformed capacity releases the restriction",
       AMalformedCapacityReleasesTheRestriction},
      {"impossible settings release the restriction",
       ImpossibleSettingsReleaseTheRestriction},
      {"an unknown device writes nothing", AnUnknownDeviceWritesNothing},
      {"stopping is honoured before any write",
       StoppingIsHonouredBeforeAnyWrite},
      {"startup clears a restriction left by a killed daemon",
       StartupClearsARestrictionLeftByAKilledDaemon},
      {"releasing an unrestricted charger writes nothing",
       ReleasingAnUnrestrictedChargerWritesNothing},
      {"the loop never exits leaving charging blocked",
       TheLoopNeverExitsLeavingChargingBlocked},
      {"capacity parsing rejects rubbish", CapacityParsingRejectsRubbish},
      {"bypass restricts without consulting the thresholds",
       BypassRestrictsWithoutConsultingTheThresholds},
      {"switching to off releases immediately", SwitchingToOffReleasesImmediately},
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
