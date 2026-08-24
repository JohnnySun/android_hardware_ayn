// SPDX-License-Identifier: Apache-2.0
#include "ayn/lights_policy.h"

#include <algorithm>

namespace ayn::lights {
namespace {

const SysfsPaths& StockPaths() {
  static const SysfsPaths paths = {
      "/sys/class/leds/red/brightness",
      "/sys/class/leds/green/brightness",
      "/sys/class/leds/blue/brightness",
  };
  return paths;
}

int ScaleChannel(int channel, int cap, int peak) {
  if (channel <= 0) {
    return 0;
  }
  const int scaled = (channel * cap) / peak;
  // A channel that carried colour must not disappear, or a dim red becomes an
  // unlit LED rather than a dimmer red.
  return std::max(scaled, 1);
}

}  // namespace

bool IsValidCap(int cap) { return cap >= 1 && cap <= kChannelMaximum; }

bool AreValidChannels(const Channels& channels) {
  const auto in_range = [](int value) {
    return value >= 0 && value <= kChannelMaximum;
  };
  return in_range(channels.red) && in_range(channels.green) &&
         in_range(channels.blue);
}

ClampDecision Clamp(const Channels& observed, int cap) {
  if (!IsValidCap(cap) || !AreValidChannels(observed)) {
    return {false, false, {0, 0, 0}};
  }

  const int peak = std::max({observed.red, observed.green, observed.blue});
  if (peak <= cap) {
    return {true, false, observed};
  }

  const Channels clamped = {
      ScaleChannel(observed.red, cap, peak),
      ScaleChannel(observed.green, cap, peak),
      ScaleChannel(observed.blue, cap, peak),
  };
  return {true, true, clamped};
}

const SysfsPaths& StockSysfsPaths() { return StockPaths(); }

bool IsSupportedDevice(const std::string& product_device) {
  return product_device == "odin2_mini";
}

bool AreExpectedSysfsPaths(const SysfsPaths& paths) {
  const SysfsPaths& expected = StockPaths();
  return paths.red_brightness == expected.red_brightness &&
         paths.green_brightness == expected.green_brightness &&
         paths.blue_brightness == expected.blue_brightness;
}

}  // namespace ayn::lights
