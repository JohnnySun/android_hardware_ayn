// SPDX-License-Identifier: Apache-2.0
#include "ayn/charge_policy.h"

namespace ayn::charge {
namespace {

const SysfsPaths& StockPaths() {
  static const SysfsPaths paths = {
      "/sys/class/qcom-battery/restrict_chg",
      "/sys/class/qcom-battery/restrict_cur",
  };
  return paths;
}

}  // namespace

bool IsValidCapacity(int capacity_percent) {
  return capacity_percent >= 0 && capacity_percent <= 100;
}

bool IsKnownMode(int mode) {
  return mode == static_cast<int>(ChargeMode::kOff) ||
         mode == static_cast<int>(ChargeMode::kLimit) ||
         mode == static_cast<int>(ChargeMode::kBypass);
}

bool AreValidSettings(const LimitSettings& settings) {
  if (!IsKnownMode(static_cast<int>(settings.mode))) {
    return false;
  }
  if (settings.mode != ChargeMode::kLimit) {
    // Off and bypass carry no thresholds worth checking. Off can only release,
    // and bypass answers from the safety floor alone.
    return true;
  }
  if (settings.stop_percent < kMinimumStopPercent ||
      settings.stop_percent > kMaximumStopPercent) {
    return false;
  }
  if (settings.resume_percent < kNeverRestrictBelowPercent) {
    return false;
  }
  if (settings.resume_percent >= settings.stop_percent) {
    return false;
  }
  return settings.stop_percent - settings.resume_percent >=
         kMinimumHysteresisPercent;
}

PolicyDecision Decide(const LimitSettings& settings, int capacity_percent) {
  if (!AreValidSettings(settings) || !IsValidCapacity(capacity_percent)) {
    return {false, ChargeAction::kInvalid};
  }
  if (settings.mode == ChargeMode::kOff) {
    return {true, ChargeAction::kAllow};
  }
  // The floor outranks every mode, including bypass.
  if (capacity_percent < kNeverRestrictBelowPercent) {
    return {true, ChargeAction::kAllow};
  }
  if (settings.mode == ChargeMode::kBypass) {
    return {true, ChargeAction::kRestrict};
  }
  if (capacity_percent <= settings.resume_percent) {
    return {true, ChargeAction::kAllow};
  }
  if (capacity_percent >= settings.stop_percent) {
    return {true, ChargeAction::kRestrict};
  }
  return {true, ChargeAction::kHold};
}

const SysfsPaths& StockSysfsPaths() { return StockPaths(); }

bool IsSupportedDevice(const std::string& product_device) {
  return product_device == "odin2_mini";
}

bool AreExpectedSysfsPaths(const SysfsPaths& paths) {
  const SysfsPaths& expected = StockPaths();
  return paths.restrict_chg == expected.restrict_chg &&
         paths.restrict_cur == expected.restrict_cur;
}

}  // namespace ayn::charge
