// SPDX-License-Identifier: Apache-2.0

#include "ayn/lights_lifecycle.h"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}

using ayn::lights::ClampOnceUnlessStopped;
using ayn::lights::ClampResult;
using ayn::lights::kDefaultChannelCap;
using ayn::lights::LoopResult;
using ayn::lights::ParseChannel;
using ayn::lights::RunClampLoopUnlessStopped;
using ayn::lights::StockSysfsPaths;
using ayn::lights::SysfsPaths;

struct Harness {
  std::map<std::string, std::string> files;
  std::vector<std::string> writes;
  bool stop = false;
  int stop_after_passes = -1;
  int passes = 0;
  bool fail_reads = false;
  bool fail_writes = false;
  int sleeps = 0;
  int sleep_until_stop = -1;

  Harness() {
    const SysfsPaths& paths = StockSysfsPaths();
    files[paths.red_brightness] = "0\n";
    files[paths.green_brightness] = "0\n";
    files[paths.blue_brightness] = "0\n";
  }

  void SetChannels(int red, int green, int blue) {
    const SysfsPaths& paths = StockSysfsPaths();
    files[paths.red_brightness] = std::to_string(red) + "\n";
    files[paths.green_brightness] = std::to_string(green) + "\n";
    files[paths.blue_brightness] = std::to_string(blue) + "\n";
  }
};

bool StopRequested(void* context) {
  Harness& harness = *static_cast<Harness*>(context);
  ++harness.passes;
  if (harness.stop_after_passes >= 0 &&
      harness.passes > harness.stop_after_passes) {
    return true;
  }
  return harness.stop;
}

bool ReadFile(void* context, const std::string& path, std::string* value) {
  Harness& harness = *static_cast<Harness*>(context);
  if (harness.fail_reads) {
    return false;
  }
  const auto found = harness.files.find(path);
  if (found == harness.files.end()) {
    return false;
  }
  *value = found->second;
  return true;
}

bool WriteFile(void* context, const std::string& path,
               const std::string& value) {
  Harness& harness = *static_cast<Harness*>(context);
  if (harness.fail_writes) {
    return false;
  }
  harness.writes.push_back(path + "=" + value);
  harness.files[path] = value;
  return true;
}

bool SleepForMilliseconds(void* context, int) {
  Harness& harness = *static_cast<Harness*>(context);
  ++harness.sleeps;
  if (harness.sleep_until_stop >= 0 &&
      harness.sleeps >= harness.sleep_until_stop) {
    harness.stop = true;
  }
  return true;
}

ClampResult RunOnce(Harness& harness, const std::string& device = "odin2_mini",
                    int cap = kDefaultChannelCap) {
  return ClampOnceUnlessStopped(device, StockSysfsPaths(), cap, StopRequested,
                                &harness, ReadFile, &harness, WriteFile,
                                &harness);
}

void FullWhiteIsClampedOnce() {
  Harness harness;
  harness.SetChannels(255, 255, 255);
  CHECK(RunOnce(harness) == ClampResult::kClamped);
  CHECK(harness.writes.size() == 3);
  CHECK(harness.files[StockSysfsPaths().red_brightness] == "1\n");
}

void AnAlreadyDimLedIsNeverWritten() {
  Harness harness;
  harness.SetChannels(1, 1, 1);
  CHECK(RunOnce(harness) == ClampResult::kAlreadyWithinCap);
  CHECK(harness.writes.empty());
}

void AnUnlitLedIsNeverWritten() {
  Harness harness;
  CHECK(RunOnce(harness) == ClampResult::kAlreadyWithinCap);
  CHECK(harness.writes.empty());
}

void OnlyChangedChannelsAreWritten() {
  Harness harness;
  // Blue is already at the clamped value, so it must not be rewritten.
  harness.SetChannels(255, 255, 0);
  CHECK(RunOnce(harness) == ClampResult::kClamped);
  CHECK(harness.writes.size() == 2);
  CHECK(harness.files[StockSysfsPaths().blue_brightness] == "0\n");
}

void AnUnknownDeviceWritesNothing() {
  Harness harness;
  harness.SetChannels(255, 255, 255);
  CHECK(RunOnce(harness, "odin2") == ClampResult::kFailedClosed);
  CHECK(harness.writes.empty());
}

void MovedPathsWriteNothing() {
  Harness harness;
  harness.SetChannels(255, 255, 255);
  SysfsPaths moved = StockSysfsPaths();
  moved.red_brightness = "/data/local/tmp/red";
  const ClampResult result = ClampOnceUnlessStopped(
      "odin2_mini", moved, kDefaultChannelCap, StopRequested, &harness,
      ReadFile, &harness, WriteFile, &harness);
  CHECK(result == ClampResult::kFailedClosed);
  CHECK(harness.writes.empty());
}

void AnUnreadableChannelWritesNothing() {
  Harness harness;
  harness.SetChannels(255, 255, 255);
  harness.fail_reads = true;
  CHECK(RunOnce(harness) == ClampResult::kFailedClosed);
  CHECK(harness.writes.empty());
}

void AMalformedChannelWritesNothing() {
  Harness harness;
  harness.files[StockSysfsPaths().green_brightness] = "bright\n";
  CHECK(RunOnce(harness) == ClampResult::kFailedClosed);
  CHECK(harness.writes.empty());
}

void AFailedWriteStopsTheRest() {
  Harness harness;
  harness.SetChannels(255, 255, 255);
  harness.fail_writes = true;
  CHECK(RunOnce(harness) == ClampResult::kFailedClosed);
  CHECK(harness.writes.empty());
}

void StoppingIsHonouredBeforeAnyWrite() {
  Harness harness;
  harness.SetChannels(255, 255, 255);
  harness.stop = true;
  CHECK(RunOnce(harness) == ClampResult::kStopped);
  CHECK(harness.writes.empty());
}

void ChannelParsingRejectsRubbish() {
  int value = -1;
  CHECK(ParseChannel("0\n", &value) && value == 0);
  CHECK(ParseChannel("255\n", &value) && value == 255);
  CHECK(ParseChannel("  7\n", &value) && value == 7);
  CHECK(!ParseChannel("", &value));
  CHECK(!ParseChannel("\n", &value));
  CHECK(!ParseChannel("-1\n", &value));
  CHECK(!ParseChannel("256\n", &value));
  CHECK(!ParseChannel("12x\n", &value));
  CHECK(!ParseChannel("bright\n", &value));
}

void TheLoopSettlesAfterOneClamp() {
  Harness harness;
  harness.SetChannels(255, 255, 255);
  harness.sleep_until_stop = 3;
  const LoopResult result = RunClampLoopUnlessStopped(
      "odin2_mini", StockSysfsPaths(), kDefaultChannelCap, StopRequested,
      &harness, ReadFile, &harness, WriteFile, &harness, SleepForMilliseconds,
      &harness);
  CHECK(result == LoopResult::kStopped);
  // Three writes for the first pass and nothing after: the clamp does not
  // answer its own write.
  CHECK(harness.writes.size() == 3);
}

void TheLoopFailsClosedOnABadRead() {
  Harness harness;
  harness.fail_reads = true;
  const LoopResult result = RunClampLoopUnlessStopped(
      "odin2_mini", StockSysfsPaths(), kDefaultChannelCap, StopRequested,
      &harness, ReadFile, &harness, WriteFile, &harness, SleepForMilliseconds,
      &harness);
  CHECK(result == LoopResult::kFailedClosed);
  CHECK(harness.writes.empty());
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests = {
      {"full white is clamped once", FullWhiteIsClampedOnce},
      {"an already dim LED is never written", AnAlreadyDimLedIsNeverWritten},
      {"an unlit LED is never written", AnUnlitLedIsNeverWritten},
      {"only changed channels are written", OnlyChangedChannelsAreWritten},
      {"an unknown device writes nothing", AnUnknownDeviceWritesNothing},
      {"moved paths write nothing", MovedPathsWriteNothing},
      {"an unreadable channel writes nothing", AnUnreadableChannelWritesNothing},
      {"a malformed channel writes nothing", AMalformedChannelWritesNothing},
      {"a failed write stops the rest", AFailedWriteStopsTheRest},
      {"stopping is honoured before any write",
       StoppingIsHonouredBeforeAnyWrite},
      {"channel parsing rejects rubbish", ChannelParsingRejectsRubbish},
      {"the loop settles after one clamp", TheLoopSettlesAfterOneClamp},
      {"the loop fails closed on a bad read", TheLoopFailsClosedOnABadRead},
  };

  size_t passed = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      ++passed;
      std::cout << "[PASS] " << test.first << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[FAIL] " << test.first << ": " << error.what() << '\n';
      return 1;
    }
  }
  std::cout << passed << " tests passed\n";
  return 0;
}
