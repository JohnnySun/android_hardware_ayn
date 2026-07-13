// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_parser.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using ayn::rsinput::Parser;
using ayn::rsinput::Status;

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

std::vector<uint8_t> MakeStatusPayload() {
  std::vector<uint8_t> payload;
  AppendLe16(&payload, 0xA55A);
  AppendLe16(&payload, 0x1234);
  AppendLe16(&payload, 0xBEEF);
  AppendLe16(&payload, static_cast<uint16_t>(-32768));
  AppendLe16(&payload, static_cast<uint16_t>(-1));
  AppendLe16(&payload, 0x1234);
  AppendLe16(&payload, 0x7FFF);
  return payload;
}

std::vector<uint8_t> MakeFrame(uint8_t sequence, uint8_t command,
                               const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> frame(Parser::kMagic.begin(), Parser::kMagic.end());
  frame.push_back(sequence);
  frame.push_back(command);
  AppendLe16(&frame, static_cast<uint16_t>(payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());
  uint8_t checksum = 0;
  for (size_t i = 4; i < frame.size(); ++i) {
    checksum ^= frame[i];
  }
  frame.push_back(checksum);
  return frame;
}

void Collect(void* context, const Status& status) {
  static_cast<std::vector<Status>*>(context)->push_back(status);
}

void Feed(Parser* parser, const uint8_t* data, size_t size,
          std::vector<Status>* statuses) {
  parser->Feed(data, size, Collect, statuses);
}

void CheckDecodedStatus(const Status& status, uint8_t sequence) {
  CHECK(status.sequence == sequence);
  CHECK(status.buttons == 0xA55A);
  CHECK(status.left_trigger == 0x1234);
  CHECK(status.right_trigger == 0xBEEF);
  CHECK(status.left_x == -32768);
  CHECK(status.left_y == -1);
  CHECK(status.right_x == 0x1234);
  CHECK(status.right_y == 32767);
}

void EveryByteSplitBoundary() {
  const auto frame = MakeFrame(0x42, Parser::kCmdStatus, MakeStatusPayload());
  for (size_t split = 0; split <= frame.size(); ++split) {
    Parser parser;
    std::vector<Status> statuses;
    Feed(&parser, frame.data(), split, &statuses);
    Feed(&parser, frame.data() + split, frame.size() - split, &statuses);
    CHECK(statuses.size() == 1);
    CheckDecodedStatus(statuses.front(), 0x42);
    CHECK(parser.buffered_bytes() == 0);
  }
}

void ByteAtATime() {
  const auto frame = MakeFrame(0x43, Parser::kCmdStatus, MakeStatusPayload());
  Parser parser;
  std::vector<Status> statuses;
  for (uint8_t byte : frame) {
    Feed(&parser, &byte, 1, &statuses);
  }
  CHECK(statuses.size() == 1);
  CheckDecodedStatus(statuses.front(), 0x43);
}

void CoalescedFrames() {
  auto bytes = MakeFrame(1, Parser::kCmdStatus, MakeStatusPayload());
  const auto second = MakeFrame(2, Parser::kCmdStatus, MakeStatusPayload());
  bytes.insert(bytes.end(), second.begin(), second.end());
  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bytes.data(), bytes.size(), &statuses);
  CHECK(statuses.size() == 2);
  CHECK(statuses[0].sequence == 1);
  CHECK(statuses[1].sequence == 2);
}

void LeadingNoiseAndPartialMagic() {
  std::vector<uint8_t> bytes = {0x00, 0xA5, 0xD3, 0x00, 0x7F, 0xA5};
  const auto frame = MakeFrame(3, Parser::kCmdStatus, MakeStatusPayload());
  bytes.insert(bytes.end(), frame.begin(), frame.end());
  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bytes.data(), bytes.size(), &statuses);
  CHECK(statuses.size() == 1);
  CHECK(statuses.front().sequence == 3);
  CHECK(parser.stats().noise_bytes >= 5);
}

void BadMagicThenRecovery() {
  auto bad = MakeFrame(4, Parser::kCmdStatus, MakeStatusPayload());
  bad[2] ^= 0x01;
  const auto good = MakeFrame(5, Parser::kCmdStatus, MakeStatusPayload());
  bad.insert(bad.end(), good.begin(), good.end());
  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bad.data(), bad.size(), &statuses);
  CHECK(statuses.size() == 1);
  CHECK(statuses.front().sequence == 5);
}

void BadChecksumThenRecovery() {
  auto bad = MakeFrame(6, Parser::kCmdStatus, MakeStatusPayload());
  bad.back() ^= 0x80;
  const auto good = MakeFrame(7, Parser::kCmdStatus, MakeStatusPayload());
  bad.insert(bad.end(), good.begin(), good.end());
  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bad.data(), bad.size(), &statuses);
  CHECK(statuses.size() == 1);
  CHECK(statuses.front().sequence == 7);
  CHECK(parser.stats().bad_checksums == 1);
}

void OversizeLengthThenRecovery() {
  std::vector<uint8_t> bytes(Parser::kMagic.begin(), Parser::kMagic.end());
  bytes.push_back(8);
  bytes.push_back(Parser::kCmdStatus);
  AppendLe16(&bytes, static_cast<uint16_t>(Parser::kMaxPayloadSize + 1));
  const auto good = MakeFrame(9, Parser::kCmdStatus, MakeStatusPayload());
  bytes.insert(bytes.end(), good.begin(), good.end());
  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bytes.data(), bytes.size(), &statuses);
  CHECK(statuses.size() == 1);
  CHECK(statuses.front().sequence == 9);
  CHECK(parser.stats().oversize_lengths == 1);
}

void InvalidStatusLengthFrameDoesNotStallRecovery() {
  auto bytes = MakeFrame(
      10, Parser::kCmdStatus,
      std::vector<uint8_t>(Parser::kMaxPayloadSize, 0xCC));
  const auto good = MakeFrame(11, Parser::kCmdStatus, MakeStatusPayload());
  bytes.insert(bytes.end(), good.begin(), good.end());

  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bytes.data(), bytes.size(), &statuses);

  CHECK(statuses.size() == 1);
  CHECK(statuses.front().sequence == 11);
  CHECK(parser.stats().invalid_status_lengths == 1);
}

void UnsupportedCommandFrameDoesNotStallRecovery() {
  auto bytes = MakeFrame(
      12, 0x7F, std::vector<uint8_t>(Parser::kMaxPayloadSize, 0xCC));
  const auto good = MakeFrame(13, Parser::kCmdStatus, MakeStatusPayload());
  bytes.insert(bytes.end(), good.begin(), good.end());

  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bytes.data(), bytes.size(), &statuses);

  CHECK(statuses.size() == 1);
  CHECK(statuses.front().sequence == 13);
  CHECK(parser.stats().unsupported_commands == 1);
}

void UnsupportedCommandFrameDoesNotParseEmbeddedStatus() {
  const auto embedded =
      MakeFrame(0x80, Parser::kCmdStatus, MakeStatusPayload());
  auto bytes = MakeFrame(0x81, 0x7F, embedded);
  const auto following =
      MakeFrame(0x82, Parser::kCmdStatus, MakeStatusPayload());
  bytes.insert(bytes.end(), following.begin(), following.end());

  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bytes.data(), bytes.size(), &statuses);

  CHECK(!statuses.empty());
  CHECK(statuses.front().sequence == 0x82);
  CHECK(statuses.size() == 1);
  CHECK(parser.stats().unsupported_commands == 1);
  CHECK(parser.stats().accepted_status_frames == 1);
  CHECK(parser.buffered_bytes() == 0);
}

void InvalidStatusLengthFrameDoesNotParseEmbeddedStatus() {
  const auto embedded =
      MakeFrame(0x83, Parser::kCmdStatus, MakeStatusPayload());
  auto bytes = MakeFrame(0x84, Parser::kCmdStatus, embedded);
  const auto following =
      MakeFrame(0x85, Parser::kCmdStatus, MakeStatusPayload());
  bytes.insert(bytes.end(), following.begin(), following.end());

  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bytes.data(), bytes.size(), &statuses);

  CHECK(!statuses.empty());
  CHECK(statuses.front().sequence == 0x85);
  CHECK(statuses.size() == 1);
  CHECK(parser.stats().invalid_status_lengths == 1);
  CHECK(parser.stats().accepted_status_frames == 1);
  CHECK(parser.buffered_bytes() == 0);
}

void ReconnectResetDropsTruncation() {
  const auto frame = MakeFrame(10, Parser::kCmdStatus, MakeStatusPayload());
  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, frame.data(), 10, &statuses);
  CHECK(parser.buffered_bytes() == 10);
  parser.Reset();
  CHECK(parser.buffered_bytes() == 0);
  Feed(&parser, frame.data() + 10, frame.size() - 10, &statuses);
  CHECK(statuses.empty());
  Feed(&parser, frame.data(), frame.size(), &statuses);
  CHECK(statuses.size() == 1);
}

void StatusLengthIsFailClosed() {
  auto short_payload = MakeStatusPayload();
  short_payload.pop_back();
  auto long_payload = MakeStatusPayload();
  long_payload.push_back(0x99);
  auto bytes = MakeFrame(11, Parser::kCmdStatus, short_payload);
  const auto long_frame = MakeFrame(12, Parser::kCmdStatus, long_payload);
  const auto exact = MakeFrame(13, Parser::kCmdStatus, MakeStatusPayload());
  bytes.insert(bytes.end(), long_frame.begin(), long_frame.end());
  bytes.insert(bytes.end(), exact.begin(), exact.end());
  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bytes.data(), bytes.size(), &statuses);
  CHECK(statuses.size() == 1);
  CHECK(statuses.front().sequence == 13);
  CHECK(parser.stats().invalid_status_lengths == 2);
}

void UnknownCommandIsFailClosed() {
  auto bytes = MakeFrame(14, 0x7F, MakeStatusPayload());
  const auto good = MakeFrame(15, Parser::kCmdStatus, MakeStatusPayload());
  bytes.insert(bytes.end(), good.begin(), good.end());
  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, bytes.data(), bytes.size(), &statuses);
  CHECK(statuses.size() == 1);
  CHECK(statuses.front().sequence == 15);
  CHECK(parser.stats().unsupported_commands == 1);
}

void LargeNoiseInputRemainsBounded() {
  std::vector<uint8_t> noise(1024 * 1024, 0xCC);
  noise.push_back(Parser::kMagic[0]);
  noise.push_back(Parser::kMagic[1]);
  noise.push_back(Parser::kMagic[2]);
  Parser parser;
  std::vector<Status> statuses;
  Feed(&parser, noise.data(), noise.size(), &statuses);
  CHECK(statuses.empty());
  CHECK(parser.buffered_bytes() == 3);
  CHECK(parser.buffered_bytes() <= Parser::kMaxFrameSize);
}

struct ReentrantContext {
  Parser* parser;
  const std::vector<uint8_t>* next_frame;
  std::vector<Status>* statuses;
  bool fed_next_frame = false;
};

void CollectAndFeedNextFrame(void* opaque, const Status& status) {
  auto* context = static_cast<ReentrantContext*>(opaque);
  context->statuses->push_back(status);
  if (context->fed_next_frame) {
    return;
  }

  context->fed_next_frame = true;
  context->parser->Feed(context->next_frame->data(),
                        context->next_frame->size(), CollectAndFeedNextFrame,
                        context);
}

void HandlerCanReenterParserWithoutRepeatingAcceptedFrame() {
  const auto first = MakeFrame(0x70, Parser::kCmdStatus, MakeStatusPayload());
  const auto second = MakeFrame(0x71, Parser::kCmdStatus, MakeStatusPayload());
  Parser parser;
  std::vector<Status> statuses;
  ReentrantContext context{&parser, &second, &statuses};

  parser.Feed(first.data(), first.size(), CollectAndFeedNextFrame, &context);

  CHECK(statuses.size() >= 2);
  CHECK(statuses[0].sequence == 0x70);
  CHECK(statuses[1].sequence == 0x71);
  CHECK(statuses.size() == 2);
  CHECK(parser.stats().accepted_status_frames == 2);
  CHECK(parser.buffered_bytes() == 0);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"every byte split boundary", EveryByteSplitBoundary},
      {"byte at a time", ByteAtATime},
      {"coalesced frames", CoalescedFrames},
      {"leading noise and partial magic", LeadingNoiseAndPartialMagic},
      {"bad magic then recovery", BadMagicThenRecovery},
      {"bad checksum then recovery", BadChecksumThenRecovery},
      {"oversize length then recovery", OversizeLengthThenRecovery},
      {"invalid status length frame does not stall recovery",
       InvalidStatusLengthFrameDoesNotStallRecovery},
      {"unsupported command frame does not stall recovery",
       UnsupportedCommandFrameDoesNotStallRecovery},
      {"invalid status length frame does not parse embedded status",
       InvalidStatusLengthFrameDoesNotParseEmbeddedStatus},
      {"unsupported command frame does not parse embedded status",
       UnsupportedCommandFrameDoesNotParseEmbeddedStatus},
      {"reconnect reset drops truncation", ReconnectResetDropsTruncation},
      {"status length is fail closed", StatusLengthIsFailClosed},
      {"unknown command is fail closed", UnknownCommandIsFailClosed},
      {"large noise input remains bounded", LargeNoiseInputRemainsBounded},
      {"handler reentry", HandlerCanReenterParserWithoutRepeatingAcceptedFrame},
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
