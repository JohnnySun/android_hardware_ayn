// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ayn::rsinput {

struct Status {
  uint8_t sequence = 0;
  uint16_t buttons = 0;
  uint16_t left_trigger = 0;
  uint16_t right_trigger = 0;
  int16_t left_x = 0;
  int16_t left_y = 0;
  int16_t right_x = 0;
  int16_t right_y = 0;
};

class Parser {
 public:
  static constexpr std::array<uint8_t, 4> kMagic = {0xA5, 0xD3, 0x5A, 0x3D};
  static constexpr uint8_t kCmdStatus = 0x02;
  static constexpr size_t kStatusPayloadSize = 14;
  static constexpr size_t kMaxPayloadSize = 64;
  static constexpr size_t kFrameOverhead = 9;
  static constexpr size_t kMaxFrameSize = kFrameOverhead + kMaxPayloadSize;

  struct Stats {
    size_t noise_bytes = 0;
    size_t bad_checksums = 0;
    size_t oversize_lengths = 0;
    size_t unsupported_commands = 0;
    size_t invalid_status_lengths = 0;
    size_t accepted_status_frames = 0;
  };

  using StatusHandler = void (*)(void* context, const Status& status);

  void Feed(const uint8_t* data, size_t size, StatusHandler handler,
            void* context);
  void Reset();

  const Stats& stats() const { return stats_; }
  size_t buffered_bytes() const { return buffered_size_; }

 private:
  std::array<uint8_t, kMaxFrameSize> buffer_{};
  size_t buffered_size_ = 0;
  Stats stats_{};
};

}  // namespace ayn::rsinput
