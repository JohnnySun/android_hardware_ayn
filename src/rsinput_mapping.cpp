// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_mapping.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ayn::rsinput {

namespace {

constexpr std::array<uint16_t, 16> kButtonCodes = {
    kBtnDpadUp,   kBtnDpadDown, kBtnDpadLeft, kBtnDpadRight,
    kBtnWest,     kBtnNorth,    kBtnEast,     kBtnSouth,
    kBtnTl,       kBtnTr,       kBtnSelect,   kBtnStart,
    kBtnThumbL,   kBtnThumbR,   kBtnMode,     kBtnBack,
};

int32_t NegateAxis(int16_t raw) {
  return -static_cast<int32_t>(raw);
}

int32_t TransformTrigger(uint16_t raw) {
  return std::clamp(0x610 - static_cast<int32_t>(raw), 0, 0x610);
}

}  // namespace

bool IsSupportedDevice(const std::string& product_device) {
  return product_device == "odin2_mini";
}

int RunIfSupportedDevice(const std::string& product_device,
                         SupportedDeviceRunner runner, void* context) {
  if (!IsSupportedDevice(product_device) || runner == nullptr) {
    return 1;
  }
  return runner(context);
}

std::array<InputEvent, kStatusEventCount> MapStatusToEvents(
    const Status& status) {
  std::array<InputEvent, kStatusEventCount> events{};
  for (size_t bit = 0; bit < kButtonCodes.size(); ++bit) {
    events[bit] = {kEventTypeKey, kButtonCodes[bit],
                   static_cast<int32_t>((status.buttons >> bit) & 1u)};
  }

  events[16] = {kEventTypeAbs, kAbsX, NegateAxis(status.left_x)};
  events[17] = {kEventTypeAbs, kAbsY, NegateAxis(status.left_y)};
  events[18] = {kEventTypeAbs, kAbsRx, NegateAxis(status.right_x)};
  events[19] = {kEventTypeAbs, kAbsRy, NegateAxis(status.right_y)};
  events[20] = {kEventTypeAbs, kAbsZ, TransformTrigger(status.left_trigger)};
  events[21] = {kEventTypeAbs, kAbsRz,
                TransformTrigger(status.right_trigger)};
  events[22] = {kEventTypeSyn, kSynReport, 0};
  return events;
}

}  // namespace ayn::rsinput
