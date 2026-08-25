// SPDX-License-Identifier: Apache-2.0

#include "ayn/performance_adapter.h"
#include "ayn/performance_service.h"

#include <aidl/com/ayn/performance/BnOdinPerformance.h>
#include <aidl/com/ayn/performance/PerformanceResponse.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>

#include <pthread.h>
#include <unistd.h>

namespace {

constexpr char kServiceName[] =
    "com.ayn.performance.IOdinPerformance/default";

int32_t AidlMode(ayn::performance::PerformanceMode mode) {
  return static_cast<int32_t>(mode);
}

int32_t AidlResult(ayn::performance::PerformanceResult result) {
  return static_cast<int32_t>(result);
}

::aidl::com::ayn::performance::PerformanceResponse ToAidlResponse(
    const ayn::performance::PerformanceResponse& response) {
  ::aidl::com::ayn::performance::PerformanceResponse aidl_response;
  aidl_response.result = AidlResult(response.result);
  aidl_response.requestedMode = AidlMode(response.requested_mode);
  aidl_response.activeMode = AidlMode(response.active_mode);
  aidl_response.initialized = response.initialized;
  return aidl_response;
}

class OdinPerformanceBinder final
    : public ::aidl::com::ayn::performance::BnOdinPerformance {
 public:
  explicit OdinPerformanceBinder(ayn::performance::DeviceIdentity identity)
      : core_(std::move(identity), ayn::performance::StockSysfsPaths(),
              ayn::performance::ReadPosixFile, nullptr,
              ayn::performance::WritePosixFile, nullptr,
              // All three stock modes. The core has always encoded the
              // disassembly-proven Normal, Performance and High targets with
              // rollback tests; the daemon withheld two of them until the
              // thermal path could be trusted. On 2026-08-25 the framework
              // began reporting real temperatures and the fan was observed
              // tracking a four-minute load to 80 C without throttling, so
              // the reason for withholding them is gone.
              ayn::performance::ControlPolicy::AllStockModes()) {}

  ayn::performance::PerformanceResponse Initialize() {
    return core_.Initialize();
  }

  ayn::performance::PerformanceResponse BeginShutdown() {
    return core_.BeginShutdown();
  }

  ::ndk::ScopedAStatus getStatus(
      ::aidl::com::ayn::performance::PerformanceResponse* response) override {
    *response = ToAidlResponse(core_.GetStatus());
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus setMode(
      int32_t mode,
      ::aidl::com::ayn::performance::PerformanceResponse* response) override {
    *response = ToAidlResponse(core_.SetMode(
        static_cast<ayn::performance::PerformanceMode>(mode)));
    return ::ndk::ScopedAStatus::ok();
  }

 private:
  ayn::performance::PerformanceService core_;
};

}  // namespace

int main() {
  sigset_t termination_signals;
  sigemptyset(&termination_signals);
  sigaddset(&termination_signals, SIGINT);
  sigaddset(&termination_signals, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &termination_signals, nullptr) != 0) {
    LOG(ERROR) << "could not block termination signals";
    return EXIT_FAILURE;
  }

  ayn::performance::DeviceIdentity identity = {
      android::base::GetProperty("ro.product.device", ""),
      android::base::GetProperty("ro.product.name", ""),
      android::base::GetProperty("ro.product.vendor.model", ""),
  };
  auto service =
      ::ndk::SharedRefBase::make<OdinPerformanceBinder>(std::move(identity));
  const ayn::performance::PerformanceResponse startup = service->Initialize();
  if (startup.result != ayn::performance::PerformanceResult::kOk) {
    LOG(ERROR) << "refusing registration before complete baseline capture; result="
               << AidlResult(startup.result);
    return EXIT_FAILURE;
  }

  ABinderProcess_setThreadPoolMaxThreadCount(2);
  ABinderProcess_startThreadPool();
  const binder_status_t registration =
      AServiceManager_addService(service->asBinder().get(), kServiceName);
  if (registration != STATUS_OK) {
    LOG(ERROR) << "failed to register " << kServiceName
               << "; status=" << registration;
    service->BeginShutdown();
    return EXIT_FAILURE;
  }

  LOG(INFO) << "registered " << kServiceName;
  std::thread([service, termination_signals]() mutable {
    int signal = 0;
    if (sigwait(&termination_signals, &signal) != 0) {
      LOG(ERROR) << "termination signal wait failed";
      _exit(EXIT_FAILURE);
    }
    const ayn::performance::PerformanceResponse restore =
        service->BeginShutdown();
    if (restore.result != ayn::performance::PerformanceResult::kOk) {
      LOG(ERROR) << "shutdown baseline restore failed; result="
                 << AidlResult(restore.result);
      _exit(EXIT_FAILURE);
    }
    _exit(EXIT_SUCCESS);
  }).detach();

  ABinderProcess_joinThreadPool();
  service->BeginShutdown();
  return EXIT_FAILURE;
}
