// SPDX-License-Identifier: Apache-2.0
#include "ayn/charge_service.h"

#include <aidl/com/ayn/charge/BnOdinCharge.h>
#include <aidl/com/ayn/charge/ChargeResponse.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <unistd.h>

namespace {

constexpr char kServiceName[] = "com.ayn.charge.IOdinCharge/default";
constexpr char kStatePath[] = "/data/system/odin-charge-mode";
constexpr size_t kMaximumValueBytes = 32;

volatile sig_atomic_t g_terminated = 0;

bool ReadPosixFile(void*, const std::string& path, std::string* value) {
  if (value == nullptr) {
    return false;
  }
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  char buffer[kMaximumValueBytes + 1];
  std::string result;
  while (true) {
    const ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      return false;
    }
    if (count == 0) {
      break;
    }
    result.append(buffer, static_cast<size_t>(count));
    if (result.size() > kMaximumValueBytes) {
      close(fd);
      return false;
    }
  }
  close(fd);
  *value = result;
  return true;
}

bool WriteToPath(const std::string& path, const std::string& value, int flags,
                 mode_t mode) {
  if (value.empty() || value.size() > kMaximumValueBytes) {
    return false;
  }
  const int fd = open(path.c_str(), flags | O_CLOEXEC, mode);
  if (fd < 0) {
    return false;
  }
  size_t offset = 0;
  while (offset < value.size()) {
    const ssize_t count =
        write(fd, value.data() + offset, value.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  close(fd);
  return true;
}

bool WritePosixFile(void*, const std::string& path, const std::string& value) {
  return WriteToPath(path, value, O_WRONLY, 0);
}

bool ReadStateFile(void*, std::string* value) {
  return ReadPosixFile(nullptr, kStatePath, value);
}

bool WriteStateFile(void*, const std::string& value) {
  return WriteToPath(kStatePath, value + "\n", O_WRONLY | O_CREAT | O_TRUNC,
                     0600);
}

bool SleepUnlessTerminated(const sigset_t& signals, int seconds) {
  struct timespec timeout = {seconds, 0};
  while (true) {
    const int signal_number = sigtimedwait(&signals, nullptr, &timeout);
    if (signal_number > 0) {
      g_terminated = 1;
      return false;
    }
    if (errno == EAGAIN) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    return false;
  }
}

int32_t AidlResult(ayn::charge::ServiceResult result) {
  return static_cast<int32_t>(result);
}

::aidl::com::ayn::charge::ChargeResponse ToAidl(
    const ayn::charge::ServiceResponse& response) {
  ::aidl::com::ayn::charge::ChargeResponse out;
  out.result = AidlResult(response.result);
  out.mode = static_cast<int32_t>(response.mode);
  out.capacity = -1;
  out.restricted = -1;
  out.stopPercent = -1;
  out.resumePercent = -1;
  if (response.snapshot_valid) {
    out.capacity = response.snapshot.capacity_percent;
    out.restricted = response.snapshot.restricted ? 1 : 0;
    out.stopPercent = response.snapshot.stop_percent;
    out.resumePercent = response.snapshot.resume_percent;
  }
  return out;
}

class OdinChargeBinder : public ::aidl::com::ayn::charge::BnOdinCharge {
 public:
  explicit OdinChargeBinder(ayn::charge::ChargeService* core) : core_(core) {}

  ::ndk::ScopedAStatus getStatus(
      ::aidl::com::ayn::charge::ChargeResponse* out) override {
    *out = ToAidl(core_->GetStatus());
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus setMode(
      int32_t mode, ::aidl::com::ayn::charge::ChargeResponse* out) override {
    if (!ayn::charge::IsKnownMode(mode)) {
      ayn::charge::ServiceResponse refused = core_->GetStatus();
      refused.result = ayn::charge::ServiceResult::kInvalidMode;
      *out = ToAidl(refused);
      return ::ndk::ScopedAStatus::ok();
    }
    *out = ToAidl(
        core_->SetMode(static_cast<ayn::charge::ChargeMode>(mode)));
    return ::ndk::ScopedAStatus::ok();
  }

 private:
  ayn::charge::ChargeService* const core_;
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

  const std::string product_device =
      android::base::GetProperty("ro.product.device", "");

  ayn::charge::ChargeService core(
      product_device, ayn::charge::StockSysfsPaths(), ReadPosixFile, nullptr,
      WritePosixFile, nullptr, ReadStateFile, nullptr, WriteStateFile, nullptr);

  const ayn::charge::ServiceResponse started = core.Start();
  if (started.result != ayn::charge::ServiceResult::kOk) {
    LOG(ERROR) << "charge limit failed closed for device '" << product_device
               << "'; charging left unrestricted";
    core.Release();
    return EXIT_FAILURE;
  }

  auto service = ::ndk::SharedRefBase::make<OdinChargeBinder>(&core);
  ABinderProcess_setThreadPoolMaxThreadCount(2);
  ABinderProcess_startThreadPool();
  // Registration needs a service_contexts entry, and that lives in system_ext
  // policy which a system-only transaction cannot deliver. Losing it costs the
  // remote control surface, not the limit itself, so the daemon keeps enforcing
  // the stored mode either way rather than leaving the battery unmanaged.
  if (AServiceManager_addService(service->asBinder().get(), kServiceName) !=
      STATUS_OK) {
    LOG(WARNING) << "could not register " << kServiceName
                 << "; continuing without remote control";
  }

  while (!g_terminated) {
    if (!SleepUnlessTerminated(termination_signals,
                               ayn::charge::kPollIntervalSeconds)) {
      break;
    }
    const ayn::charge::ServiceResponse refreshed = core.Refresh();
    if (refreshed.result != ayn::charge::ServiceResult::kOk) {
      LOG(ERROR) << "charge limit failed closed; charging left unrestricted";
      core.Release();
      return EXIT_FAILURE;
    }
  }

  // Stopping must never leave the battery unable to charge.
  core.Release();
  return EXIT_SUCCESS;
}
