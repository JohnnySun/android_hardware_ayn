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

// The Odin2 Mini MCU reports approximately +/-0x500 at stick end stops.
// Keep the declared uinput range in the same units as the status protocol.
constexpr int32_t kStickAxisMin = -0x500;
constexpr int32_t kStickAxisMax = 0x500;
constexpr int32_t kStickAxisFlat = 0x80;
constexpr int32_t kTriggerAxisMin = 0;
constexpr int32_t kTriggerAxisMax = 0x610;
constexpr int32_t kTriggerAxisFlat = 30;

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

}  // namespace ayn::rsinput
