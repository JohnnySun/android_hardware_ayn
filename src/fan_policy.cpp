// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_policy.h"

#include <algorithm>

namespace ayn::fan {

namespace {

constexpr char kProductDevice[] = "odin2_mini";
constexpr char kStatePath[] = "/sys/class/gpio5_pwm2/state";
constexpr char kDutyPath[] = "/sys/class/gpio5_pwm2/duty";
constexpr char kSpeedPath[] = "/sys/class/gpio5_pwm2/speed";

int SmartDutyForTemperature(int temperature_c) {
  if (temperature_c < 5) {
    return 0;
  }
  if (temperature_c < 40) {
    return 8000;
  }
  if (temperature_c < 85) {
    return ((151 * temperature_c - 2800) * 250) / 100;
  }
  return kSportDutyNs;
}

}  // namespace

bool IsSupportedDevice(const std::string& product_device) {
  return product_device == kProductDevice;
}

bool AreExpectedSysfsPaths(const SysfsPaths& paths) {
  return paths.state == kStatePath && paths.duty == kDutyPath &&
         paths.speed == kSpeedPath;
}

PolicyResult ResolveDuty(const FanSettings& settings, int temperature_c) {
  if (settings.mode != FanMode::kCustom && settings.custom_duty_ns != 0) {
    return {false, 0};
  }

  switch (settings.mode) {
    case FanMode::kDisabled:
      return {true, 0};
    case FanMode::kQuiet:
      return {true, kQuietDutyNs};
    case FanMode::kSport:
      return {true, kSportDutyNs};
    case FanMode::kCustom: {
      const int clamped =
          std::clamp(settings.custom_duty_ns, kCustomMinimumDutyNs,
                     kCustomMaximumDutyNs);
      const int stepped = kCustomMinimumDutyNs +
                          ((clamped - kCustomMinimumDutyNs) /
                           kCustomDutyStepNs) *
                              kCustomDutyStepNs;
      return {true, stepped};
    }
    case FanMode::kSmart:
      return {true, SmartDutyForTemperature(temperature_c)};
  }
  return {false, 0};
}

}  // namespace ayn::fan
