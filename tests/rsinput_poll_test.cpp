// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_poll.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using ayn::rsinput::StartupPollRxDetector;

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}

void AppendLe16(std::vector<uint8_t>* bytes, uint16_t value) {
  bytes->push_back(static_cast<uint8_t>(value & 0xff));
  bytes->push_back(static_cast<uint8_t>(value >> 8));
}

void AppendBe32(std::vector<uint8_t>* bytes, uint32_t value) {
  bytes->push_back(static_cast<uint8_t>(value >> 24));
  bytes->push_back(static_cast<uint8_t>(value >> 16));
  bytes->push_back(static_cast<uint8_t>(value >> 8));
  bytes->push_back(static_cast<uint8_t>(value));
}

std::vector<uint8_t> MakeA5Frame(uint8_t outer_type,
                                 const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> frame = {0xA5, 0xD3, 0x5A, 0x3D, 0x27, outer_type};
  AppendLe16(&frame, static_cast<uint16_t>(payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());
  uint8_t checksum = 0;
  for (size_t index = 4; index < frame.size(); ++index) {
    checksum ^= frame[index];
  }
  frame.push_back(checksum);
  return frame;
}

struct LegacyRecord {
  uint8_t kind;
  uint32_t value;
};

std::vector<uint8_t> MakeLegacyFrame(
    const std::vector<LegacyRecord>& records,
    const std::vector<uint8_t>& padding = {}) {
  const size_t payload_size = records.size() * 8 + padding.size();
  CHECK(payload_size <= 255);
  CHECK(records.size() <= 255);

  std::vector<uint8_t> frame = {
      0x47, 0xB3, 0x57, 0xE2, static_cast<uint8_t>(payload_size),
      static_cast<uint8_t>(records.size()), 0, 0, 0, 0, 0, 0,
  };
  for (const LegacyRecord& record : records) {
    frame.push_back(record.kind);
    frame.push_back(0x91);
    frame.push_back(0x82);
    frame.push_back(0x73);
    AppendBe32(&frame, record.value);
  }
  frame.insert(frame.end(), padding.begin(), padding.end());
  return frame;
}

void Feed(StartupPollRxDetector* detector, const std::vector<uint8_t>& bytes) {
  detector->Feed(bytes.data(), bytes.size());
}

void CheckNoRequests(StartupPollRxDetector* detector) {
  CHECK(!detector->poll_stop_requested());
  CHECK(detector->consume_immediate_poll_requests() == 0);
}

void EveryA5ByteSplitBoundary() {
  const auto frame = MakeA5Frame(1, {5, 0x31, 0x41});
  for (size_t split = 0; split <= frame.size(); ++split) {
    StartupPollRxDetector detector;
    detector.Feed(frame.data(), split);
    if (split < frame.size()) {
      CHECK(!detector.poll_stop_requested());
    }
    detector.Feed(frame.data() + split, frame.size() - split);
    CHECK(detector.poll_stop_requested());
    CHECK(detector.consume_immediate_poll_requests() == 0);
    CHECK(detector.buffered_bytes() == 0);
  }
}

void EveryLegacyByteSplitBoundary() {
  const auto frame = MakeLegacyFrame({{4, 1}, {4, 1}});
  for (size_t split = 0; split <= frame.size(); ++split) {
    StartupPollRxDetector detector;
    detector.Feed(frame.data(), split);
    if (split < frame.size()) {
      CHECK(detector.consume_immediate_poll_requests() == 0);
    }
    detector.Feed(frame.data() + split, frame.size() - split);
    CHECK(!detector.poll_stop_requested());
    CHECK(detector.consume_immediate_poll_requests() == 2);
    CHECK(detector.consume_immediate_poll_requests() == 0);
    CHECK(detector.buffered_bytes() == 0);
  }
}

void ByteAtATimeAcrossPartialMagicAndNoise() {
  std::vector<uint8_t> bytes = {0xA5, 0xD3, 0x00, 0x47, 0xB3, 0x57,
                                0x00, 0xA5, 0xD3, 0x5A};
  const auto frame = MakeA5Frame(1, {5});
  bytes.insert(bytes.end(), frame.begin(), frame.end());

  StartupPollRxDetector detector;
  for (uint8_t byte : bytes) {
    detector.Feed(&byte, 1);
    CHECK(detector.buffered_bytes() <=
          StartupPollRxDetector::kMaxBufferedBytes);
  }
  CHECK(detector.poll_stop_requested());
}

void CoalescedA5AndLegacyFramesAreDemultiplexed() {
  auto bytes = MakeA5Frame(1, {5, 0xAA});
  const auto legacy =
      MakeLegacyFrame({{7, 0}, {4, 1}, {4, 0x00000100}, {4, 1}},
                      {0xDE, 0xAD});
  bytes.insert(bytes.end(), legacy.begin(), legacy.end());

  StartupPollRxDetector detector;
  Feed(&detector, bytes);
  CHECK(detector.poll_stop_requested());
  CHECK(detector.consume_immediate_poll_requests() == 2);
  CHECK(detector.buffered_bytes() == 0);
}

void A5ConditionsAreFailClosed() {
  std::vector<std::vector<uint8_t>> rejected;
  rejected.push_back(MakeA5Frame(2, {5}));
  rejected.push_back(MakeA5Frame(1, {}));
  rejected.push_back(MakeA5Frame(1, {4}));

  auto bad_checksum = MakeA5Frame(1, {5});
  bad_checksum.back() ^= 0x80;
  rejected.push_back(std::move(bad_checksum));

  for (const auto& frame : rejected) {
    StartupPollRxDetector detector;
    Feed(&detector, frame);
    CheckNoRequests(&detector);
  }
}

void A5ChecksumCoversHeaderAndEntirePayload() {
  auto payload_corruption = MakeA5Frame(1, {5, 0x10, 0x20});
  payload_corruption[9] ^= 0x01;
  auto sequence_corruption = MakeA5Frame(1, {5});
  sequence_corruption[4] ^= 0x01;
  auto length_corruption = MakeA5Frame(1, {5});
  length_corruption[6] = 2;
  length_corruption.push_back(0);

  for (const auto* frame : {&payload_corruption, &sequence_corruption,
                            &length_corruption}) {
    StartupPollRxDetector detector;
    Feed(&detector, *frame);
    CheckNoRequests(&detector);
  }
}

void A5MaximumLengthAndOversizeAreHandled() {
  std::vector<uint8_t> maximum_payload(49, 0xCC);
  maximum_payload[0] = 5;
  const auto maximum = MakeA5Frame(1, maximum_payload);

  StartupPollRxDetector accepted;
  Feed(&accepted, maximum);
  CHECK(accepted.poll_stop_requested());

  const auto oversize = MakeA5Frame(1, std::vector<uint8_t>(50, 5));
  StartupPollRxDetector rejected;
  Feed(&rejected, oversize);
  CheckNoRequests(&rejected);
  CHECK(rejected.buffered_bytes() == 0);

  const auto following = MakeA5Frame(1, {5});
  Feed(&rejected, following);
  CHECK(rejected.poll_stop_requested());
  CHECK(rejected.consume_immediate_poll_requests() == 0);
  CHECK(rejected.buffered_bytes() == 0);
}

void IncompleteA5FrameCannotTrigger() {
  const auto frame = MakeA5Frame(1, {5});
  for (size_t size = 0; size < frame.size(); ++size) {
    StartupPollRxDetector detector;
    detector.Feed(frame.data(), size);
    CheckNoRequests(&detector);
  }
}

void LegacyStopAndImmediateValuesUseFullBe32() {
  const auto frame = MakeLegacyFrame({
      {4, 0x00000000}, {4, 0x00000001}, {4, 0x00000100},
      {4, 0x00010000}, {4, 0x01000000}, {4, 0xFFFFFFFF}, {3, 0},
  });
  StartupPollRxDetector detector;
  Feed(&detector, frame);
  CHECK(detector.poll_stop_requested());
  CHECK(detector.consume_immediate_poll_requests() == 1);
}

void LegacyRecordBoundsAreFailClosed() {
  std::vector<uint8_t> too_many_records = {
      0x47, 0xB3, 0x57, 0xE2, 7, 1, 0, 0, 0, 0, 0, 0,
      4,    0,    0,    0,    0, 0, 0,
  };
  const auto following = MakeLegacyFrame({{4, 1}});
  too_many_records.insert(too_many_records.end(), following.begin(),
                          following.end());

  StartupPollRxDetector detector;
  Feed(&detector, too_many_records);
  CHECK(!detector.poll_stop_requested());
  CHECK(detector.consume_immediate_poll_requests() == 1);
  CHECK(detector.buffered_bytes() == 0);
}

void IncompleteLegacyRecordsCannotTrigger() {
  const auto stop = MakeLegacyFrame({{4, 0}});
  const auto immediate = MakeLegacyFrame({{4, 1}});
  for (const auto* frame : {&stop, &immediate}) {
    for (size_t size = 0; size < frame->size(); ++size) {
      StartupPollRxDetector detector;
      detector.Feed(frame->data(), size);
      CheckNoRequests(&detector);
    }
  }
}

void MalformedFramesDoNotActivateEmbeddedMagic() {
  const auto embedded_a5 = MakeA5Frame(1, {5});
  auto bad_a5 = MakeA5Frame(1, embedded_a5);
  bad_a5.back() ^= 0x01;

  const auto embedded_legacy = MakeLegacyFrame({{4, 0}});
  std::vector<uint8_t> bad_legacy = {
      0x47, 0xB3, 0x57, 0xE2, static_cast<uint8_t>(embedded_legacy.size()),
      0xFF, 0, 0, 0, 0, 0, 0,
  };
  bad_legacy.insert(bad_legacy.end(), embedded_legacy.begin(),
                    embedded_legacy.end());

  for (const auto* frame : {&bad_a5, &bad_legacy}) {
    StartupPollRxDetector detector;
    Feed(&detector, *frame);
    CheckNoRequests(&detector);
    CHECK(detector.buffered_bytes() == 0);
  }
}

void NullAndEmptyFeedAreNoOps() {
  StartupPollRxDetector detector;
  detector.Feed(nullptr, 100);
  detector.Feed(nullptr, 0);
  CheckNoRequests(&detector);
  CHECK(detector.buffered_bytes() == 0);
}

void NoiseBufferRemainsBoundedAndRecovers() {
  StartupPollRxDetector detector;
  std::vector<uint8_t> noise(1024 * 1024, 0xA5);
  for (size_t offset = 0; offset < noise.size(); offset += 137) {
    const size_t remaining = noise.size() - offset;
    const size_t chunk = remaining < 137 ? remaining : 137;
    detector.Feed(noise.data() + offset, chunk);
    CHECK(detector.buffered_bytes() <=
          StartupPollRxDetector::kMaxBufferedBytes);
  }
  CHECK(detector.buffered_bytes() <= 3);
  CheckNoRequests(&detector);

  const auto immediate = MakeLegacyFrame({{4, 1}});
  Feed(&detector, immediate);
  CHECK(detector.consume_immediate_poll_requests() == 1);
  CHECK(detector.buffered_bytes() == 0);
}

void ImmediateCounterIsConsumedWithoutClearingStop() {
  auto bytes = MakeLegacyFrame({{4, 1}, {4, 1}, {4, 0}});
  StartupPollRxDetector detector;
  Feed(&detector, bytes);
  CHECK(detector.consume_immediate_poll_requests() == 2);
  CHECK(detector.consume_immediate_poll_requests() == 0);
  CHECK(detector.poll_stop_requested());
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"EveryA5ByteSplitBoundary", EveryA5ByteSplitBoundary},
      {"EveryLegacyByteSplitBoundary", EveryLegacyByteSplitBoundary},
      {"ByteAtATimeAcrossPartialMagicAndNoise",
       ByteAtATimeAcrossPartialMagicAndNoise},
      {"CoalescedA5AndLegacyFramesAreDemultiplexed",
       CoalescedA5AndLegacyFramesAreDemultiplexed},
      {"A5ConditionsAreFailClosed", A5ConditionsAreFailClosed},
      {"A5ChecksumCoversHeaderAndEntirePayload",
       A5ChecksumCoversHeaderAndEntirePayload},
      {"A5MaximumLengthAndOversizeAreHandled",
       A5MaximumLengthAndOversizeAreHandled},
      {"IncompleteA5FrameCannotTrigger", IncompleteA5FrameCannotTrigger},
      {"LegacyStopAndImmediateValuesUseFullBe32",
       LegacyStopAndImmediateValuesUseFullBe32},
      {"LegacyRecordBoundsAreFailClosed", LegacyRecordBoundsAreFailClosed},
      {"IncompleteLegacyRecordsCannotTrigger",
       IncompleteLegacyRecordsCannotTrigger},
      {"MalformedFramesDoNotActivateEmbeddedMagic",
       MalformedFramesDoNotActivateEmbeddedMagic},
      {"NullAndEmptyFeedAreNoOps", NullAndEmptyFeedAreNoOps},
      {"NoiseBufferRemainsBoundedAndRecovers",
       NoiseBufferRemainsBoundedAndRecovers},
      {"ImmediateCounterIsConsumedWithoutClearingStop",
       ImmediateCounterIsConsumedWithoutClearingStop},
  };

  try {
    for (const auto& [name, test] : tests) {
      test();
      std::cout << "PASS " << name << '\n';
    }
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
  return 0;
}
