// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_adapter.h"

#include <fcntl.h>
#include <unistd.h>

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

using ayn::fan::AdapterResult;
using ayn::fan::FanMode;
using ayn::fan::FanSettings;
using ayn::fan::SysfsPaths;

const SysfsPaths kExpectedPaths = {
    "/sys/class/gpio5_pwm2/state",
    "/sys/class/gpio5_pwm2/duty",
    "/sys/class/gpio5_pwm2/period",
    "/sys/class/gpio5_pwm2/speed",
};

struct Harness {
  bool stop_requested = false;
  bool settings_valid = true;
  FanSettings settings = {FanMode::kQuiet, 0};
  bool temperature_valid = true;
  int temperature_c = 40;
  size_t settings_reads = 0;
  size_t temperature_reads = 0;
  size_t sysfs_reads = 0;
  bool writes_valid = true;
  std::map<std::string, std::string> files = {
      {kExpectedPaths.state, "0\n"},
      {kExpectedPaths.duty, "0\n"},
      {kExpectedPaths.period, "50000\n"},
      {kExpectedPaths.speed, "0\n"},
  };
  std::vector<std::pair<std::string, std::string>> writes;
};

bool StopRequested(void* context) {
  return static_cast<Harness*>(context)->stop_requested;
}

bool ReadSettings(void* context, FanSettings* settings) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->settings_reads;
  if (!harness->settings_valid) {
    return false;
  }
  *settings = harness->settings;
  return true;
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

bool ReadFile(void* context, const std::string& path, std::string* value) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->sysfs_reads;
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
  if (!harness->writes_valid) {
    return false;
  }
  harness->files[path] = value + "\n";
  return true;
}

AdapterResult Apply(Harness* harness,
                    const std::string& product_device = "odin2_mini",
                    const SysfsPaths& paths = kExpectedPaths) {
  return ayn::fan::ApplyCurrentSettingsUnlessStopped(
      product_device, paths, StopRequested, harness, ReadSettings, harness,
      ReadTemperature, harness, ReadFile, harness, WriteFile, harness);
}

void UnknownDeviceAndWrongPathsCannotReachAdapters() {
  CHECK(ayn::fan::AreExpectedSysfsPaths(ayn::fan::StockSysfsPaths()));

  Harness unknown;
  CHECK(Apply(&unknown, "kalama") == AdapterResult::kFailedClosed);
  CHECK(unknown.settings_reads == 0);
  CHECK(unknown.sysfs_reads == 0);
  CHECK(unknown.writes.empty());

  Harness wrong_path;
  SysfsPaths paths = kExpectedPaths;
  paths.duty += ".bak";
  CHECK(Apply(&wrong_path, "odin2_mini", paths) ==
        AdapterResult::kFailedClosed);
  CHECK(wrong_path.settings_reads == 0);
  CHECK(wrong_path.sysfs_reads == 0);
  CHECK(wrong_path.writes.empty());
}

void MissingOrInvalidSettingsCannotReachSysfs() {
  Harness missing;
  missing.settings_valid = false;
  CHECK(Apply(&missing) == AdapterResult::kFailedClosed);
  CHECK(missing.settings_reads == 1);
  CHECK(missing.sysfs_reads == 0);
  CHECK(missing.writes.empty());

  for (const FanSettings settings : {
           FanSettings{static_cast<FanMode>(99), 0},
           FanSettings{FanMode::kQuiet, 1},
           FanSettings{FanMode::kCustom,
                       ayn::fan::kCustomMinimumDutyNs - 1},
           FanSettings{FanMode::kCustom,
                       ayn::fan::kCustomMaximumDutyNs + 1},
       }) {
    Harness invalid;
    invalid.settings = settings;
    CHECK(Apply(&invalid) == AdapterResult::kFailedClosed);
    CHECK(invalid.sysfs_reads == 0);
    CHECK(invalid.writes.empty());
  }
}

void AutomaticSettingsReadTemperatureBeforeSysfsWrites() {
  Harness quiet;
  CHECK(Apply(&quiet) == AdapterResult::kApplied);
  CHECK(quiet.settings_reads == 1);
  CHECK(quiet.temperature_reads == 1);
  CHECK(quiet.writes.back() ==
        std::make_pair(kExpectedPaths.state, std::string("1")));
  CHECK(quiet.files[kExpectedPaths.duty] == "5000\n");

  Harness sport;
  sport.settings = {FanMode::kSport, 0};
  sport.temperature_c = 60;
  CHECK(Apply(&sport) == AdapterResult::kApplied);
  CHECK(sport.temperature_reads == 1);
  CHECK(sport.files[kExpectedPaths.duty] == "19000\n");

  Harness smart;
  smart.settings = {FanMode::kSmart, 0};
  CHECK(Apply(&smart) == AdapterResult::kApplied);
  CHECK(smart.temperature_reads == 1);
  CHECK(smart.files[kExpectedPaths.duty] == "8100\n");
}

void CustomSettingRemainsManualAndSkipsTemperature() {
  Harness custom;
  custom.settings = {FanMode::kCustom, 25100};
  custom.temperature_valid = false;
  CHECK(Apply(&custom) == AdapterResult::kApplied);
  CHECK(custom.temperature_reads == 0);
  CHECK(custom.files[kExpectedPaths.duty] == "25100\n");
}

void TemperatureFailureCannotReachSysfs() {
  for (FanMode mode : {FanMode::kQuiet, FanMode::kSport, FanMode::kSmart}) {
    Harness harness;
    harness.settings = {mode, 0};
    harness.temperature_valid = false;
    CHECK(Apply(&harness) == AdapterResult::kFailedClosed);
    CHECK(harness.temperature_reads == 1);
    CHECK(harness.sysfs_reads == 0);
    CHECK(harness.writes.empty());
  }

  for (int temperature_c : {ayn::fan::kMinimumTemperatureC - 1,
                            ayn::fan::kMaximumTemperatureC + 1}) {
    Harness out_of_range;
    out_of_range.settings = {FanMode::kSmart, 0};
    out_of_range.temperature_c = temperature_c;
    CHECK(Apply(&out_of_range) == AdapterResult::kFailedClosed);
    CHECK(out_of_range.sysfs_reads == 0);
    CHECK(out_of_range.writes.empty());
  }
}

void CpuTemperatureReaderFindsOneNamedZoneAndNormalizesUnits() {
  const std::string zone0 = "/sys/class/thermal/thermal_zone0/";
  const std::string zone47 = "/sys/class/thermal/thermal_zone47/";

  Harness millidegrees;
  millidegrees.files[zone0 + "type"] = "pa\n";
  millidegrees.files[zone47 + "type"] = "cpu-0-0\n";
  millidegrees.files[zone47 + "temp"] = "52750\n";
  int temperature_c = 0;
  CHECK(ayn::fan::ReadCpuTemperatureFromZones(ReadFile, &millidegrees,
                                               &temperature_c));
  CHECK(temperature_c == 52);

  Harness degrees;
  degrees.files[zone47 + "type"] = "cpu-0-0\n";
  degrees.files[zone47 + "temp"] = "61\n";
  CHECK(ayn::fan::ReadCpuTemperatureFromZones(ReadFile, &degrees,
                                               &temperature_c));
  CHECK(temperature_c == 61);
}

void CpuTemperatureReaderFailsClosedOnAmbiguousOrInvalidInput() {
  const std::string zone47 = "/sys/class/thermal/thermal_zone47/";
  const std::string zone48 = "/sys/class/thermal/thermal_zone48/";
  int temperature_c = 0;

  Harness duplicate;
  duplicate.files[zone47 + "type"] = "cpu-0-0\n";
  duplicate.files[zone47 + "temp"] = "52000\n";
  duplicate.files[zone48 + "type"] = "cpu-0-0\n";
  duplicate.files[zone48 + "temp"] = "53000\n";
  CHECK(!ayn::fan::ReadCpuTemperatureFromZones(ReadFile, &duplicate,
                                                &temperature_c));

  for (const std::string& raw : {"", "hot\n", "151\n", "151000\n"}) {
    Harness invalid;
    invalid.files[zone47 + "type"] = "cpu-0-0\n";
    invalid.files[zone47 + "temp"] = raw;
    CHECK(!ayn::fan::ReadCpuTemperatureFromZones(ReadFile, &invalid,
                                                  &temperature_c));
  }
}

void UnconfirmedDisablePropagatesAcrossTheAdapterBoundary() {
  Harness harness;
  harness.files[kExpectedPaths.state] = "1\n";
  harness.writes_valid = false;
  CHECK(Apply(&harness) == AdapterResult::kDisableUnconfirmed);
}

void PosixFileAdapterRoundTripsWithoutTouchingSysfs() {
  char path[] = "/tmp/ayn-fan-adapter.XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  CHECK(close(fd) == 0);

  CHECK(ayn::fan::WritePosixFile(nullptr, path, "25000"));
  std::string value;
  CHECK(ayn::fan::ReadPosixFile(nullptr, path, &value));
  CHECK(value == "25000");
  CHECK(unlink(path) == 0);

  CHECK(!ayn::fan::ReadPosixFile(nullptr, path, &value));
  CHECK(!ayn::fan::WritePosixFile(nullptr, path, "1"));
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"unknown device and wrong paths cannot reach adapters",
       UnknownDeviceAndWrongPathsCannotReachAdapters},
      {"missing or invalid settings cannot reach sysfs",
       MissingOrInvalidSettingsCannotReachSysfs},
      {"automatic settings read temperature before sysfs writes",
       AutomaticSettingsReadTemperatureBeforeSysfsWrites},
      {"custom setting remains manual and skips temperature",
       CustomSettingRemainsManualAndSkipsTemperature},
      {"temperature failure cannot reach sysfs",
       TemperatureFailureCannotReachSysfs},
      {"CPU temperature reader finds one named zone and normalizes units",
       CpuTemperatureReaderFindsOneNamedZoneAndNormalizesUnits},
      {"CPU temperature reader fails closed on ambiguous or invalid input",
       CpuTemperatureReaderFailsClosedOnAmbiguousOrInvalidInput},
      {"unconfirmed disable propagates across the adapter boundary",
       UnconfirmedDisablePropagatesAcrossTheAdapterBoundary},
      {"POSIX file adapter round trips without touching sysfs",
       PosixFileAdapterRoundTripsWithoutTouchingSysfs},
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
