// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace ayn::fan {

constexpr int kPwmPeriodNs = 50000;
constexpr int kQuietDutyNs = 5000;
constexpr int kSportDutyNs = 25000;
constexpr int kCustomMinimumDutyNs = 25000;
constexpr int kCustomMaximumDutyNs = 35000;
constexpr int kCustomDutyStepNs = 100;
constexpr int kSmartPollIntervalSeconds = 5;
constexpr int kMinimumTemperatureC = -40;
constexpr int kMaximumTemperatureC = 150;

enum class FanMode {
  kDisabled,
  kQuiet,
  kSport,
  kCustom,
  kSmart,
};

struct FanSettings {
  FanMode mode;
  int custom_duty_ns;
};

struct PolicyResult {
  bool valid;
  int duty_ns;

  bool operator==(const PolicyResult& other) const {
    return valid == other.valid && duty_ns == other.duty_ns;
  }
};

struct SysfsPaths {
  std::string state;
  std::string duty;
  std::string speed;
};

const SysfsPaths& StockSysfsPaths();
bool IsSupportedDevice(const std::string& product_device);
bool AreExpectedSysfsPaths(const SysfsPaths& paths);
PolicyResult ResolveDuty(const FanSettings& settings, int temperature_c);

}  // namespace ayn::fan
