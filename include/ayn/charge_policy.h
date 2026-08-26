// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace ayn::charge {

// The charger firmware clamps the fast-charge current to restrict_cur while
// restrict_chg is set. A restricted current of zero stops charging outright
// while the adapter keeps powering the device.
constexpr int kRestrictedCurrentUa = 0;

constexpr int kDefaultStopPercent = 80;
constexpr int kDefaultResumePercent = 75;

// A limit below this is not a battery-longevity policy, it is a way to make the
// device unusable off the charger. A limit above this is not a limit.
constexpr int kMinimumStopPercent = 50;
constexpr int kMaximumStopPercent = 99;

// Without a band the policy would toggle the charger on every sample near the
// threshold.
constexpr int kMinimumHysteresisPercent = 2;

// Never hold charging off a battery that is already low, whatever the mode or
// thresholds say. This bounds the damage from a bad stored value, and it is
// what stops bypass from running the pack flat when the load outruns the
// adapter.
constexpr int kNeverRestrictBelowPercent = 40;

enum class ChargeMode {
  // Charge normally. The daemon holds no restriction.
  kOff = 0,
  // Hold the battery between the resume and stop thresholds.
  kLimit = 1,
  // Keep charging off for as long as the adapter is attached, whatever the
  // capacity, so the pack neither charges nor carries the load. This is not a
  // hardware bypass: it sets the charge current to zero, so if the system draws
  // more than the adapter supplies the battery still makes up the difference.
  kBypass = 2,
};

struct LimitSettings {
  ChargeMode mode;
  int stop_percent;
  int resume_percent;
};

bool IsKnownMode(int mode);

enum class ChargeAction {
  // Inputs could not be trusted. The caller must release the restriction.
  kInvalid,
  // Inside the hysteresis band: keep whatever the charger is already doing.
  kHold,
  // Stop charging.
  kRestrict,
  // Permit charging.
  kAllow,
};

struct PolicyDecision {
  bool valid;
  ChargeAction action;

  bool operator==(const PolicyDecision& other) const {
    return valid == other.valid && action == other.action;
  }
};

// Capacity is deliberately absent. It does not come from sysfs any more; see
// CapacitySource in charge_lifecycle.h for why.
struct SysfsPaths {
  std::string restrict_chg;
  std::string restrict_cur;
};

bool AreValidSettings(const LimitSettings& settings);
bool IsValidCapacity(int capacity_percent);

// Inside the hysteresis band the answer is kHold, so the caller leaves the
// charger alone rather than rewriting the same value every sample. At startup
// that means the kernel's own post-boot state stands, which is unrestricted.
PolicyDecision Decide(const LimitSettings& settings, int capacity_percent);

const SysfsPaths& StockSysfsPaths();
bool IsSupportedDevice(const std::string& product_device);
bool AreExpectedSysfsPaths(const SysfsPaths& paths);

}  // namespace ayn::charge
