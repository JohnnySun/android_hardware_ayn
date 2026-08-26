// SPDX-License-Identifier: Apache-2.0

#include "ayn/charge_service.h"

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

using ayn::charge::ChargeMode;
using ayn::charge::ChargeService;
using ayn::charge::ParseMode;
using ayn::charge::SerialiseMode;
using ayn::charge::ServiceResponse;
using ayn::charge::ServiceResult;
using ayn::charge::StockSysfsPaths;
using ayn::charge::SysfsPaths;

struct Harness {
  std::map<std::string, std::string> files;
  std::string stored;
  bool has_stored = false;
  bool fail_writes = false;
  int state_writes = 0;

  int capacity = 100;
  bool fail_capacity = false;

  Harness() {
    const SysfsPaths& p = StockSysfsPaths();
    files[p.restrict_chg] = "0\n";
    files[p.restrict_cur] = "1000000\n";
  }
  void SetCapacity(int v) { capacity = v; }
  bool Restricted() const {
    return files.at(StockSysfsPaths().restrict_chg) == "1\n";
  }
};

bool ReadCapacity(void* c, int* capacity_percent) {
  Harness& h = *static_cast<Harness*>(c);
  if (h.fail_capacity) {
    return false;
  }
  *capacity_percent = h.capacity;
  return true;
}

ayn::charge::CapacitySource CapacityFrom(Harness& h) {
  return {ReadCapacity, &h};
}

bool ReadFile(void* c, const std::string& path, std::string* v) {
  Harness& h = *static_cast<Harness*>(c);
  const auto it = h.files.find(path);
  if (it == h.files.end()) return false;
  *v = it->second;
  return true;
}
bool WriteFile(void* c, const std::string& path, const std::string& v) {
  Harness& h = *static_cast<Harness*>(c);
  if (h.fail_writes) return false;
  h.files[path] = v;
  return true;
}
bool ReadState(void* c, std::string* v) {
  Harness& h = *static_cast<Harness*>(c);
  if (!h.has_stored) return false;
  *v = h.stored;
  return true;
}
bool WriteState(void* c, const std::string& v) {
  Harness& h = *static_cast<Harness*>(c);
  h.stored = v;
  h.has_stored = true;
  ++h.state_writes;
  return true;
}

ChargeService Make(Harness& h, const std::string& device = "odin2_mini") {
  return ChargeService(device, StockSysfsPaths(), CapacityFrom(h), ReadFile, &h,
                       WriteFile, &h, ReadState, &h, WriteState, &h);
}

void StartWithoutStoredStateUsesTheLimitDefault() {
  Harness h;
  ChargeService s = Make(h);
  const ServiceResponse r = s.Start();
  CHECK(r.result == ServiceResult::kOk);
  CHECK(r.mode == ChargeMode::kLimit);
  CHECK(h.Restricted());  // capacity 100 is above the stop threshold
}

void StartRestoresTheStoredMode() {
  Harness h;
  h.stored = "off";
  h.has_stored = true;
  ChargeService s = Make(h);
  const ServiceResponse r = s.Start();
  CHECK(r.mode == ChargeMode::kOff);
  CHECK(!h.Restricted());
}

void StartIgnoresRubbishStoredState() {
  Harness h;
  h.stored = "sideways";
  h.has_stored = true;
  ChargeService s = Make(h);
  CHECK(s.Start().mode == ChargeMode::kLimit);
}

void SettingBypassHoldsChargingOffBelowTheThresholds() {
  Harness h;
  h.SetCapacity(50);
  ChargeService s = Make(h);
  s.Start();
  CHECK(!h.Restricted());  // limit mode holds inside the band
  const ServiceResponse r = s.SetMode(ChargeMode::kBypass);
  CHECK(r.result == ServiceResult::kOk);
  CHECK(h.Restricted());
}

void SettingOffReleasesImmediately() {
  Harness h;
  ChargeService s = Make(h);
  s.Start();
  CHECK(h.Restricted());
  CHECK(s.SetMode(ChargeMode::kOff).result == ServiceResult::kOk);
  CHECK(!h.Restricted());
}

void ThresholdsAreSettableAndSurviveARestart() {
  Harness h;
  ChargeService s = Make(h);
  CHECK(s.Start().result == ServiceResult::kOk);
  // What the row shows has to be what the policy uses, or the UI reports a
  // limit the charger is not working to.
  CHECK(s.GetStatus().snapshot.stop_percent == ayn::charge::kDefaultStopPercent);

  CHECK(s.SetThresholds(90, 85).result == ServiceResult::kOk);
  const ServiceResponse after = s.GetStatus();
  CHECK(after.snapshot.stop_percent == 90);
  CHECK(after.snapshot.resume_percent == 85);

  // A fresh service reading the same stored state comes back to the same pair.
  ChargeService restarted = Make(h);
  const ServiceResponse resumed = restarted.Start();
  CHECK(resumed.result == ServiceResult::kOk);
  CHECK(resumed.snapshot.stop_percent == 90);
  CHECK(resumed.snapshot.resume_percent == 85);
}

void AStateFileWrittenBeforeThresholdsExistedStillReads() {
  // The mode on its own, which is every file written until now.
  Harness h;
  h.stored = "bypass\n";
  h.has_stored = true;
  ChargeService s = Make(h);
  const ServiceResponse r = s.Start();
  CHECK(r.result == ServiceResult::kOk);
  CHECK(r.mode == ChargeMode::kBypass);
  CHECK(r.snapshot.stop_percent == ayn::charge::kDefaultStopPercent);
  CHECK(r.snapshot.resume_percent == ayn::charge::kDefaultResumePercent);
}

void ThresholdsThePolicyRefusesNeverReachTheChargerOrTheFile() {
  Harness h;
  ChargeService s = Make(h);
  CHECK(s.Start().result == ServiceResult::kOk);
  const int writes = h.state_writes;

  struct Bad { int stop; int resume; };
  const Bad refused[] = {
      {40, 35},    // below the minimum stop
      {100, 95},   // above the maximum stop
      {80, 79},    // less hysteresis than the band needs
      {80, 30},    // resume under the floor that always releases
      {70, 80},    // resume above stop
  };
  for (const Bad& bad : refused) {
    CHECK(s.SetThresholds(bad.stop, bad.resume).result ==
          ServiceResult::kInvalidMode);
  }
  CHECK(h.state_writes == writes);
  const ServiceResponse unchanged = s.GetStatus();
  CHECK(unchanged.snapshot.stop_percent == ayn::charge::kDefaultStopPercent);
  CHECK(unchanged.snapshot.resume_percent == ayn::charge::kDefaultResumePercent);
}

void AModeIsPersistedOnlyAfterTheChargerAgrees() {
  Harness h;
  ChargeService s = Make(h);
  s.Start();
  const int before = h.state_writes;
  h.fail_writes = true;
  const ServiceResponse r = s.SetMode(ChargeMode::kOff);
  CHECK(r.result != ServiceResult::kOk);
  CHECK(h.state_writes == before);
  // and the rejected mode must not stick
  CHECK(s.GetStatus().mode == ChargeMode::kLimit);
}

void AnUnknownModeIsRefused() {
  Harness h;
  ChargeService s = Make(h);
  s.Start();
  const ServiceResponse r = s.SetMode(static_cast<ChargeMode>(9));
  CHECK(r.result == ServiceResult::kInvalidMode);
  CHECK(s.GetStatus().mode == ChargeMode::kLimit);
}

void AnUnknownDeviceNeverWrites() {
  Harness h;
  ChargeService s = Make(h, "odin2");
  const ServiceResponse r = s.Start();
  CHECK(r.result == ServiceResult::kUnsupportedDevice);
  CHECK(!h.Restricted());
}

void StatusReportsWhatTheChargerActuallyShows() {
  Harness h;
  h.SetCapacity(64);
  ChargeService s = Make(h);
  s.Start();
  const ServiceResponse r = s.GetStatus();
  CHECK(r.snapshot_valid);
  CHECK(r.snapshot.capacity_percent == 64);
  CHECK(r.snapshot.restricted == h.Restricted());
}

void ReleaseLetsGoWhateverTheModeSays() {
  Harness h;
  ChargeService s = Make(h);
  s.Start();
  CHECK(h.Restricted());
  s.Release();
  CHECK(!h.Restricted());
  // the mode itself is untouched, so a restart resumes the owner's choice
  CHECK(s.GetStatus().mode == ChargeMode::kLimit);
}

void ModeStringsRoundTrip() {
  for (const ChargeMode mode :
       {ChargeMode::kOff, ChargeMode::kLimit, ChargeMode::kBypass}) {
    ChargeMode parsed = ChargeMode::kOff;
    CHECK(ParseMode(SerialiseMode(mode), &parsed));
    CHECK(parsed == mode);
  }
  ChargeMode ignored = ChargeMode::kOff;
  CHECK(ParseMode("limit\n", &ignored));
  CHECK(!ParseMode("", &ignored));
  CHECK(!ParseMode("LIMIT", &ignored));
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"start without stored state uses the limit default",
       StartWithoutStoredStateUsesTheLimitDefault},
      {"start restores the stored mode", StartRestoresTheStoredMode},
      {"start ignores rubbish stored state", StartIgnoresRubbishStoredState},
      {"setting bypass holds charging off below the thresholds",
       SettingBypassHoldsChargingOffBelowTheThresholds},
      {"setting off releases immediately", SettingOffReleasesImmediately},
      {"a mode is persisted only after the charger agrees",
       AModeIsPersistedOnlyAfterTheChargerAgrees},
      {"thresholds are settable and survive a restart",
       ThresholdsAreSettableAndSurviveARestart},
      {"a state file written before thresholds existed still reads",
       AStateFileWrittenBeforeThresholdsExistedStillReads},
      {"thresholds the policy refuses never reach the charger or the file",
       ThresholdsThePolicyRefusesNeverReachTheChargerOrTheFile},
      {"an unknown mode is refused", AnUnknownModeIsRefused},
      {"an unknown device never writes", AnUnknownDeviceNeverWrites},
      {"status reports what the charger actually shows",
       StatusReportsWhatTheChargerActuallyShows},
      {"release lets go whatever the mode says", ReleaseLetsGoWhateverTheModeSays},
      {"mode strings round trip", ModeStringsRoundTrip},
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
