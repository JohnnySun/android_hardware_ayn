// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_mapping.h"
#include "ayn/rsinput_protocol.h"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using ayn::rsinput::InputEvent;
using ayn::rsinput::MapStatusToEvents;
using ayn::rsinput::Parser;
using ayn::rsinput::Status;

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}

void CheckFrame(const std::vector<uint8_t>& actual,
                const std::vector<uint8_t>& expected) {
  CHECK(actual == expected);
  uint8_t checksum = 0;
  for (size_t i = 4; i + 1 < actual.size(); ++i) {
    checksum ^= actual[i];
  }
  CHECK(checksum == actual.back());
}

void AppendLe16(std::vector<uint8_t>* bytes, uint16_t value) {
  bytes->push_back(static_cast<uint8_t>(value & 0xff));
  bytes->push_back(static_cast<uint8_t>(value >> 8));
}

std::vector<uint8_t> MakeStatusFrame() {
  std::vector<uint8_t> frame(Parser::kMagic.begin(), Parser::kMagic.end());
  frame.insert(frame.end(), {0x44, Parser::kCmdStatus, 0x0e, 0x00});
  AppendLe16(&frame, 0x0001);
  AppendLe16(&frame, 0x0610);
  AppendLe16(&frame, 0x0000);
  AppendLe16(&frame, 0x0010);
  AppendLe16(&frame, 0xfff0);
  AppendLe16(&frame, 0x0020);
  AppendLe16(&frame, 0xffe0);
  uint8_t checksum = 0;
  for (size_t index = 4; index < frame.size(); ++index) {
    checksum ^= frame[index];
  }
  frame.push_back(checksum);
  return frame;
}

void CollectMappedEvents(void* context, const Status& status) {
  auto* events = static_cast<std::vector<InputEvent>*>(context);
  const auto mapped = MapStatusToEvents(status);
  events->insert(events->end(), mapped.begin(), mapped.end());
}

void ExactQ9HandshakeFramesAreEncoded() {
  const auto frames = ayn::rsinput::BuildQ9HandshakeFrames();
  CHECK(frames.size() == 3);
  CheckFrame(frames[0], {0xA5, 0xD3, 0x5A, 0x3D, 0x01, 0x01, 0x01, 0x00,
                         0x06, 0x07});
  CheckFrame(frames[1], {0xA5, 0xD3, 0x5A, 0x3D, 0x02, 0x01, 0x0A, 0x00,
                         0x05, 0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00,
                         0x00, 0x01, 0x09});
  CheckFrame(frames[2], {0xA5, 0xD3, 0x5A, 0x3D, 0x03, 0x01, 0x01, 0x00,
                         0x06, 0x05});
}

void DeviceGateIsExactAndFailClosed() {
  CHECK(ayn::rsinput::IsSupportedDevice("odin2_mini"));
  CHECK(!ayn::rsinput::IsSupportedDevice(""));
  CHECK(!ayn::rsinput::IsSupportedDevice("odin2"));
  CHECK(!ayn::rsinput::IsSupportedDevice("Odin2_Mini"));
  CHECK(!ayn::rsinput::IsSupportedDevice("odin2_mini "));
}

int CountRuntimeEntry(void* context) {
  auto* count = static_cast<int*>(context);
  ++*count;
  return 23;
}

void UnsupportedDeviceCannotEnterRuntimeIo() {
  int runtime_entries = 0;
  CHECK(ayn::rsinput::RunIfSupportedDevice("kalama", CountRuntimeEntry,
                                           &runtime_entries) == 1);
  CHECK(runtime_entries == 0);
  CHECK(ayn::rsinput::RunIfSupportedDevice("odin2_mini", CountRuntimeEntry,
                                           &runtime_entries) == 23);
  CHECK(runtime_entries == 1);
}

void StatusBitsMapToTheRequiredLinuxCodes() {
  Status status;
  status.buttons = 0xA55A;
  const auto events = MapStatusToEvents(status);

  const std::array<uint16_t, 16> expected_codes = {
      ayn::rsinput::kBtnDpadUp,   ayn::rsinput::kBtnDpadDown,
      ayn::rsinput::kBtnDpadLeft, ayn::rsinput::kBtnDpadRight,
      ayn::rsinput::kBtnNorth,    ayn::rsinput::kBtnWest,
      ayn::rsinput::kBtnEast,     ayn::rsinput::kBtnSouth,
      ayn::rsinput::kBtnTl,       ayn::rsinput::kBtnTr,
      ayn::rsinput::kBtnSelect,   ayn::rsinput::kBtnStart,
      ayn::rsinput::kBtnThumbL,   ayn::rsinput::kBtnThumbR,
      ayn::rsinput::kBtnMode,     ayn::rsinput::kBtnBack,
  };

  for (size_t bit = 0; bit < expected_codes.size(); ++bit) {
    CHECK(events[bit].type == ayn::rsinput::kEventTypeKey);
    CHECK(events[bit].code == expected_codes[bit]);
    const int32_t expected_value =
        static_cast<int32_t>((status.buttons >> bit) & 1u);
    CHECK(events[bit].value == expected_value);
  }
}

void Odin2AxisAndTriggerPolicyIsPreserved() {
  Status status;
  status.left_x = -32768;
  status.left_y = 123;
  status.right_x = -456;
  status.right_y = 32767;
  status.left_trigger = 0;
  status.right_trigger = 0x700;

  const auto events = MapStatusToEvents(status);
  CHECK((events[16] == InputEvent{ayn::rsinput::kEventTypeAbs,
                                  ayn::rsinput::kAbsX, 32768}));
  CHECK((events[17] == InputEvent{ayn::rsinput::kEventTypeAbs,
                                  ayn::rsinput::kAbsY, -123}));
  CHECK((events[18] == InputEvent{ayn::rsinput::kEventTypeAbs,
                                  ayn::rsinput::kAbsRx, 456}));
  CHECK((events[19] == InputEvent{ayn::rsinput::kEventTypeAbs,
                                  ayn::rsinput::kAbsRy, -32767}));
  CHECK((events[20] == InputEvent{ayn::rsinput::kEventTypeAbs,
                                  ayn::rsinput::kAbsZ, 0x610}));
  CHECK((events[21] == InputEvent{ayn::rsinput::kEventTypeAbs,
                                  ayn::rsinput::kAbsRz, 0}));
  CHECK((events[22] == InputEvent{ayn::rsinput::kEventTypeSyn,
                                  ayn::rsinput::kSynReport, 0}));
}

void TriggerPolicyClampsBothSides() {
  Status status;
  status.left_trigger = 0x610;
  status.right_trigger = 0x5FF;
  const auto events = MapStatusToEvents(status);
  CHECK(events[20].value == 0);
  CHECK(events[21].value == 0x11);
}

void MalformedOrIncompleteDataCannotEmitInputEvents() {
  const auto valid_frame = MakeStatusFrame();
  auto bad_frame = valid_frame;
  bad_frame.back() ^= 0x80;

  Parser parser;
  std::vector<InputEvent> events;
  parser.Feed(valid_frame.data(), valid_frame.size() - 1, CollectMappedEvents,
              &events);
  CHECK(events.empty());
  parser.Feed(bad_frame.data(), bad_frame.size(), CollectMappedEvents, &events);
  CHECK(events.empty());
  parser.Feed(valid_frame.data() + valid_frame.size() - 1, 1,
              CollectMappedEvents, &events);
  CHECK(events.empty());
  parser.Feed(valid_frame.data(), valid_frame.size(), CollectMappedEvents,
              &events);
  CHECK(events.size() == ayn::rsinput::kStatusEventCount);
  CHECK(events.back().type == ayn::rsinput::kEventTypeSyn);
  CHECK(events.back().code == ayn::rsinput::kSynReport);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"exact Q9 handshake frames are encoded",
       ExactQ9HandshakeFramesAreEncoded},
      {"device gate is exact and fail closed", DeviceGateIsExactAndFailClosed},
      {"unsupported device cannot enter runtime I/O",
       UnsupportedDeviceCannotEnterRuntimeIo},
      {"status bits map to the required Linux codes",
       StatusBitsMapToTheRequiredLinuxCodes},
      {"Odin2 axis and trigger policy is preserved",
       Odin2AxisAndTriggerPolicyIsPreserved},
      {"trigger policy clamps both sides", TriggerPolicyClampsBothSides},
      {"malformed or incomplete data cannot emit input events",
       MalformedOrIncompleteDataCannotEmitInputEvents},
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
