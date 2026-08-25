// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ayn/rsinput_parser.h"

#include <array>
#include <cstdint>
#include <string>

namespace ayn::rsinput {

constexpr uint16_t kEventTypeSyn = 0x00;
constexpr uint16_t kEventTypeKey = 0x01;
constexpr uint16_t kEventTypeAbs = 0x03;
constexpr uint16_t kSynReport = 0;

constexpr uint16_t kBtnDpadUp = 0x220;
constexpr uint16_t kBtnDpadDown = 0x221;
constexpr uint16_t kBtnDpadLeft = 0x222;
constexpr uint16_t kBtnDpadRight = 0x223;
constexpr uint16_t kBtnNorth = 0x133;
constexpr uint16_t kBtnWest = 0x134;
constexpr uint16_t kBtnEast = 0x131;
constexpr uint16_t kBtnSouth = 0x130;
constexpr uint16_t kBtnTl = 0x136;
constexpr uint16_t kBtnTr = 0x137;
constexpr uint16_t kBtnSelect = 0x13a;
constexpr uint16_t kBtnStart = 0x13b;
constexpr uint16_t kBtnThumbL = 0x13d;
constexpr uint16_t kBtnThumbR = 0x13e;
constexpr uint16_t kBtnMode = 0x13c;
constexpr uint16_t kBtnBack = 0x116;

constexpr uint16_t kAbsX = 0x00;
constexpr uint16_t kAbsY = 0x01;
constexpr uint16_t kAbsZ = 0x02;
constexpr uint16_t kAbsRx = 0x03;
constexpr uint16_t kAbsRy = 0x04;
constexpr uint16_t kAbsRz = 0x05;

// A declared range must sit at or below what the hardware can actually reach.
// Declaring more than the hardware reaches means full deflection never reads
// full; declaring less only clamps the last fraction, which reads as reaching
// full slightly before the mechanical stop. So the bias is always downward.
//
// Measured on 2026-08-24 over hundreds of samples per axis: the four stick
// axes end between 1242 and 1324, and both triggers top out at 1511 to 1514,
// including a deliberate press to the hard stop.
//
// The sticks keep +/-0x500, which is 1280 and sits inside that spread: axes
// that reach past it clamp to full, and the weakest still reads 97 percent.
// Widening to the largest observed 1324 would make the weakest axis stop at
// 94 percent, which is worse.
constexpr int32_t kStickAxisMin = -0x500;
constexpr int32_t kStickAxisMax = 0x500;
constexpr int32_t kStickAxisFlat = 0x80;
constexpr int32_t kTriggerAxisMin = 0;
// The triggers did need it. At the old 0x610, which is 1552, a trigger pressed
// to its hard stop reported 97.4 percent and full was unreachable. 1500 is just
// under the lowest measured maximum, so a full press reads full with a little
// room for a unit whose triggers end slightly shorter.
constexpr int32_t kTriggerAxisMax = 1500;
constexpr int32_t kTriggerAxisFlat = 30;

enum class ControllerProfile : int32_t {
  kStandard = 0,
  kFlippedFace = 1,
};

struct InputEvent {
  uint16_t type;
  uint16_t code;
  int32_t value;

  bool operator==(const InputEvent& other) const {
    return type == other.type && code == other.code && value == other.value;
  }
};

constexpr size_t kStatusEventCount = 23;

using SupportedDeviceRunner = int (*)(void* context);

bool IsSupportedDevice(const std::string& product_device);
int RunIfSupportedDevice(const std::string& product_device,
                         SupportedDeviceRunner runner, void* context);
std::array<InputEvent, kStatusEventCount> MapStatusToEvents(
    const Status& status);
std::array<InputEvent, kStatusEventCount> MapStatusToEvents(
    const Status& status, ControllerProfile profile);
bool IsValidControllerProfile(ControllerProfile profile);

}  // namespace ayn::rsinput
