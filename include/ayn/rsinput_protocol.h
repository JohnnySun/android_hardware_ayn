// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ayn::rsinput {

constexpr uint8_t kCmdCommod = 0x01;
constexpr uint8_t kCmdStatus = 0x02;
constexpr std::array<uint8_t, 6> kQ9RawPoll = {0x2e, 0x01, 0x02,
                                               0x0a, 0x01, 0x00};
constexpr std::array<uint8_t, 1> kQ9StartPayload = {0x06};
// Keep the stock-compatible report period and enabled-input mask. The smaller
// 0x05/0x01 values produce sequenced status frames with frozen payload data.
constexpr std::array<uint8_t, 10> kSetParametersPayload = {
    0x05, 0x01, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x07,
};

enum class HandshakeState {
  kNotStarted,
  kAwaitingType1,
  kSendConfiguration,
  kAwaitingType2,
  kInitialized,
  kFailed,
};

struct HandshakeStats {
  size_t noise_bytes = 0;
  size_t malformed_frames = 0;
  size_t unrelated_packets = 0;
  size_t accepted_responses = 0;
};

class Q9Handshake {
 public:
  void Start();
  void Feed(const uint8_t* data, size_t size);
  void ConfigurationSent();
  void Fail();

  HandshakeState state() const { return state_; }
  uint8_t expected_response_type() const { return expected_response_type_; }
  bool status_stream_observed() const { return status_stream_observed_; }
  const HandshakeStats& stats() const { return stats_; }

 private:
  void ProcessBuffered();

  std::array<uint8_t, 73> buffer_{};
  size_t buffered_size_ = 0;
  HandshakeState state_ = HandshakeState::kNotStarted;
  uint8_t expected_response_type_ = 0;
  bool status_stream_observed_ = false;
  HandshakeStats stats_{};
};

std::array<std::vector<uint8_t>, 4> BuildQ9HandshakeFrames();

}  // namespace ayn::rsinput
