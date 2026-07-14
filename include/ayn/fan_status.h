// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>

namespace ayn::fan {

using FanStatusReader = bool (*)(void* context, const std::string& path,
                                 std::string* value);

enum class FanStatusResult {
  kAvailable,
  kUnsupportedDevice,
  kUnexpectedPaths,
  kUnavailableRead,
  kMalformedValue,
};

struct FanStatusIdentity {
  std::string product_device;
  std::string retro_name;
};

struct FanStatusPaths {
  std::string state;
  std::string duty;
};

struct FanStatusSnapshot {
  int state;
  int duty;

  bool operator==(const FanStatusSnapshot& other) const {
    return state == other.state && duty == other.duty;
  }
};

struct FanStatusRead {
  FanStatusResult result;
  std::optional<FanStatusSnapshot> snapshot;
};

FanStatusRead ReadFanStatus(const FanStatusIdentity& identity,
                            const FanStatusPaths& paths,
                            FanStatusReader read_file,
                            void* reader_context);

}  // namespace ayn::fan
