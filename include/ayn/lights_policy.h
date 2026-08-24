// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace ayn::lights {

// android.hardware.light.LightType, in the order the AIDL enum declares them.
enum class LightType {
  kBacklight = 0,
  kKeyboard = 1,
  kButtons = 2,
  kBattery = 3,
  kNotifications = 4,
  kAttention = 5,
  kBluetooth = 6,
  kWifi = 7,
};

// android.hardware.light.FlashMode.
enum class FlashMode {
  kNone = 0,
  kTimed = 1,
  kHardware = 2,
};

constexpr int kChannelMaximum = 255;

// A backstop, not the policy. How dim the indicator should be is decided by
// config_indicatorLightBrightnessCap in the framework, which already caps the
// colour before it reaches this HAL. This ceiling only bounds what a broken or
// hostile caller can do to the panel-adjacent LED.
constexpr int kHardChannelCeiling = 64;

struct ChannelPlan {
  // False means this light is not ours to drive and nothing must be written.
  bool handled;
  int red;
  int green;
  int blue;
  bool blink;
  int delay_on_ms;
  int delay_off_ms;

  bool operator==(const ChannelPlan& other) const {
    return handled == other.handled && red == other.red &&
           green == other.green && blue == other.blue && blink == other.blink &&
           delay_on_ms == other.delay_on_ms &&
           delay_off_ms == other.delay_off_ms;
  }
};

struct SysfsPaths {
  std::string red_brightness;
  std::string green_brightness;
  std::string blue_brightness;
};

// The stock HAL declares eight lights but only ever drives the tri-colour LED.
// Everything else is declared and ignored, including the backlight, which it
// has no code path to. Reproducing that exactly is what makes this a safe
// replacement.
bool DrivesIndicator(LightType type);
bool IsKnownLightId(int id);

// Decodes ARGB, scales each channel by alpha the way a faithful lights HAL
// should, and bounds the result. Returns an unhandled plan for lights this HAL
// declares but does not drive.
ChannelPlan Plan(LightType type, unsigned int argb, FlashMode flash_mode,
                 int on_ms, int off_ms);

const SysfsPaths& StockSysfsPaths();
bool IsSupportedDevice(const std::string& product_device);
bool AreExpectedSysfsPaths(const SysfsPaths& paths);

}  // namespace ayn::lights
