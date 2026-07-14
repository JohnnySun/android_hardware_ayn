// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_protocol.h"

#include "ayn/rsinput_parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ayn::rsinput {

namespace {

static_assert(kCmdStatus == Parser::kCmdStatus);

std::vector<uint8_t> BuildCommandFrame(uint8_t sequence,
                                       const uint8_t* payload,
                                       size_t payload_size) {
  std::vector<uint8_t> frame(Parser::kMagic.begin(), Parser::kMagic.end());
  frame.push_back(sequence);
  frame.push_back(kCmdCommod);
  frame.push_back(static_cast<uint8_t>(payload_size & 0xff));
  frame.push_back(static_cast<uint8_t>(payload_size >> 8));
  frame.insert(frame.end(), payload, payload + payload_size);

  uint8_t checksum = 0;
  for (size_t index = 4; index < frame.size(); ++index) {
    checksum ^= frame[index];
  }
  frame.push_back(checksum);
  return frame;
}

}  // namespace

void Q9Handshake::Start() {
  if (state_ != HandshakeState::kNotStarted) {
    Fail();
    return;
  }
  state_ = HandshakeState::kAwaitingType1;
  expected_response_type_ = kCmdCommod;
}

void Q9Handshake::Feed(const uint8_t* data, size_t size) {
  if (data == nullptr && size != 0) {
    Fail();
    return;
  }

  for (size_t index = 0; index < size; ++index) {
    if (buffered_size_ == buffer_.size()) {
      ++stats_.noise_bytes;
      std::memmove(buffer_.data(), buffer_.data() + 1, buffered_size_ - 1);
      --buffered_size_;
    }
    buffer_[buffered_size_++] = data[index];
    ProcessBuffered();
  }
  ProcessBuffered();
}

void Q9Handshake::ConfigurationSent() {
  if (state_ != HandshakeState::kSendConfiguration) {
    Fail();
    return;
  }
  buffered_size_ = 0;
  state_ = HandshakeState::kAwaitingType2;
  expected_response_type_ = kCmdStatus;
}

void Q9Handshake::Fail() {
  state_ = HandshakeState::kFailed;
}

void Q9Handshake::ProcessBuffered() {
  if (state_ != HandshakeState::kAwaitingType1 &&
      state_ != HandshakeState::kAwaitingType2) {
    return;
  }

  const auto discard_prefix = [this](size_t count) {
    if (count >= buffered_size_) {
      buffered_size_ = 0;
      return;
    }
    std::memmove(buffer_.data(), buffer_.data() + count,
                 buffered_size_ - count);
    buffered_size_ -= count;
  };

  while (buffered_size_ != 0) {
    size_t magic_position = buffered_size_;
    if (buffered_size_ >= Parser::kMagic.size()) {
      for (size_t index = 0;
           index <= buffered_size_ - Parser::kMagic.size(); ++index) {
        if (std::equal(Parser::kMagic.begin(), Parser::kMagic.end(),
                       buffer_.begin() + index)) {
          magic_position = index;
          break;
        }
      }
    }

    if (magic_position == buffered_size_) {
      size_t keep = std::min(buffered_size_, Parser::kMagic.size() - 1);
      while (keep != 0 &&
             !std::equal(buffer_.begin() + buffered_size_ - keep,
                         buffer_.begin() + buffered_size_,
                         Parser::kMagic.begin())) {
        --keep;
      }
      const size_t discarded = buffered_size_ - keep;
      stats_.noise_bytes += discarded;
      discard_prefix(discarded);
      return;
    }

    if (magic_position != 0) {
      stats_.noise_bytes += magic_position;
      discard_prefix(magic_position);
    }

    constexpr size_t kHeaderSize = 8;
    if (buffered_size_ < kHeaderSize) {
      return;
    }
    const size_t payload_size = static_cast<size_t>(buffer_[6]) |
                                (static_cast<size_t>(buffer_[7]) << 8);
    if (payload_size > Parser::kMaxPayloadSize) {
      ++stats_.malformed_frames;
      discard_prefix(1);
      continue;
    }
    const size_t frame_size = Parser::kFrameOverhead + payload_size;
    if (buffered_size_ < frame_size) {
      return;
    }

    uint8_t checksum = 0;
    for (size_t index = 4; index + 1 < frame_size; ++index) {
      checksum ^= buffer_[index];
    }
    if (checksum != buffer_[frame_size - 1]) {
      ++stats_.malformed_frames;
      discard_prefix(1);
      continue;
    }

    const uint8_t response_type = buffer_[5];
    discard_prefix(frame_size);
    if (response_type != expected_response_type_) {
      ++stats_.unrelated_packets;
      continue;
    }

    ++stats_.accepted_responses;
    if (state_ == HandshakeState::kAwaitingType1) {
      state_ = HandshakeState::kSendConfiguration;
    } else {
      state_ = HandshakeState::kInitialized;
      expected_response_type_ = 0;
    }
    return;
  }
}

std::array<std::vector<uint8_t>, 3> BuildQ9HandshakeFrames() {
  return {BuildCommandFrame(1, kQ9StartPayload.data(),
                            kQ9StartPayload.size()),
          BuildCommandFrame(2, kSetParametersPayload.data(),
                            kSetParametersPayload.size()),
          BuildCommandFrame(3, kQ9StartPayload.data(),
                            kQ9StartPayload.size())};
}

}  // namespace ayn::rsinput
