// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ayn::rsinput {

class StartupPollRxDetector {
 public:
  static constexpr size_t kMaxBufferedBytes = 267;

  void Feed(const uint8_t* data, size_t size);

  bool poll_stop_requested() const { return poll_stop_requested_; }
  uint64_t consume_immediate_poll_requests();
  size_t buffered_bytes() const { return buffered_size_; }

 private:
  void ProcessBuffered();
  void DiscardPrefix(size_t count);

  std::array<uint8_t, kMaxBufferedBytes> buffer_{};
  size_t buffered_size_ = 0;
  size_t discard_remaining_ = 0;
  bool poll_stop_requested_ = false;
  uint64_t immediate_poll_requests_ = 0;
};

}  // namespace ayn::rsinput
