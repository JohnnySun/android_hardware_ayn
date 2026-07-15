// SPDX-License-Identifier: Apache-2.0

#include "ayn/performance_service.h"

#include <array>
#include <cstddef>
#include <exception>
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

using ayn::performance::DeviceIdentity;
using ayn::performance::PerformanceMode;
using ayn::performance::PerformanceResult;
using ayn::performance::PerformanceService;
using ayn::performance::SysfsPaths;

constexpr size_t kNodeCount = ayn::performance::kPerformanceNodeCount;

const SysfsPaths kExpectedPaths = ayn::performance::StockSysfsPaths();
const std::array<uint64_t, kNodeCount> kBaseline = {
    556800, 2016000, 614400, 2803200, 864000,
    3187200, 124800000, 680000000, 547000, 547000,
};

const std::array<uint64_t, kNodeCount> kStockNormal = {
    902400, 2016000, 1651200, 2803200, 1843200,
    3187200, 401000000, 680000000, 4224000, 4224000,
};

const std::array<uint64_t, kNodeCount> kPerformance = {
    1228800, 2016000, 2054400, 2803200, 2476800,
    3187200, 680000000, 680000000, 4224000, 4224000,
};

const std::array<uint64_t, kNodeCount> kHigh = {
    2016000, 2016000, 2803200, 2803200, 3187200,
    3187200, 680000000, 680000000, 4224000, 4224000,
};

struct Harness {
  std::map<std::string, std::string> files;
  std::vector<std::string> events;
  size_t write_count = 0;
  size_t fail_write_at = 0;
  size_t corrupt_readback_at = 0;
  size_t pending_corrupt_write = 0;
  size_t fail_read_at = 0;
  size_t read_count = 0;
  size_t fail_rollback_write_at = 0;
  bool transaction_failed = false;

  Harness() {
    for (size_t index = 0; index < kNodeCount; ++index) {
      files.emplace(kExpectedPaths.nodes[index],
                    std::to_string(kBaseline[index]) + "\n");
    }
  }
};

bool ReadFile(void* context, const std::string& path, std::string* value) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->read_count;
  harness->events.push_back("R " + path);
  if (harness->fail_read_at == harness->read_count) {
    return false;
  }
  const auto found = harness->files.find(path);
  if (found == harness->files.end()) {
    return false;
  }
  if (harness->pending_corrupt_write != 0 &&
      harness->pending_corrupt_write == harness->write_count) {
    harness->pending_corrupt_write = 0;
    harness->transaction_failed = true;
    *value = "1\n";
    return true;
  }
  *value = found->second;
  return true;
}

bool WriteFile(void* context, const std::string& path,
               const std::string& value) {
  auto* harness = static_cast<Harness*>(context);
  ++harness->write_count;
  harness->events.push_back("W " + path + "=" + value);
  if (harness->fail_write_at == harness->write_count) {
    harness->transaction_failed = true;
    return false;
  }
  if (harness->transaction_failed && harness->fail_rollback_write_at != 0 &&
      harness->write_count == harness->fail_rollback_write_at) {
    return false;
  }
  harness->files[path] = value + "\n";
  if (harness->corrupt_readback_at == harness->write_count) {
    harness->pending_corrupt_write = harness->write_count;
  }
  return true;
}

PerformanceService Service(
    Harness* harness,
    const DeviceIdentity& identity = {
        "odin2_mini", "lineage_odin2_mini", "Odin2 Mini"},
    const SysfsPaths& paths = kExpectedPaths) {
  return PerformanceService(identity, paths, ReadFile, harness, WriteFile,
                            harness);
}

std::array<uint64_t, kNodeCount> Values(const Harness& harness) {
  std::array<uint64_t, kNodeCount> values{};
  for (size_t index = 0; index < kNodeCount; ++index) {
    values[index] = std::stoull(harness.files.at(kExpectedPaths.nodes[index]));
  }
  return values;
}

void ExactIdentityAndPathsGateAllIo() {
  for (size_t index = 0; index < 3; ++index) {
    Harness harness;
    DeviceIdentity identity = {
        "odin2_mini", "lineage_odin2_mini", "Odin2 Mini"};
    std::string* fields[] = {&identity.product_device, &identity.product_name,
                             &identity.vendor_model};
    *fields[index] += ".wrong";
    PerformanceService service = Service(&harness, identity);
    CHECK(service.Initialize().result ==
          PerformanceResult::kUnsupportedDevice);
    CHECK(harness.events.empty());
  }

  for (size_t index = 0; index < kNodeCount; ++index) {
    Harness harness;
    SysfsPaths paths = kExpectedPaths;
    paths.nodes[index] += ".wrong";
    PerformanceService service = Service(
        &harness,
        {"odin2_mini", "lineage_odin2_mini", "Odin2 Mini"}, paths);
    CHECK(service.Initialize().result == PerformanceResult::kUnexpectedPaths);
    CHECK(harness.events.empty());
  }
}

void InitializationCapturesAllTenBaselineNodesWithoutWriting() {
  Harness harness;
  PerformanceService service = Service(&harness);
  const auto response = service.Initialize();
  CHECK(response.result == PerformanceResult::kOk);
  CHECK(response.active_mode == PerformanceMode::kSystemManaged);
  CHECK(response.initialized);
  CHECK(harness.read_count == kNodeCount);
  CHECK(harness.write_count == 0);

  CHECK(service.Initialize().result == PerformanceResult::kAlreadyInitialized);
  CHECK(harness.read_count == kNodeCount);
  CHECK(harness.write_count == 0);
}

void InitializationRequiresACompleteStrictNumericSnapshot() {
  for (size_t index = 0; index < kNodeCount; ++index) {
    Harness harness;
    harness.files[kExpectedPaths.nodes[index]] = "not-a-number\n";
    PerformanceService service = Service(&harness);
    CHECK(service.Initialize().result == PerformanceResult::kReadFailed);
    CHECK(harness.write_count == 0);
  }
}

void EachControlledModeWritesExactContractAndReadsBackEveryWrite() {
  const std::array<std::pair<PerformanceMode,
                             std::array<uint64_t, kNodeCount>>, 3>
      cases = {{{PerformanceMode::kStockNormal, kStockNormal},
                {PerformanceMode::kPerformance, kPerformance},
                {PerformanceMode::kHigh, kHigh}}};

  for (const auto& [mode, expected] : cases) {
    Harness harness;
    PerformanceService service = Service(&harness);
    CHECK(service.Initialize().result == PerformanceResult::kOk);
    harness.events.clear();
    harness.read_count = 0;

    const auto response = service.SetMode(mode);
    CHECK(response.result == PerformanceResult::kOk);
    CHECK(response.active_mode == mode);
    CHECK(Values(harness) == expected);
    CHECK(harness.write_count == kNodeCount);
    CHECK(harness.read_count == kNodeCount * 2);
    CHECK(harness.events.size() == kNodeCount * 3);
    for (size_t index = kNodeCount; index < harness.events.size(); index += 2) {
      CHECK(harness.events[index].rfind("W ", 0) == 0);
      CHECK(harness.events[index + 1].rfind("R ", 0) == 0);
      CHECK(harness.events[index].substr(2,
                                         harness.events[index].find('=') - 2) ==
            harness.events[index + 1].substr(2));
    }
  }
}

void InvalidOrPrematureRequestsNeverWrite() {
  Harness harness;
  PerformanceService service = Service(&harness);
  CHECK(service.GetStatus().result == PerformanceResult::kNotInitialized);
  CHECK(service.SetMode(PerformanceMode::kHigh).result ==
        PerformanceResult::kNotInitialized);
  CHECK(harness.events.empty());
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  const size_t events = harness.events.size();
  CHECK(service.SetMode(static_cast<PerformanceMode>(99)).result ==
        PerformanceResult::kInvalidMode);
  CHECK(harness.events.size() == events);
  CHECK(harness.write_count == 0);
}

void EveryWriteFailureRollsBackTheCompleteTouchedPrefix() {
  for (size_t fail_at = 1; fail_at <= kNodeCount; ++fail_at) {
    Harness harness;
    PerformanceService service = Service(&harness);
    CHECK(service.Initialize().result == PerformanceResult::kOk);
    harness.fail_write_at = fail_at;
    const auto response = service.SetMode(PerformanceMode::kHigh);
    CHECK(response.result == PerformanceResult::kWriteFailed);
    CHECK(response.active_mode == PerformanceMode::kSystemManaged);
    CHECK(Values(harness) == kBaseline);
    CHECK(harness.write_count == fail_at * 2);
  }
}

void EveryReadbackFailureRollsBackIncludingTheJustWrittenNode() {
  for (size_t fail_at = 1; fail_at <= kNodeCount; ++fail_at) {
    Harness harness;
    PerformanceService service = Service(&harness);
    CHECK(service.Initialize().result == PerformanceResult::kOk);
    harness.corrupt_readback_at = fail_at;
    const auto response = service.SetMode(PerformanceMode::kPerformance);
    CHECK(response.result == PerformanceResult::kReadbackFailed);
    CHECK(response.active_mode == PerformanceMode::kSystemManaged);
    CHECK(Values(harness) == kBaseline);
    CHECK(harness.write_count == fail_at * 2);
  }
}

void FailedModeTransitionRestoresTheTransactionSnapshot() {
  Harness harness;
  PerformanceService service = Service(&harness);
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  CHECK(service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kOk);
  CHECK(Values(harness) == kStockNormal);
  harness.write_count = 0;
  harness.fail_write_at = 7;

  const auto response = service.SetMode(PerformanceMode::kHigh);
  CHECK(response.result == PerformanceResult::kWriteFailed);
  CHECK(response.active_mode == PerformanceMode::kStockNormal);
  CHECK(Values(harness) == kStockNormal);
}

void SystemManagedRestoresTheFullStartupBaseline() {
  Harness harness;
  PerformanceService service = Service(&harness);
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  CHECK(service.SetMode(PerformanceMode::kHigh).result ==
        PerformanceResult::kOk);
  harness.events.clear();
  harness.read_count = 0;
  harness.write_count = 0;

  const auto response = service.SetMode(PerformanceMode::kSystemManaged);
  CHECK(response.result == PerformanceResult::kOk);
  CHECK(response.active_mode == PerformanceMode::kSystemManaged);
  CHECK(Values(harness) == kBaseline);
  CHECK(harness.write_count == kNodeCount);
  CHECK(harness.read_count == kNodeCount * 2);
}

void RollbackFailureIsReportedAndBlocksFurtherWrites() {
  Harness harness;
  PerformanceService service = Service(&harness);
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  harness.corrupt_readback_at = 3;
  harness.fail_rollback_write_at = 4;
  CHECK(service.SetMode(PerformanceMode::kHigh).result ==
        PerformanceResult::kRollbackFailed);
  CHECK(harness.write_count == 6);
  const size_t writes = harness.write_count;
  CHECK(service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kRollbackFailed);
  CHECK(harness.write_count == writes);
}

void RollbackFailureStillAllowsExactBaselineRecovery() {
  Harness harness;
  PerformanceService service = Service(&harness);
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  harness.corrupt_readback_at = 3;
  harness.fail_rollback_write_at = 4;
  CHECK(service.SetMode(PerformanceMode::kHigh).result ==
        PerformanceResult::kRollbackFailed);

  harness.fail_rollback_write_at = 0;
  harness.corrupt_readback_at = 0;
  const size_t writes_before_recovery = harness.write_count;
  const auto recovery = service.SetMode(PerformanceMode::kSystemManaged);
  CHECK(recovery.result == PerformanceResult::kOk);
  CHECK(recovery.active_mode == PerformanceMode::kSystemManaged);
  CHECK(Values(harness) == kBaseline);
  CHECK(harness.write_count == writes_before_recovery + kNodeCount);
  CHECK(service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kOk);
}

void FailedBaselineRecoveryAttemptsEveryNodeAndKeepsLatch() {
  Harness harness;
  PerformanceService service = Service(&harness);
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  harness.corrupt_readback_at = 3;
  harness.fail_rollback_write_at = 4;
  CHECK(service.SetMode(PerformanceMode::kHigh).result ==
        PerformanceResult::kRollbackFailed);

  harness.fail_rollback_write_at = harness.write_count + 2;
  harness.corrupt_readback_at = 0;
  const size_t writes_before_recovery = harness.write_count;
  CHECK(service.SetMode(PerformanceMode::kSystemManaged).result ==
        PerformanceResult::kRollbackFailed);
  CHECK(harness.write_count == writes_before_recovery + kNodeCount);
  const size_t writes_after_recovery = harness.write_count;
  CHECK(service.SetMode(PerformanceMode::kPerformance).result ==
        PerformanceResult::kRollbackFailed);
  CHECK(harness.write_count == writes_after_recovery);
}

}  // namespace

int main() {
  try {
    ExactIdentityAndPathsGateAllIo();
    InitializationCapturesAllTenBaselineNodesWithoutWriting();
    InitializationRequiresACompleteStrictNumericSnapshot();
    EachControlledModeWritesExactContractAndReadsBackEveryWrite();
    InvalidOrPrematureRequestsNeverWrite();
    EveryWriteFailureRollsBackTheCompleteTouchedPrefix();
    EveryReadbackFailureRollsBackIncludingTheJustWrittenNode();
    FailedModeTransitionRestoresTheTransactionSnapshot();
    SystemManagedRestoresTheFullStartupBaseline();
    RollbackFailureIsReportedAndBlocksFurtherWrites();
    RollbackFailureStillAllowsExactBaselineRecovery();
    FailedBaselineRecoveryAttemptsEveryNodeAndKeepsLatch();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "performance_service_test: PASS\n";
  return 0;
}
