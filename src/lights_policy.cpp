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

int ScaleByAlpha(int channel, int alpha) {
  // An alpha of zero from a caller that also sent colour means "no opacity
  // stated", which the framework already treats as fully opaque.
  const int effective_alpha = alpha == 0 ? kChannelMaximum : alpha;
  return (channel * effective_alpha) / kChannelMaximum;
}

int Bound(int channel) {
  return std::clamp(channel, 0, std::min(kChannelMaximum, kHardChannelCeiling));
}

}  // namespace

bool DrivesIndicator(LightType type) {
  return type == LightType::kBattery || type == LightType::kNotifications ||
         type == LightType::kAttention;
}

bool IsKnownLightId(int id) {
  return id >= static_cast<int>(LightType::kBacklight) &&
         id <= static_cast<int>(LightType::kWifi);
}

ChannelPlan Plan(LightType type, unsigned int argb, FlashMode flash_mode,
                 int on_ms, int off_ms) {
  if (!DrivesIndicator(type)) {
    return {false, 0, 0, 0, false, 0, 0};
  }

  const int alpha = static_cast<int>((argb >> 24) & 0xFF);
  const int red = static_cast<int>((argb >> 16) & 0xFF);
  const int green = static_cast<int>((argb >> 8) & 0xFF);
  const int blue = static_cast<int>(argb & 0xFF);

  const int planned_red = Bound(ScaleByAlpha(red, alpha));
  const int planned_green = Bound(ScaleByAlpha(green, alpha));
  const int planned_blue = Bound(ScaleByAlpha(blue, alpha));

  // Blinking with no timings, or with the light already off, is a steady write.
  const bool lit = planned_red > 0 || planned_green > 0 || planned_blue > 0;
  const bool blink =
      flash_mode == FlashMode::kTimed && lit && on_ms > 0 && off_ms > 0;

  return {true,
          planned_red,
          planned_green,
          planned_blue,
          blink,
          blink ? on_ms : 0,
          blink ? off_ms : 0};
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
