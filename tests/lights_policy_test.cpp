// SPDX-License-Identifier: Apache-2.0

#include "ayn/lights_policy.h"

#include <algorithm>
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
using ayn::lights::Channels;
using ayn::lights::Clamp;
using ayn::lights::ClampDecision;
using ayn::lights::IsSupportedDevice;
using ayn::lights::IsValidCap;
using ayn::lights::kChannelMaximum;
using ayn::lights::kDefaultChannelCap;
using ayn::lights::StockSysfsPaths;
using ayn::lights::SysfsPaths;

void FullWhiteIsBroughtDownToTheCap() {
  // The observed failure: a notification drove every channel to 255.
  const ClampDecision decision = Clamp({255, 255, 255}, kDefaultChannelCap);
  CHECK(decision.valid);
  CHECK(decision.write_required);
  CHECK((decision.channels == Channels{1, 1, 1}));
}

void AlreadyDimLeavesTheNodesAlone() {
  // Without this the daemon would answer its own writes forever.
  const ClampDecision decision = Clamp({1, 1, 1}, kDefaultChannelCap);
  CHECK(decision.valid);
  CHECK(!decision.write_required);
  CHECK((decision.channels == Channels{1, 1, 1}));
}

void AnUnlitLedIsNotTouched() {
  const ClampDecision decision = Clamp({0, 0, 0}, kDefaultChannelCap);
  CHECK(decision.valid);
  CHECK(!decision.write_required);
}

void ClampingIsIdempotent() {
  const ClampDecision first = Clamp({255, 128, 0}, 8);
  CHECK(first.write_required);
  const ClampDecision second = Clamp(first.channels, 8);
  CHECK(!second.write_required);
  CHECK(second.channels == first.channels);
}

void HueSurvivesTheClamp() {
  // Clipping each channel independently would turn any bright colour white.
  const ClampDecision decision = Clamp({255, 128, 0}, 8);
  CHECK(decision.channels.red == 8);
  CHECK(decision.channels.green == 4);
  CHECK(decision.channels.blue == 0);
}

void ALitChannelNeverRoundsAwayToOff() {
  // Battery-low red is 0x08,0,0. Scaled to a cap of one it must stay red.
  const ClampDecision decision = Clamp({8, 0, 0}, kDefaultChannelCap);
  CHECK(decision.write_required);
  CHECK((decision.channels == Channels{1, 0, 0}));

  const ClampDecision faint = Clamp({255, 3, 0}, 1);
  CHECK(faint.channels.red == 1);
  CHECK(faint.channels.green == 1);
  CHECK(faint.channels.blue == 0);
}

void UntrustworthyInputFailsClosed() {
  CHECK(!Clamp({256, 0, 0}, 1).valid);
  CHECK(!Clamp({-1, 0, 0}, 1).valid);
  CHECK(!Clamp({255, 255, 255}, 0).valid);
  CHECK(!Clamp({255, 255, 255}, 256).valid);
  CHECK(!Clamp({255, 255, 255}, 0).write_required);
}

void CapBoundsAreEnforced() {
  CHECK(IsValidCap(1));
  CHECK(IsValidCap(kChannelMaximum));
  CHECK(!IsValidCap(0));
  CHECK(!IsValidCap(-1));
  CHECK(!IsValidCap(kChannelMaximum + 1));
}

void UnknownDeviceAndMovedPathsAreRefused() {
  CHECK(IsSupportedDevice("odin2_mini"));
  CHECK(!IsSupportedDevice("odin2"));
  CHECK(!IsSupportedDevice(""));
  CHECK(AreExpectedSysfsPaths(StockSysfsPaths()));

  SysfsPaths moved = StockSysfsPaths();
  moved.blue_brightness = "/data/local/tmp/blue";
  CHECK(!AreExpectedSysfsPaths(moved));
}

void NothingEverExceedsTheCap() {
  for (int red = 0; red <= kChannelMaximum; ++red) {
    for (int cap = 1; cap <= 16; ++cap) {
      const ClampDecision decision = Clamp({red, kChannelMaximum - red, 7}, cap);
      CHECK(decision.valid);
      CHECK(decision.channels.red <= std::max(cap, red));
      CHECK(decision.channels.green <= kChannelMaximum);
      if (decision.write_required) {
        CHECK(decision.channels.red <= cap);
        CHECK(decision.channels.green <= cap);
        CHECK(decision.channels.blue <= cap);
      }
    }
  }
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"full white is brought down to the cap", FullWhiteIsBroughtDownToTheCap},
      {"an already dim LED is left alone", AlreadyDimLeavesTheNodesAlone},
      {"an unlit LED is not touched", AnUnlitLedIsNotTouched},
      {"clamping is idempotent", ClampingIsIdempotent},
      {"hue survives the clamp", HueSurvivesTheClamp},
      {"a lit channel never rounds away to off",
       ALitChannelNeverRoundsAwayToOff},
      {"untrustworthy input fails closed", UntrustworthyInputFailsClosed},
      {"cap bounds are enforced", CapBoundsAreEnforced},
      {"unknown device and moved paths are refused",
       UnknownDeviceAndMovedPathsAreRefused},
      {"nothing ever exceeds the cap", NothingEverExceedsTheCap},
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
