// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_status.h"

#include <charconv>
#include <cstddef>
#include <system_error>

namespace ayn::fan {

namespace {

constexpr size_t kMaximumRawValueBytes = 32;
constexpr char kProductDevice[] = "odin2_mini";
constexpr char kRetroName[] = "Q9";
constexpr char kStatePath[] = "/sys/class/gpio5_pwm2/state";
constexpr char kDutyPath[] = "/sys/class/gpio5_pwm2/duty";

enum class ValueReadResult {
  kAvailable,
  kUnavailable,
  kMalformed,
};

bool ParseNonNegativeDecimal(const std::string& raw, int* value) {
  if (raw.empty() || raw.size() > kMaximumRawValueBytes) {
    return false;
  }

  size_t decimal_size = raw.size();
  if (raw.back() == '\n') {
    --decimal_size;
  }
  if (decimal_size == 0) {
    return false;
  }

  for (size_t index = 0; index < decimal_size; ++index) {
    if (raw[index] < '0' || raw[index] > '9') {
      return false;
    }
  }

  int parsed = 0;
  const char* const begin = raw.data();
  const char* const end = begin + decimal_size;
  const auto conversion = std::from_chars(begin, end, parsed);
  if (conversion.ec != std::errc() || conversion.ptr != end) {
    return false;
  }

  *value = parsed;
  return true;
}

ValueReadResult ReadValue(FanStatusReader read_file, void* reader_context,
                          const std::string& path, int* value) {
  std::string raw;
  if (!read_file(reader_context, path, &raw)) {
    return ValueReadResult::kUnavailable;
  }
  if (!ParseNonNegativeDecimal(raw, value)) {
    return ValueReadResult::kMalformed;
  }
  return ValueReadResult::kAvailable;
}

FanStatusRead FailedRead(ValueReadResult result) {
  return {result == ValueReadResult::kUnavailable
              ? FanStatusResult::kUnavailableRead
              : FanStatusResult::kMalformedValue,
          std::nullopt};
}

}  // namespace

FanStatusRead ReadFanStatus(const FanStatusIdentity& identity,
                            const FanStatusPaths& paths,
                            FanStatusReader read_file,
                            void* reader_context) {
  if (identity.product_device != kProductDevice ||
      identity.retro_name != kRetroName) {
    return {FanStatusResult::kUnsupportedDevice, std::nullopt};
  }
  if (paths.state != kStatePath || paths.duty != kDutyPath) {
    return {FanStatusResult::kUnexpectedPaths, std::nullopt};
  }
  if (read_file == nullptr) {
    return {FanStatusResult::kUnavailableRead, std::nullopt};
  }

  int state = 0;
  const ValueReadResult state_result =
      ReadValue(read_file, reader_context, paths.state, &state);
  if (state_result != ValueReadResult::kAvailable) {
    return FailedRead(state_result);
  }
  if (state != 0 && state != 1) {
    return {FanStatusResult::kMalformedValue, std::nullopt};
  }

  int duty = 0;
  const ValueReadResult duty_result =
      ReadValue(read_file, reader_context, paths.duty, &duty);
  if (duty_result != ValueReadResult::kAvailable) {
    return FailedRead(duty_result);
  }

  return {FanStatusResult::kAvailable, FanStatusSnapshot{state, duty}};
}

}  // namespace ayn::fan
