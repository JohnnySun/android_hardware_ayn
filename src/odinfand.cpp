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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
              SleepForMilliseconds, nullptr),
        death_recipient_(AIBinder_DeathRecipient_new(OwnerDiedCallback)) {}

  ayn::fan::FanResponse ForceOff() {
    std::lock_guard<std::mutex> lock(mutex_);
    ClearOwnerLocked();
    return core_.ForceOff();
  }

  ayn::fan::FanResponse Refresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    const ayn::fan::FanResponse response = core_.Refresh();
    if (response.result != ayn::fan::FanResult::kOk &&
        response.result != ayn::fan::FanResult::kNotOwner) {
      ClearOwnerLocked();
    }
    return response;
  }

  ::ndk::ScopedAStatus getStatus(
      ::aidl::com::ayn::fan::FanResponse* response) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const ayn::fan::FanResponse core_response = core_.GetStatus();
    if (core_response.result != ayn::fan::FanResult::kOk) {
      ClearOwnerLocked();
    }
    *response = ToAidlResponse(core_response);
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus setMode(
      int32_t mode, const ::ndk::SpAIBinder& owner,
      ::aidl::com::ayn::fan::FanResponse* response) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const ayn::fan::FanMode fan_mode = static_cast<ayn::fan::FanMode>(mode);
    if (owner.get() == nullptr || !AIBinder_isAlive(owner.get())) {
      ClearOwnerLocked();
      *response = ToAidlResponse(core_.SetMode(fan_mode, 0, false));
      return ::ndk::ScopedAStatus::ok();
    }

    if (owner_.get() == owner.get()) {
      const ayn::fan::FanResponse core_response =
          core_.SetMode(fan_mode, owner_id_, true);
      if (core_response.result != ayn::fan::FanResult::kOk ||
          fan_mode == ayn::fan::FanMode::kOff) {
        ClearOwnerLocked();
      }
      *response = ToAidlResponse(core_response);
      return ::ndk::ScopedAStatus::ok();
    }

    auto cookie = std::make_unique<OwnerCookie>();
    cookie->service = this;
    cookie->id = next_owner_id_++;
    OwnerCookie* const cookie_pointer = cookie.get();
    cookies_.push_back(std::move(cookie));
    const binder_status_t link_status = AIBinder_linkToDeath(
        owner.get(), death_recipient_.get(), cookie_pointer);
    if (link_status != STATUS_OK || !AIBinder_isAlive(owner.get())) {
      if (link_status == STATUS_OK) {
        AIBinder_unlinkToDeath(owner.get(), death_recipient_.get(),
                               cookie_pointer);
      }
      ClearOwnerLocked();
      *response = ToAidlResponse(core_.SetMode(fan_mode, 0, false));
      return ::ndk::ScopedAStatus::ok();
    }

    const ayn::fan::FanResponse core_response =
        core_.SetMode(fan_mode, cookie_pointer->id, true);
    if (core_response.result == ayn::fan::FanResult::kOk &&
        fan_mode != ayn::fan::FanMode::kOff) {
      ClearOwnerLocked();
      owner_ = owner;
      owner_cookie_ = cookie_pointer;
      owner_id_ = cookie_pointer->id;
    } else {
      AIBinder_unlinkToDeath(owner.get(), death_recipient_.get(),
                             cookie_pointer);
      ClearOwnerLocked();
    }
    *response = ToAidlResponse(core_response);
    return ::ndk::ScopedAStatus::ok();
  }

 private:
  struct OwnerCookie {
    OdinFanBinder* service;
    uintptr_t id;
  };

  static void OwnerDiedCallback(void* cookie) {
    auto* owner_cookie = static_cast<OwnerCookie*>(cookie);
    if (owner_cookie != nullptr && owner_cookie->service != nullptr) {
      owner_cookie->service->HandleOwnerDeath(owner_cookie->id);
    }
  }

  void HandleOwnerDeath(uintptr_t owner_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_id == 0 || owner_id != owner_id_) {
      return;
    }
    owner_.set(nullptr);
    owner_cookie_ = nullptr;
    owner_id_ = 0;
    const ayn::fan::FanResponse response = core_.OwnerDied(owner_id);
    if (response.result != ayn::fan::FanResult::kOk) {
      LOG(ERROR) << "owner death could not confirm fan Off; result="
                 << AidlResult(response.result);
    }
  }

  void ClearOwnerLocked() {
    if (owner_.get() != nullptr && owner_cookie_ != nullptr) {
      AIBinder_unlinkToDeath(owner_.get(), death_recipient_.get(),
                             owner_cookie_);
    }
    owner_.set(nullptr);
    owner_cookie_ = nullptr;
    owner_id_ = 0;
  }

  ayn::fan::FanService core_;
  ::ndk::ScopedAIBinder_DeathRecipient death_recipient_;
  std::mutex mutex_;
  ::ndk::SpAIBinder owner_;
  OwnerCookie* owner_cookie_ = nullptr;
  uintptr_t owner_id_ = 0;
  uintptr_t next_owner_id_ = 1;
  std::vector<std::unique_ptr<OwnerCookie>> cookies_;
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

  const ayn::fan::FanResponse startup = service->ForceOff();
  if (startup.result != ayn::fan::FanResult::kOk) {
    LOG(ERROR) << "refusing Binder registration before confirmed fan Off; result="
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
      if (refresh.result != ayn::fan::FanResult::kOk &&
          refresh.result != ayn::fan::FanResult::kNotOwner) {
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
