// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace ayn::rsinput {

constexpr uint8_t kCmdCommod = 0x01;
constexpr std::array<uint8_t, 1> kVersionRequestPayload = {0x02};
constexpr std::array<uint8_t, 10> kSetParametersPayload = {
    0x05, 0x01, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x07,
};

std::array<std::vector<uint8_t>, 2> BuildInitializationFrames();

}  // namespace ayn::rsinput
