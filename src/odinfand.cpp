// SPDX-License-Identifier: Apache-2.0

#include "ayn/fan_adapter.h"
#include "ayn/fan_service.h"

#include <aidl/com/ayn/fan/BnOdinFan.h>
#include <aidl/com/ayn/fan/FanResponse.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

#include <pthread.h>
#include <unistd.h>

namespace {

constexpr char kServiceName[] = "com.ayn.fan.IOdinFan/default";

bool SleepForMilliseconds(void*, int milliseconds) {
  if (milliseconds < 0) {
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
  return true;
}

int32_t AidlMode(ayn::fan::FanMode mode) {
  return static_cast<int32_t>(mode);
}

int32_t AidlResult(ayn::fan::FanResult result) {
  return static_cast<int32_t>(result);
}

::aidl::com::ayn::fan::FanResponse ToAidlResponse(
    const ayn::fan::FanResponse& response) {
  ::aidl::com::ayn::fan::FanResponse aidl_response;
  aidl_response.result = AidlResult(response.result);
  aidl_response.mode = AidlMode(response.requested_mode);
  aidl_response.state = -1;
  aidl_response.duty = -1;
  aidl_response.tach = -1;
  if (response.snapshot.has_value()) {
    aidl_response.mode = AidlMode(response.snapshot->mode);
    aidl_response.state = response.snapshot->state;
    aidl_response.duty = response.snapshot->duty;
    aidl_response.tach = response.snapshot->tach;
  }
  return aidl_response;
}

class OdinFanBinder final : public ::aidl::com::ayn::fan::BnOdinFan {
 public:
  explicit OdinFanBinder(ayn::fan::FanDeviceIdentity identity)
      : core_(std::move(identity), ayn::fan::StockSysfsPaths(),
              ayn::fan::ReadPosixFile, nullptr, ayn::fan::WritePosixFile,
              nullptr, ayn::fan::ReadCpuTemperature, nullptr,
              SleepForMilliseconds, nullptr) {}

  ayn::fan::FanResponse InitializeSafeDefault() {
    return core_.InitializeSafeDefault();
  }

  ayn::fan::FanResponse ForceOff() {
    return core_.ForceOff();
  }

  ayn::fan::FanResponse Refresh() {
    return core_.Refresh();
  }

  ::ndk::ScopedAStatus getStatus(
      ::aidl::com::ayn::fan::FanResponse* response) override {
    *response = ToAidlResponse(core_.GetStatus());
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus setMode(
      int32_t mode,
      ::aidl::com::ayn::fan::FanResponse* response) override {
    const ayn::fan::FanMode fan_mode = static_cast<ayn::fan::FanMode>(mode);
    *response = ToAidlResponse(core_.SetMode(fan_mode));
    return ::ndk::ScopedAStatus::ok();
  }

 private:
  ayn::fan::FanService core_;
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

  ayn::fan::FanDeviceIdentity identity = {
      android::base::GetProperty("ro.product.device", ""),
      android::base::GetProperty("ro.product.name", ""),
      android::base::GetProperty("ro.product.vendor.model", ""),
  };
  auto service = ::ndk::SharedRefBase::make<OdinFanBinder>(std::move(identity));

  const ayn::fan::FanResponse startup = service->InitializeSafeDefault();
  if (startup.result != ayn::fan::FanResult::kOk) {
    LOG(ERROR) << "refusing Binder registration before safe Quiet default; result="
               << AidlResult(startup.result);
    return EXIT_FAILURE;
  }

  ABinderProcess_setThreadPoolMaxThreadCount(4);
  ABinderProcess_startThreadPool();
  const binder_status_t registration =
      AServiceManager_addService(service->asBinder().get(), kServiceName);
  if (registration != STATUS_OK) {
    LOG(ERROR) << "failed to register " << kServiceName
               << "; status=" << registration;
    service->ForceOff();
    return EXIT_FAILURE;
  }

  LOG(INFO) << "registered " << kServiceName;
  std::thread([service]() mutable {
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(
          ayn::fan::kAutomaticPollIntervalSeconds));
      const ayn::fan::FanResponse refresh = service->Refresh();
      if (refresh.result != ayn::fan::FanResult::kOk) {
        LOG(ERROR) << "automatic fan refresh failed closed; result="
                   << AidlResult(refresh.result);
      }
    }
  }).detach();
  std::thread([service, termination_signals]() mutable {
    int signal = 0;
    if (sigwait(&termination_signals, &signal) != 0) {
      LOG(ERROR) << "termination signal wait failed";
      _exit(EXIT_FAILURE);
    }
    const ayn::fan::FanResponse shutdown = service->ForceOff();
    if (shutdown.result != ayn::fan::FanResult::kOk) {
      LOG(ERROR) << "shutdown could not confirm fan Off; result="
                 << AidlResult(shutdown.result);
      _exit(EXIT_FAILURE);
    }
    _exit(EXIT_SUCCESS);
  }).detach();
  ABinderProcess_joinThreadPool();
  service->ForceOff();
  return EXIT_FAILURE;
}
