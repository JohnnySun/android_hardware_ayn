// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_protocol.h"

#include "ayn/rsinput_parser.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ayn::rsinput {

namespace {

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

std::array<std::vector<uint8_t>, 2> BuildInitializationFrames() {
  return {BuildCommandFrame(0, kVersionRequestPayload.data(),
                            kVersionRequestPayload.size()),
          BuildCommandFrame(1, kSetParametersPayload.data(),
                            kSetParametersPayload.size())};
}

}  // namespace ayn::rsinput
