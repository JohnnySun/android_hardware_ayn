// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace ayn::lights {

constexpr int kChannelMaximum = 255;

// Chosen on the device by a person stepping the LED down through 255, 64, 32,
// 16, 8, 4, 3, 2 and 1 and stopping where it stopped being intrusive.
constexpr int kDefaultChannelCap = 1;

// The stock QTI lights HAL owns these nodes and cannot be replaced: the vendor
// partition is the preserved stock Android 13 one, and the odm route is
// forbidden after it hung a boot. It writes only on state change, so clamping
// what it wrote is enough and no resident override of the HAL is needed.
struct SysfsPaths {
  std::string red_brightness;
  std::string green_brightness;
  std::string blue_brightness;
};

struct Channels {
  int red;
  int green;
  int blue;

  bool operator==(const Channels& other) const {
    return red == other.red && green == other.green && blue == other.blue;
  }
};

struct ClampDecision {
  bool valid;
  // False when what is already on the LED is within the cap, so nothing should
  // be written and no write loop can start.
  bool write_required;
  Channels channels;

  bool operator==(const ClampDecision& other) const {
    return valid == other.valid && write_required == other.write_required &&
           channels == other.channels;
  }
};

bool IsValidCap(int cap);
bool AreValidChannels(const Channels& channels);

// Scales the whole triplet by cap/max rather than clipping each channel, so a
// colour keeps its hue instead of collapsing towards white. A channel that was
// lit never rounds down to off.
ClampDecision Clamp(const Channels& observed, int cap);

const SysfsPaths& StockSysfsPaths();
bool IsSupportedDevice(const std::string& product_device);
bool AreExpectedSysfsPaths(const SysfsPaths& paths);

}  // namespace ayn::lights
