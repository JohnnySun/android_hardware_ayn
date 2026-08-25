// SPDX-License-Identifier: Apache-2.0

#include "ayn/performance_adapter.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}

void BoundedReadAdapterReadsExactContents() {
  char path[] = "/tmp/ayn-performance-adapter.XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  const std::string expected = "680000000\n";
  CHECK(write(fd, expected.data(), expected.size()) ==
        static_cast<ssize_t>(expected.size()));
  CHECK(close(fd) == 0);

  std::string observed;
  CHECK(ayn::performance::ReadPosixFile(nullptr, path, &observed));
  CHECK(observed == expected);
  CHECK(unlink(path) == 0);
}

void AdapterRejectsInvalidBuffersAndMissingFiles() {
  std::string observed;
  CHECK(!ayn::performance::ReadPosixFile(nullptr, "/does/not/exist",
                                        &observed));
  CHECK(!ayn::performance::ReadPosixFile(nullptr, "/does/not/exist",
                                        nullptr));
}

constexpr std::array<const char*, 10> kExpectedWriterPaths = {{
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
    "/sys/devices/system/cpu/cpu3/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu3/cpufreq/scaling_max_freq",
    "/sys/devices/system/cpu/cpu7/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu7/cpufreq/scaling_max_freq",
    "/sys/class/kgsl/kgsl-3d0/devfreq/min_freq",
    "/sys/class/kgsl/kgsl-3d0/devfreq/max_freq",
    "/sys/devices/system/cpu/bus_dcvs/DDR/hw_min_freq",
    "/sys/devices/system/cpu/bus_dcvs/DDR/soc:qcom,memlat:ddr:gold/min_freq",
}};

struct WriterHarness {
  std::string opened_path;
  std::string written;
  size_t open_calls = 0;
  size_t write_calls = 0;
  size_t close_calls = 0;
  size_t readback_calls = 0;
  size_t open_eintr_count = 0;
  size_t write_eintr_count = 0;
  size_t maximum_write = 1024;
  bool close_fails = false;
  bool readback_fails = false;
  bool readback_mismatch = false;
};

int OpenWriter(void* context, const char* path) {
  auto* harness = static_cast<WriterHarness*>(context);
  ++harness->open_calls;
  if (harness->open_eintr_count > 0) {
    --harness->open_eintr_count;
    errno = EINTR;
    return -1;
  }
  harness->opened_path = path;
  return 42;
}

ssize_t WriteWriter(void* context, int fd, const void* data, size_t size) {
  auto* harness = static_cast<WriterHarness*>(context);
  CHECK(fd == 42);
  ++harness->write_calls;
  if (harness->write_eintr_count > 0) {
    --harness->write_eintr_count;
    errno = EINTR;
    return -1;
  }
  const size_t count = std::min(size, harness->maximum_write);
  harness->written.append(static_cast<const char*>(data), count);
  return static_cast<ssize_t>(count);
}

int CloseWriter(void* context, int fd) {
  auto* harness = static_cast<WriterHarness*>(context);
  CHECK(fd == 42);
  ++harness->close_calls;
  return harness->close_fails ? -1 : 0;
}

bool ReadWriter(void* context, const std::string& path, std::string* value) {
  auto* harness = static_cast<WriterHarness*>(context);
  ++harness->readback_calls;
  CHECK(path == harness->opened_path);
  if (harness->readback_fails) {
    return false;
  }
  *value = harness->readback_mismatch ? "1\n" : harness->written + "\n";
  return true;
}

ayn::performance::PosixWriterOperations Operations(WriterHarness* harness) {
  return {harness, OpenWriter, WriteWriter, CloseWriter, ReadWriter};
}

void WriterAllowsExactlyTheStockTenPaths() {
  for (const char* path : kExpectedWriterPaths) {
    WriterHarness harness;
    auto operations = Operations(&harness);
    CHECK(ayn::performance::WritePosixFile(&operations, path, "902400"));
    CHECK(harness.opened_path == path);
    // The trailing newline is required: the DDR floor node returns EIO for a
    // value written without one.
    CHECK(harness.written == "902400\n");
  }

  WriterHarness harness;
  auto operations = Operations(&harness);
  CHECK(!ayn::performance::WritePosixFile(
      &operations, "/sys/devices/system/cpu/cpu1/online", "1"));
  CHECK(harness.open_calls == 0);
}

void WriterRetriesEintrAndCompletesShortWrites() {
  WriterHarness harness;
  harness.open_eintr_count = 1;
  harness.write_eintr_count = 1;
  harness.maximum_write = 2;
  auto operations = Operations(&harness);

  CHECK(ayn::performance::WritePosixFile(
      &operations, kExpectedWriterPaths[0], "902400"));
  CHECK(harness.open_calls == 2);
  // Seven payload bytes in two-byte chunks is four writes, plus the one that
  // reported EINTR.
  CHECK(harness.write_calls == 5);
  CHECK(harness.written == "902400\n");
  CHECK(harness.close_calls == 1);
  CHECK(harness.readback_calls == 1);
}

// A close failure or an unreadable node leaves the outcome unknown, so both
// stay fail-closed.
void WriterFailsClosedWhenTheOutcomeIsUnknown() {
  WriterHarness close_failure;
  close_failure.close_fails = true;
  auto close_operations = Operations(&close_failure);
  CHECK(!ayn::performance::WritePosixFile(
      &close_operations, kExpectedWriterPaths[0], "902400"));
  CHECK(close_failure.readback_calls == 0);

  WriterHarness read_failure;
  read_failure.readback_fails = true;
  auto read_operations = Operations(&read_failure);
  CHECK(!ayn::performance::WritePosixFile(
      &read_operations, kExpectedWriterPaths[0], "902400"));

}

// A value that reads back different is a known outcome, not an unknown one:
// the write landed and something else moved it. On this device that something
// is the QTI perf stack, which resets the cpufreq limits, and the stock daemon
// answers by rewriting every second rather than by giving up. Treating it as a
// failure here made every mode change fail closed on a write that had worked.
void WriterAcceptsAValueSomethingElseMoved() {
  WriterHarness mismatch;
  mismatch.readback_mismatch = true;
  auto mismatch_operations = Operations(&mismatch);
  CHECK(ayn::performance::WritePosixFile(
      &mismatch_operations, kExpectedWriterPaths[0], "902400"));
  CHECK(mismatch.readback_calls == 1);
}

}  // namespace

int main() {
  try {
    BoundedReadAdapterReadsExactContents();
    AdapterRejectsInvalidBuffersAndMissingFiles();
    WriterAllowsExactlyTheStockTenPaths();
    WriterRetriesEintrAndCompletesShortWrites();
    WriterFailsClosedWhenTheOutcomeIsUnknown();
    WriterAcceptsAValueSomethingElseMoved();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "performance_adapter_test: PASS\n";
  return 0;
}
