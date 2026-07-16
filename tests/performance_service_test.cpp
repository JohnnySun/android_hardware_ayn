// SPDX-License-Identifier: Apache-2.0

#include "ayn/performance_service.h"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

#define CHECK(condition) Check((condition), #condition, __FILE__, __LINE__)

void Check(bool condition, const char* expression, const char* file, int line) {
  if (!condition) {
    throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                             ": CHECK failed: " + expression);
  }
}

using ayn::performance::ControlPolicy;
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

struct Harness {
  std::map<std::string, std::string> files;
  std::vector<std::string> events;
  size_t write_count = 0;
  size_t fail_write_at = 0;
  size_t fail_write_after_mutation_at = 0;
  bool transaction_failed = false;
  size_t fail_rollback_write_at = 0;

  Harness() {
    for (size_t index = 0; index < kNodeCount; ++index) {
      files.emplace(kExpectedPaths.nodes[index],
                    std::to_string(kBaseline[index]) + "\n");
    }
  }
};

bool ReadFile(void* context, const std::string& path, std::string* value) {
  auto* harness = static_cast<Harness*>(context);
  harness->events.push_back("R " + path);
  const auto found = harness->files.find(path);
  if (found == harness->files.end()) {
    return false;
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
  if (harness->fail_write_after_mutation_at == harness->write_count) {
    harness->transaction_failed = true;
    return false;
  }
  return true;
}

PerformanceService Service(
    Harness* harness, ControlPolicy policy = ControlPolicy::ReadOnly(),
    const DeviceIdentity& identity = {
        "odin2_mini", "lineage_odin2_mini", "Odin2 Mini"},
    const SysfsPaths& paths = kExpectedPaths) {
  return PerformanceService(identity, paths, ReadFile, harness, WriteFile,
                            harness, policy);
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
    PerformanceService service = Service(&harness, ControlPolicy::ReadOnly(),
                                         identity);
    CHECK(service.Initialize().result ==
          PerformanceResult::kUnsupportedDevice);
    CHECK(harness.events.empty());
  }

  for (size_t index = 0; index < kNodeCount; ++index) {
    Harness harness;
    SysfsPaths paths = kExpectedPaths;
    paths.nodes[index] += ".wrong";
    PerformanceService service = Service(
        &harness, ControlPolicy::ReadOnly(),
        {"odin2_mini", "lineage_odin2_mini", "Odin2 Mini"}, paths);
    CHECK(service.Initialize().result == PerformanceResult::kUnexpectedPaths);
    CHECK(harness.events.empty());
  }
}

void InitializationIsReadOnlyAndReportsSystemManaged() {
  Harness harness;
  PerformanceService service = Service(&harness);
  CHECK(service.GetStatus().result == PerformanceResult::kNotInitialized);
  const auto response = service.Initialize();
  CHECK(response.result == PerformanceResult::kOk);
  CHECK(response.active_mode == PerformanceMode::kSystemManaged);
  CHECK(response.initialized);
  CHECK(harness.events.size() == kNodeCount);
  CHECK(harness.write_count == 0);
}

void DefaultPolicyCannotWriteAnyMode() {
  Harness harness;
  PerformanceService service = Service(&harness);
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  harness.events.clear();

  CHECK(service.SetMode(PerformanceMode::kSystemManaged).result ==
        PerformanceResult::kOk);
  for (PerformanceMode mode : {PerformanceMode::kStockNormal,
                               PerformanceMode::kPerformance,
                               PerformanceMode::kHigh}) {
    const auto response = service.SetMode(mode);
    CHECK(response.result == PerformanceResult::kModeUnavailable);
    CHECK(response.active_mode == PerformanceMode::kSystemManaged);
  }
  CHECK(harness.events.empty());
  CHECK(harness.write_count == 0);
  CHECK(Values(harness) == kBaseline);
}

void ProductionReadOnlyPolicyNeedsNoWriter() {
  Harness harness;
  PerformanceService service(
      {"odin2_mini", "lineage_odin2_mini", "Odin2 Mini"}, kExpectedPaths,
      ReadFile, &harness, nullptr, nullptr, ControlPolicy::ReadOnly());
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  harness.events.clear();

  for (PerformanceMode mode : {PerformanceMode::kStockNormal,
                               PerformanceMode::kPerformance,
                               PerformanceMode::kHigh}) {
    CHECK(service.SetMode(mode).result ==
          PerformanceResult::kModeUnavailable);
  }
  CHECK(harness.events.empty());
  CHECK(harness.write_count == 0);
}

void HighAndPerformanceRemainUnavailableUnderStockNormalPolicy() {
  Harness harness;
  PerformanceService service =
      Service(&harness, ControlPolicy::StockNormalOnly());
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  harness.events.clear();

  for (PerformanceMode mode : {PerformanceMode::kPerformance,
                               PerformanceMode::kHigh}) {
    CHECK(service.SetMode(mode).result ==
          PerformanceResult::kModeUnavailable);
  }
  CHECK(harness.events.empty());
  CHECK(harness.write_count == 0);
}

void StockNormalTransitionCanRestoreStartupBaseline() {
  Harness harness;
  PerformanceService service =
      Service(&harness, ControlPolicy::StockNormalOnly());
  CHECK(service.Initialize().result == PerformanceResult::kOk);

  CHECK(service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kOk);
  CHECK(Values(harness) == kStockNormal);
  CHECK(service.GetStatus().active_mode == PerformanceMode::kStockNormal);

  CHECK(service.SetMode(PerformanceMode::kSystemManaged).result ==
        PerformanceResult::kOk);
  CHECK(Values(harness) == kBaseline);
  CHECK(service.GetStatus().active_mode == PerformanceMode::kSystemManaged);
}

void FailedNormalTransitionRollsBackOrLocksFurtherWrites() {
  Harness rollback;
  PerformanceService rollback_service =
      Service(&rollback, ControlPolicy::StockNormalOnly());
  CHECK(rollback_service.Initialize().result == PerformanceResult::kOk);
  rollback.fail_write_at = 4;
  CHECK(rollback_service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kWriteFailed);
  CHECK(Values(rollback) == kBaseline);
  CHECK(rollback_service.GetStatus().active_mode ==
        PerformanceMode::kSystemManaged);

  Harness failed_rollback;
  PerformanceService failed_service =
      Service(&failed_rollback, ControlPolicy::StockNormalOnly());
  CHECK(failed_service.Initialize().result == PerformanceResult::kOk);
  failed_rollback.fail_write_at = 4;
  failed_rollback.fail_rollback_write_at = 5;
  CHECK(failed_service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kRollbackFailed);
  const size_t writes = failed_rollback.write_count;
  CHECK(failed_service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kRollbackFailed);
  CHECK(failed_rollback.write_count == writes);
}

void WriterFailureAfterMutationRollsBackTheAttemptedNode() {
  Harness harness;
  PerformanceService service =
      Service(&harness, ControlPolicy::StockNormalOnly());
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  harness.fail_write_after_mutation_at = 3;

  CHECK(service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kWriteFailed);
  CHECK(Values(harness) == kBaseline);
  CHECK(service.GetStatus().active_mode ==
        PerformanceMode::kSystemManaged);
}

void RollbackLatchRequiresACompletePointByPointBaselineRestore() {
  Harness harness;
  PerformanceService service =
      Service(&harness, ControlPolicy::StockNormalOnly());
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  harness.fail_write_at = 4;
  harness.fail_rollback_write_at = 5;
  CHECK(service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kRollbackFailed);

  harness.fail_rollback_write_at = 0;
  const size_t latched_writes = harness.write_count;
  CHECK(service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kRollbackFailed);
  CHECK(harness.write_count == latched_writes);

  harness.fail_write_at = harness.write_count + 3;
  CHECK(service.SetMode(PerformanceMode::kSystemManaged).result ==
        PerformanceResult::kRollbackFailed);
  CHECK(harness.write_count == latched_writes + kNodeCount);
  CHECK(service.GetStatus().result == PerformanceResult::kRollbackFailed);

  const size_t partial_restore_writes = harness.write_count;
  CHECK(service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kRollbackFailed);
  CHECK(harness.write_count == partial_restore_writes);

  harness.fail_write_at = 0;
  CHECK(service.SetMode(PerformanceMode::kSystemManaged).result ==
        PerformanceResult::kOk);
  CHECK(harness.write_count == partial_restore_writes + kNodeCount);
  CHECK(Values(harness) == kBaseline);
  CHECK(service.GetStatus().result == PerformanceResult::kOk);
  CHECK(service.GetStatus().active_mode ==
        PerformanceMode::kSystemManaged);
}

void ShutdownLatchRestoresBaselineAndRejectsLaterWrites() {
  Harness harness;
  PerformanceService service =
      Service(&harness, ControlPolicy::StockNormalOnly());
  CHECK(service.Initialize().result == PerformanceResult::kOk);
  CHECK(service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kOk);

  CHECK(service.BeginShutdown().result == PerformanceResult::kOk);
  CHECK(Values(harness) == kBaseline);
  const size_t shutdown_writes = harness.write_count;
  CHECK(service.SetMode(PerformanceMode::kStockNormal).result ==
        PerformanceResult::kModeUnavailable);
  CHECK(harness.write_count == shutdown_writes);
  CHECK(Values(harness) == kBaseline);
}

}  // namespace

int main() {
  try {
    ExactIdentityAndPathsGateAllIo();
    InitializationIsReadOnlyAndReportsSystemManaged();
    DefaultPolicyCannotWriteAnyMode();
    ProductionReadOnlyPolicyNeedsNoWriter();
    HighAndPerformanceRemainUnavailableUnderStockNormalPolicy();
    StockNormalTransitionCanRestoreStartupBaseline();
    FailedNormalTransitionRollsBackOrLocksFurtherWrites();
    WriterFailureAfterMutationRollsBackTheAttemptedNode();
    RollbackLatchRequiresACompletePointByPointBaselineRestore();
    ShutdownLatchRestoresBaselineAndRejectsLaterWrites();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "performance_service_test: PASS\n";
  return 0;
}
