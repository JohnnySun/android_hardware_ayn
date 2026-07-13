// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_parser.h"

#include <algorithm>
#include <cstring>

namespace ayn::rsinput {

namespace {

uint16_t ReadLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

int16_t ReadLeI16(const uint8_t* bytes) {
  const uint16_t value = ReadLe16(bytes);
  if ((value & 0x8000u) == 0) {
    return static_cast<int16_t>(value);
  }
  return static_cast<int16_t>(static_cast<int32_t>(value) - 0x10000);
}

}  // namespace

void Parser::Feed(const uint8_t* data, size_t size, StatusHandler handler,
                  void* context) {
  if (data == nullptr) {
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

  const auto process_buffer = [this, handler, context, &discard_prefix]() {
    while (buffered_size_ != 0) {
      size_t magic_position = buffered_size_;
      if (buffered_size_ >= kMagic.size()) {
        for (size_t i = 0; i <= buffered_size_ - kMagic.size(); ++i) {
          if (std::equal(kMagic.begin(), kMagic.end(), buffer_.begin() + i)) {
            magic_position = i;
            break;
          }
        }
      }

      if (magic_position == buffered_size_) {
        size_t keep = std::min(buffered_size_, kMagic.size() - 1);
        while (keep != 0 &&
               !std::equal(buffer_.begin() + buffered_size_ - keep,
                           buffer_.begin() + buffered_size_, kMagic.begin())) {
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

      const size_t payload_size = ReadLe16(buffer_.data() + 6);
      if (payload_size > kMaxPayloadSize) {
        ++stats_.oversize_lengths;
        discard_prefix(1);
        continue;
      }

      const uint8_t command = buffer_[5];
      if (command != kCmdStatus) {
        ++stats_.unsupported_commands;
        discard_prefix(1);
        continue;
      }
      if (payload_size != kStatusPayloadSize) {
        ++stats_.invalid_status_lengths;
        discard_prefix(1);
        continue;
      }

      const size_t frame_size = kFrameOverhead + payload_size;
      if (buffered_size_ < frame_size) {
        return;
      }

      uint8_t checksum = 0;
      for (size_t i = 4; i + 1 < frame_size; ++i) {
        checksum ^= buffer_[i];
      }
      if (checksum != buffer_[frame_size - 1]) {
        ++stats_.bad_checksums;
        discard_prefix(1);
        continue;
      }

      const uint8_t* payload = buffer_.data() + 8;
      Status status;
      status.sequence = buffer_[4];
      status.buttons = ReadLe16(payload);
      status.left_trigger = ReadLe16(payload + 2);
      status.right_trigger = ReadLe16(payload + 4);
      status.left_x = ReadLeI16(payload + 6);
      status.left_y = ReadLeI16(payload + 8);
      status.right_x = ReadLeI16(payload + 10);
      status.right_y = ReadLeI16(payload + 12);
      ++stats_.accepted_status_frames;
      if (handler != nullptr) {
        handler(context, status);
      }

      discard_prefix(frame_size);
    }
  };

  for (size_t i = 0; i < size; ++i) {
    if (buffered_size_ == buffer_.size()) {
      ++stats_.noise_bytes;
      discard_prefix(1);
    }
    buffer_[buffered_size_++] = data[i];
    process_buffer();
  }
}

void Parser::Reset() {
  buffered_size_ = 0;
}

}  // namespace ayn::rsinput
