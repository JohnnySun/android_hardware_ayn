// SPDX-License-Identifier: Apache-2.0
#include "ayn/rsinput_combo.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using ayn::rsinput::ComboDetector;
using ayn::rsinput::kOverlayComboMask;

constexpr uint16_t kSelect = 1u << 10;
constexpr uint16_t kR1 = 1u << 9;
constexpr uint16_t kSouth = 1u << 7;

int g_failures = 0;

void Check(bool value, const std::string& what) {
  if (value) {
    std::cout << "[PASS] " << what << "\n";
  } else {
    std::cout << "[FAIL] " << what << "\n";
    ++g_failures;
  }
}

void TheChordFiresOnceWhenItCompletes() {
  ComboDetector detector;
  detector.Filter(kSelect);
  Check(!detector.fired(), "half the chord does not fire");
  detector.Filter(kSelect | kR1);
  Check(detector.fired(), "completing the chord fires");
}

void HoldingTheChordDoesNotRepeat() {
  // A menu that reopened every sample while the buttons were held would be
  // unusable, and the pad reports far faster than anyone can let go.
  ComboDetector detector;
  detector.Filter(kSelect | kR1);
  Check(detector.fired(), "the first sample fires");
  for (int sample = 0; sample < 50; ++sample) {
    detector.Filter(kSelect | kR1);
    if (detector.fired()) {
      Check(false, "holding the chord must not fire again");
      return;
    }
  }
  Check(true, "holding the chord does not fire again");
}

void ReleasingAndPressingAgainFires() {
  ComboDetector detector;
  detector.Filter(kSelect | kR1);
  detector.Filter(kSelect);
  Check(!detector.fired(), "breaking the chord does not fire");
  detector.Filter(kSelect | kR1);
  Check(detector.fired(), "pressing it again fires");
}

void TheChordIsHiddenFromTheGameWhileHeld() {
  ComboDetector detector;
  const uint16_t seen = detector.Filter(kSelect | kR1 | kSouth);
  Check((seen & kOverlayComboMask) == 0, "neither chord button reaches the game");
  Check((seen & kSouth) == kSouth, "and every other button still does");
}

void EitherButtonAloneReachesTheGame() {
  // R1 on its own is one of the most used buttons there is. Swallowing it
  // would break every game on the device.
  ComboDetector detector;
  Check(detector.Filter(kR1) == kR1, "R1 alone passes through");
  Check(!detector.fired(), "and does not fire");
  Check(detector.Filter(kSelect) == kSelect, "select alone passes through");
  Check(!detector.fired(), "and does not fire");
}

void ReleasingRestoresTheButtonsImmediately() {
  ComboDetector detector;
  detector.Filter(kSelect | kR1);
  Check(detector.Filter(kR1) == kR1, "the surviving button reaches the game on the next sample");
}

void NothingPressedIsInert() {
  ComboDetector detector;
  Check(detector.Filter(0) == 0, "an empty sample passes through unchanged");
  Check(!detector.fired(), "and does not fire");
}

}  // namespace

int main() {
  TheChordFiresOnceWhenItCompletes();
  HoldingTheChordDoesNotRepeat();
  ReleasingAndPressingAgainFires();
  TheChordIsHiddenFromTheGameWhileHeld();
  EitherButtonAloneReachesTheGame();
  ReleasingRestoresTheButtonsImmediately();
  NothingPressedIsInert();
  if (g_failures != 0) {
    std::cout << g_failures << " failed\n";
    return 1;
  }
  std::cout << "rsinput combo tests passed\n";
  return 0;
}
