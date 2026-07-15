// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_poll.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace ayn::rsinput {

namespace {

constexpr std::array<uint8_t, 4> kA5Magic = {0xA5, 0xD3, 0x5A, 0x3D};
constexpr std::array<uint8_t, 4> kLegacyMagic = {0x47, 0xB3, 0x57, 0xE2};
constexpr size_t kA5HeaderSize = 8;
constexpr size_t kA5FrameOverhead = 9;
constexpr size_t kA5MaxPayloadSize = 49;
constexpr size_t kLegacyLengthHeaderSize = 6;
constexpr size_t kLegacyFrameOverhead = 12;
constexpr size_t kLegacyRecordSize = 8;

uint16_t ReadLe16(const uint8_t* bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t ReadBe32(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

bool MatchesMagic(const uint8_t* bytes,
                  const std::array<uint8_t, 4>& magic) {
  return std::equal(magic.begin(), magic.end(), bytes);
}

bool IsMagicPrefix(const uint8_t* bytes, size_t size,
                   const std::array<uint8_t, 4>& magic) {
  return std::equal(bytes, bytes + size, magic.begin());
}

}  // namespace

void StartupPollRxDetector::Feed(const uint8_t* data, size_t size) {
  if (data == nullptr) {
    return;
  }

  for (size_t index = 0; index < size; ++index) {
    if (discard_remaining_ != 0) {
      --discard_remaining_;
      continue;
    }

    if (buffered_size_ == buffer_.size()) {
      DiscardPrefix(1);
    }
    buffer_[buffered_size_++] = data[index];
    ProcessBuffered();
  }
}

uint64_t StartupPollRxDetector::consume_immediate_poll_requests() {
  const uint64_t requests = immediate_poll_requests_;
  immediate_poll_requests_ = 0;
  return requests;
}

void StartupPollRxDetector::ProcessBuffered() {
  while (buffered_size_ != 0) {
    size_t magic_position = buffered_size_;
    if (buffered_size_ >= kA5Magic.size()) {
      for (size_t index = 0; index <= buffered_size_ - kA5Magic.size();
           ++index) {
        if (MatchesMagic(buffer_.data() + index, kA5Magic) ||
            MatchesMagic(buffer_.data() + index, kLegacyMagic)) {
          magic_position = index;
          break;
        }
      }
    }

    if (magic_position == buffered_size_) {
      size_t keep = std::min(buffered_size_, kA5Magic.size() - 1);
      while (keep != 0) {
        const uint8_t* suffix = buffer_.data() + buffered_size_ - keep;
        if (IsMagicPrefix(suffix, keep, kA5Magic) ||
            IsMagicPrefix(suffix, keep, kLegacyMagic)) {
          break;
        }
        --keep;
      }
      DiscardPrefix(buffered_size_ - keep);
      return;
    }

    if (magic_position != 0) {
      DiscardPrefix(magic_position);
    }

    if (MatchesMagic(buffer_.data(), kA5Magic)) {
      if (buffered_size_ < kA5HeaderSize) {
        return;
      }

      const size_t payload_size = ReadLe16(buffer_.data() + 6);
      const size_t frame_size = kA5FrameOverhead + payload_size;
      if (payload_size > kA5MaxPayloadSize) {
        discard_remaining_ = frame_size - buffered_size_;
        buffered_size_ = 0;
        return;
      }
      if (buffered_size_ < frame_size) {
        return;
      }

      uint8_t checksum = 0;
      for (size_t index = 4; index + 1 < frame_size; ++index) {
        checksum ^= buffer_[index];
      }
      if (checksum == buffer_[frame_size - 1] && buffer_[5] == 1 &&
          payload_size >= 1 && buffer_[8] == 5) {
        poll_stop_requested_ = true;
      }
      DiscardPrefix(frame_size);
      continue;
    }

    if (buffered_size_ < kLegacyLengthHeaderSize) {
      return;
    }

    const size_t payload_size = buffer_[4];
    const size_t record_count = buffer_[5];
    const size_t frame_size = kLegacyFrameOverhead + payload_size;
    if (record_count * kLegacyRecordSize > payload_size) {
      discard_remaining_ = frame_size - buffered_size_;
      buffered_size_ = 0;
      return;
    }
    if (buffered_size_ < frame_size) {
      return;
    }

    for (size_t record_index = 0; record_index < record_count;
         ++record_index) {
      const uint8_t* record =
          buffer_.data() + kLegacyFrameOverhead +
          record_index * kLegacyRecordSize;
      if (record[0] != 4) {
        continue;
      }

      const uint32_t value = ReadBe32(record + 4);
      if (value == 0) {
        poll_stop_requested_ = true;
      } else if (value == 1 &&
                 immediate_poll_requests_ !=
                     std::numeric_limits<uint64_t>::max()) {
        ++immediate_poll_requests_;
      }
    }
    DiscardPrefix(frame_size);
  }
}

void StartupPollRxDetector::DiscardPrefix(size_t count) {
  if (count >= buffered_size_) {
    buffered_size_ = 0;
    return;
  }
  std::memmove(buffer_.data(), buffer_.data() + count,
               buffered_size_ - count);
  buffered_size_ -= count;
}

}  // namespace ayn::rsinput
