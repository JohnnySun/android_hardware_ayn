// SPDX-License-Identifier: Apache-2.0
#include "ayn/rsinput_combo.h"

namespace ayn::rsinput {

uint16_t ComboDetector::Filter(uint16_t buttons) {
  const bool held = (buttons & kOverlayComboMask) == kOverlayComboMask;
  fired_ = held && !engaged_;
  engaged_ = held;
  // Only the chord is hidden, and only while it is complete. Pressing R1 alone
  // must still reach the game, which is the common case by a long way.
  return held ? static_cast<uint16_t>(buttons & ~kOverlayComboMask) : buttons;
}

}  // namespace ayn::rsinput
