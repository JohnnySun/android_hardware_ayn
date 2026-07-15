// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace ayn::fan {

constexpr int kPwmPeriodNs = 50000;
constexpr int kPwmPeriod = 50000;
constexpr int kOffDuty = 10000;
constexpr int kSafeMaximumDutyNs = 25000;
constexpr int kCustomMinimumDutyNs = 25000;
constexpr int kCustomMaximumDutyNs = 35000;
constexpr int kCustomDutyStepNs = 100;
constexpr int kAutomaticPollIntervalSeconds = 5;
constexpr int kSmartPollIntervalSeconds = kAutomaticPollIntervalSeconds;
constexpr int kMinimumTemperatureC = -40;
constexpr int kMaximumTemperatureC = 150;
constexpr int kHighTemperatureC = 85;
constexpr int kCurveHysteresisC = 2;
constexpr int kCurveDebounceSamples = 2;

enum class FanMode {
  kOff,
  kDisabled = kOff,
  kQuiet,
  kSport,
  kPerformance = kSport,
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

struct CurveDecision {
  bool valid;
  bool apply;
  int duty_ns;

  bool operator==(const CurveDecision& other) const {
    return valid == other.valid && apply == other.apply &&
           duty_ns == other.duty_ns;
  }
};

class FanCurveController {
 public:
  CurveDecision Observe(FanMode mode, int temperature_c);
  void Reset();

 private:
  bool initialized_ = false;
  FanMode mode_ = FanMode::kOff;
  int accepted_temperature_c_ = 0;
  int duty_ns_ = 0;
  int pending_direction_ = 0;
  int pending_samples_ = 0;
};

struct SysfsPaths {
  std::string state;
  std::string duty;
  std::string period;
  std::string speed;
};

const SysfsPaths& StockSysfsPaths();
bool IsSupportedDevice(const std::string& product_device);
bool AreExpectedSysfsPaths(const SysfsPaths& paths);
PolicyResult ResolveDuty(const FanSettings& settings, int temperature_c);

}  // namespace ayn::fan
