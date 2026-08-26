// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace ayn::rsinput {

// The combination that opens the game overlay.
//
// This daemon is the only thing on the device that sees the pad before the
// framework does, which is why the detection lives here: an app cannot grab a
// global chord without becoming an accessibility service, and the framework
// route would mean patching PhoneWindowManager.
//
// SELECT and R1 together, by the bit positions in Status::buttons - bit 9 is
// BTN_TR and bit 10 is BTN_SELECT. Chosen because neither is a movement or
// face button, the pair is awkward to hit by accident, and both are reachable
// without letting go of the device. It is one constant to change.
constexpr uint16_t kOverlayComboMask =
    static_cast<uint16_t>((1u << 9) | (1u << 10));

// Recognises the chord in a stream of button samples.
//
// The chord is swallowed while it is held, so a game never sees the buttons
// that opened a menu over it. Releasing either one ends the hold and the bits
// resume reaching the framework on the next sample.
class ComboDetector {
 public:
  // Feed one sample. Returns the buttons the framework should see.
  uint16_t Filter(uint16_t buttons);

  // Whether the last Filter call completed the chord. True on exactly one
  // sample per press: holding it does not repeat, because a menu that reopened
  // sixty times a second would be unusable.
  bool fired() const { return fired_; }

 private:
  bool engaged_ = false;
  bool fired_ = false;
};

}  // namespace ayn::rsinput
