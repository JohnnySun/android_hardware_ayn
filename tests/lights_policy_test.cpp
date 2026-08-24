// SPDX-License-Identifier: Apache-2.0

#include "ayn/lights_policy.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}

using ayn::lights::AreExpectedSysfsPaths;
using ayn::lights::ChannelPlan;
using ayn::lights::DrivesIndicator;
using ayn::lights::FlashMode;
using ayn::lights::IsKnownLightId;
using ayn::lights::IsSupportedDevice;
using ayn::lights::kHardChannelCeiling;
using ayn::lights::LightType;
using ayn::lights::Plan;
using ayn::lights::StockSysfsPaths;
using ayn::lights::SysfsPaths;

ChannelPlan Steady(LightType type, unsigned int argb) {
  return Plan(type, argb, FlashMode::kNone, 0, 0);
}

void DeclaredButUndrivenLightsWriteNothing() {
  // The stock HAL declares these and has no code path to any of them. A
  // replacement that started driving them would be a behaviour change, and for
  // the backlight it would be a dangerous one.
  for (const LightType type :
       {LightType::kBacklight, LightType::kKeyboard, LightType::kButtons,
        LightType::kBluetooth, LightType::kWifi}) {
    const ChannelPlan plan = Steady(type, 0xFFFFFFFFu);
    CHECK(!plan.handled);
    CHECK(plan.red == 0 && plan.green == 0 && plan.blue == 0);
    CHECK(!DrivesIndicator(type));
  }
}

void IndicatorLightsAreDriven() {
  for (const LightType type : {LightType::kBattery, LightType::kNotifications,
                               LightType::kAttention}) {
    CHECK(DrivesIndicator(type));
    CHECK(Steady(type, 0xFF010101u).handled);
  }
}

void ColourReachesTheChannelsUnchanged() {
  // This is the whole point of replacing the stock HAL: it discarded the
  // colour and wrote full brightness. A capped colour must survive.
  const ChannelPlan plan = Steady(LightType::kNotifications, 0xFF010101u);
  CHECK(plan.red == 1 && plan.green == 1 && plan.blue == 1);

  const ChannelPlan coloured = Steady(LightType::kBattery, 0xFF080402u);
  CHECK(coloured.red == 8 && coloured.green == 4 && coloured.blue == 2);
}

void AlphaScalesEachChannel() {
  const ChannelPlan half = Steady(LightType::kNotifications, 0x80404040u);
  CHECK(half.red == 32 && half.green == 32 && half.blue == 32);

  // Zero alpha alongside colour means the caller stated no opacity, which the
  // framework already treats as fully opaque.
  const ChannelPlan opaque = Steady(LightType::kNotifications, 0x00202020u);
  CHECK(opaque.red == 32 && opaque.green == 32 && opaque.blue == 32);
}

void TheHardCeilingBoundsAHostileCaller() {
  const ChannelPlan plan = Steady(LightType::kAttention, 0xFFFFFFFFu);
  CHECK(plan.red == kHardChannelCeiling);
  CHECK(plan.green == kHardChannelCeiling);
  CHECK(plan.blue == kHardChannelCeiling);
}

void BlackTurnsTheLightOffWithoutBlinking() {
  const ChannelPlan plan = Plan(LightType::kNotifications, 0x00000000u,
                                FlashMode::kTimed, 500, 500);
  CHECK(plan.handled);
  CHECK(plan.red == 0 && plan.green == 0 && plan.blue == 0);
  CHECK(!plan.blink);
  CHECK(plan.delay_on_ms == 0 && plan.delay_off_ms == 0);
}

void BlinkNeedsTimedModeAndBothTimings() {
  const ChannelPlan timed =
      Plan(LightType::kNotifications, 0xFF080808u, FlashMode::kTimed, 300, 700);
  CHECK(timed.blink);
  CHECK(timed.delay_on_ms == 300 && timed.delay_off_ms == 700);

  CHECK(!Plan(LightType::kNotifications, 0xFF080808u, FlashMode::kTimed, 300, 0)
             .blink);
  CHECK(!Plan(LightType::kNotifications, 0xFF080808u, FlashMode::kTimed, 0, 700)
             .blink);
  CHECK(!Plan(LightType::kNotifications, 0xFF080808u, FlashMode::kNone, 300, 700)
             .blink);
  CHECK(!Plan(LightType::kNotifications, 0xFF080808u, FlashMode::kHardware, 300,
              700)
             .blink);
}

void OnlyTheDeclaredLightIdsExist() {
  CHECK(IsKnownLightId(0));
  CHECK(IsKnownLightId(7));
  CHECK(!IsKnownLightId(-1));
  CHECK(!IsKnownLightId(8));
}

void UnknownDeviceAndMovedPathsAreRefused() {
  CHECK(IsSupportedDevice("odin2_mini"));
  CHECK(!IsSupportedDevice("odin2"));
  CHECK(!IsSupportedDevice(""));
  CHECK(AreExpectedSysfsPaths(StockSysfsPaths()));

  SysfsPaths moved = StockSysfsPaths();
  moved.green_brightness = "/data/local/tmp/green";
  CHECK(!AreExpectedSysfsPaths(moved));
}

void NoColourEverExceedsTheCeiling() {
  for (unsigned int channel = 0; channel <= 255; ++channel) {
    const unsigned int argb =
        0xFF000000u | (channel << 16) | (channel << 8) | channel;
    const ChannelPlan plan = Steady(LightType::kNotifications, argb);
    CHECK(plan.red <= kHardChannelCeiling);
    CHECK(plan.green <= kHardChannelCeiling);
    CHECK(plan.blue <= kHardChannelCeiling);
    CHECK(plan.red >= 0 && plan.green >= 0 && plan.blue >= 0);
  }
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"declared but undriven lights write nothing",
       DeclaredButUndrivenLightsWriteNothing},
      {"indicator lights are driven", IndicatorLightsAreDriven},
      {"colour reaches the channels unchanged",
       ColourReachesTheChannelsUnchanged},
      {"alpha scales each channel", AlphaScalesEachChannel},
      {"the hard ceiling bounds a hostile caller",
       TheHardCeilingBoundsAHostileCaller},
      {"black turns the light off without blinking",
       BlackTurnsTheLightOffWithoutBlinking},
      {"blink needs timed mode and both timings",
       BlinkNeedsTimedModeAndBothTimings},
      {"only the declared light ids exist", OnlyTheDeclaredLightIdsExist},
      {"unknown device and moved paths are refused",
       UnknownDeviceAndMovedPathsAreRefused},
      {"no colour ever exceeds the ceiling", NoColourEverExceedsTheCeiling},
  };

  size_t passed = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      ++passed;
      std::cout << "[PASS] " << test.first << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n';
      return 1;
    }
  }
  std::cout << passed << " tests passed\n";
  return 0;
}
