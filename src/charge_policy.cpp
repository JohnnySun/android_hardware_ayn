// SPDX-License-Identifier: Apache-2.0
#include "ayn/charge_policy.h"

namespace ayn::charge {
namespace {

const SysfsPaths& StockPaths() {
  static const SysfsPaths paths = {
      "/sys/class/power_supply/battery/capacity",
      "/sys/class/qcom-battery/restrict_chg",
      "/sys/class/qcom-battery/restrict_cur",
  };
  return paths;
}

}  // namespace

bool IsValidCapacity(int capacity_percent) {
  return capacity_percent >= 0 && capacity_percent <= 100;
}

bool AreValidSettings(const LimitSettings& settings) {
  if (!settings.enabled) {
    // A disabled policy carries no thresholds worth checking; the only action
    // it can produce is releasing the restriction.
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
  if (!settings.enabled) {
    return {true, ChargeAction::kAllow};
  }
  if (capacity_percent < kNeverRestrictBelowPercent) {
    return {true, ChargeAction::kAllow};
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
  return paths.capacity == expected.capacity &&
         paths.restrict_chg == expected.restrict_chg &&
         paths.restrict_cur == expected.restrict_cur;
}

}  // namespace ayn::charge
