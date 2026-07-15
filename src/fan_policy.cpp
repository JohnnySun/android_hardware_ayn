// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_policy.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace ayn::fan {

namespace {

constexpr char kProductDevice[] = "odin2_mini";
constexpr char kStatePath[] = "/sys/class/gpio5_pwm2/state";
constexpr char kDutyPath[] = "/sys/class/gpio5_pwm2/duty";
constexpr char kPeriodPath[] = "/sys/class/gpio5_pwm2/period";
constexpr char kSpeedPath[] = "/sys/class/gpio5_pwm2/speed";

struct CurvePoint {
  int temperature_c;
  int duty_ns;
};

constexpr std::array<CurvePoint, 6> kQuietCurve = {{
    {40, 5000},
    {50, 8000},
    {60, 12000},
    {70, 17000},
    {80, 22000},
    {85, kSafeMaximumDutyNs},
}};

constexpr std::array<CurvePoint, 5> kSportCurve = {{
    {35, 8000},
    {45, 12000},
    {55, 17000},
    {65, 21000},
    {75, kSafeMaximumDutyNs},
}};

template <size_t Size>
int InterpolateCurve(const std::array<CurvePoint, Size>& curve,
                     int temperature_c) {
  if (temperature_c <= curve.front().temperature_c) {
    return curve.front().duty_ns;
  }
  for (size_t index = 1; index < curve.size(); ++index) {
    const CurvePoint& upper = curve[index];
    if (temperature_c <= upper.temperature_c) {
      const CurvePoint& lower = curve[index - 1];
      const int temperature_span =
          upper.temperature_c - lower.temperature_c;
      const int temperature_offset =
          temperature_c - lower.temperature_c;
      const int duty_span = upper.duty_ns - lower.duty_ns;
      return lower.duty_ns +
             (duty_span * temperature_offset) / temperature_span;
    }
  }
  return curve.back().duty_ns;
}

bool IsCurveMode(FanMode mode) {
  return mode == FanMode::kQuiet || mode == FanMode::kSport;
}

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
  return kSafeMaximumDutyNs;
}

}  // namespace

const SysfsPaths& StockSysfsPaths() {
  static const SysfsPaths paths = {kStatePath, kDutyPath, kPeriodPath,
                                   kSpeedPath};
  return paths;
}

bool IsSupportedDevice(const std::string& product_device) {
  return product_device == kProductDevice;
}

bool AreExpectedSysfsPaths(const SysfsPaths& paths) {
  const SysfsPaths& stock = StockSysfsPaths();
  return paths.state == stock.state && paths.duty == stock.duty &&
         paths.period == stock.period && paths.speed == stock.speed;
}

PolicyResult ResolveDuty(const FanSettings& settings, int temperature_c) {
  if (settings.mode != FanMode::kCustom && settings.custom_duty_ns != 0) {
    return {false, 0};
  }

  switch (settings.mode) {
    case FanMode::kDisabled:
      return {true, 0};
    case FanMode::kQuiet:
      if (temperature_c < kMinimumTemperatureC ||
          temperature_c > kMaximumTemperatureC) {
        return {false, 0};
      }
      return {true, std::clamp(InterpolateCurve(kQuietCurve, temperature_c),
                               0, kSafeMaximumDutyNs)};
    case FanMode::kSport:
      if (temperature_c < kMinimumTemperatureC ||
          temperature_c > kMaximumTemperatureC) {
        return {false, 0};
      }
      return {true, std::clamp(InterpolateCurve(kSportCurve, temperature_c),
                               0, kSafeMaximumDutyNs)};
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
      if (temperature_c < kMinimumTemperatureC ||
          temperature_c > kMaximumTemperatureC) {
        return {false, 0};
      }
      return {true, SmartDutyForTemperature(temperature_c)};
  }
  return {false, 0};
}

CurveDecision FanCurveController::Observe(FanMode mode, int temperature_c) {
  if (!IsCurveMode(mode)) {
    return {false, false, 0};
  }
  const PolicyResult policy = ResolveDuty({mode, 0}, temperature_c);
  if (!policy.valid) {
    return {false, false, 0};
  }

  if (!initialized_ || mode_ != mode) {
    Reset();
    initialized_ = true;
    mode_ = mode;
    accepted_temperature_c_ = temperature_c;
    duty_ns_ = policy.duty_ns;
    return {true, true, duty_ns_};
  }

  if (temperature_c >= kHighTemperatureC) {
    accepted_temperature_c_ = temperature_c;
    pending_direction_ = 0;
    pending_samples_ = 0;
    const bool changed = duty_ns_ != kSafeMaximumDutyNs;
    duty_ns_ = kSafeMaximumDutyNs;
    return {true, changed, duty_ns_};
  }

  const int delta = temperature_c - accepted_temperature_c_;
  if (std::abs(delta) < kCurveHysteresisC) {
    pending_direction_ = 0;
    pending_samples_ = 0;
    return {true, false, duty_ns_};
  }

  const int direction = delta > 0 ? 1 : -1;
  if (direction != pending_direction_) {
    pending_direction_ = direction;
    pending_samples_ = 1;
  } else {
    ++pending_samples_;
  }
  if (pending_samples_ < kCurveDebounceSamples) {
    return {true, false, duty_ns_};
  }

  accepted_temperature_c_ = temperature_c;
  pending_direction_ = 0;
  pending_samples_ = 0;
  const bool changed = duty_ns_ != policy.duty_ns;
  duty_ns_ = policy.duty_ns;
  return {true, changed, duty_ns_};
}

void FanCurveController::Reset() {
  initialized_ = false;
  mode_ = FanMode::kOff;
  accepted_temperature_c_ = 0;
  duty_ns_ = 0;
  pending_direction_ = 0;
  pending_samples_ = 0;
}

}  // namespace ayn::fan
