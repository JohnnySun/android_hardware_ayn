// SPDX-License-Identifier: Apache-2.0

#include "ayn/performance_adapter.h"
#include "ayn/performance_service.h"

#include <aidl/com/ayn/performance/BnOdinPerformance.h>
#include <aidl/com/ayn/performance/PerformanceResponse.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <fcntl.h>

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <chrono>
#include <thread>
#include <utility>

#include <pthread.h>
#include <unistd.h>

namespace {

constexpr char kServiceName[] =
    "com.ayn.performance.IOdinPerformance/default";

// Matches the stock daemon's cadence and its screen-state skip, both read out
// of its disassembly and recorded in stock-performance-mode-consumer.json.
constexpr int kReassertIntervalSeconds = 1;
constexpr char kScreenStateProperty[] = "debug.tracing.screen_state";
constexpr char kScreenOffValue[] = "1";

// A mode the owner chose is a setting, not a session. Every desktop system
// that offers one restores it at boot, and coming up in SYSTEM_MANAGED after
// the owner asked for Performance is the surprising behaviour, not the safe
// one. The charge daemon already keeps its mode this way.
constexpr char kStatePath[] = "/data/system/odin-performance-mode";
constexpr size_t kMaximumStateBytes = 32;

bool ReadStoredMode(ayn::performance::PerformanceMode* mode) {
  const int fd = open(kStatePath, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  char buffer[kMaximumStateBytes + 1];
  ssize_t count = 0;
  do {
    count = read(fd, buffer, sizeof(buffer) - 1);
  } while (count < 0 && errno == EINTR);
  close(fd);
  if (count <= 0) {
    return false;
  }
  const char* begin = buffer;
  const char* end = buffer + count;
  while (end > begin && (end[-1] == '\n' || end[-1] == ' ')) {
    --end;
  }
  int value = 0;
  const auto parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return false;
  }
  switch (value) {
    case 0:
    case 1:
    case 2:
    case 3:
      *mode = static_cast<ayn::performance::PerformanceMode>(value);
      return true;
    default:
      return false;
  }
}

// Best effort by design. The mode is already applied by the time this runs,
// and a mode that works but is not remembered is a far better outcome than a
// mode change that fails because /data would not take a write.
void StoreMode(ayn::performance::PerformanceMode mode) {
  const int fd =
      open(kStatePath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    LOG(WARNING) << "could not open " << kStatePath << ": " << strerror(errno);
    return;
  }
  const std::string value =
      std::to_string(static_cast<int32_t>(mode)) + "\n";
  size_t offset = 0;
  while (offset < value.size()) {
    const ssize_t count =
        write(fd, value.data() + offset, value.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      LOG(WARNING) << "could not persist the performance mode: "
                   << strerror(errno);
      break;
    }
    offset += static_cast<size_t>(count);
  }
  close(fd);
}

void ForgetStoredMode() { unlink(kStatePath); }

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

  ayn::performance::PerformanceResponse Reassert() {
    return core_.Reassert();
  }

  ayn::performance::PerformanceResponse SetModeDirect(
      ayn::performance::PerformanceMode mode) {
    return core_.SetMode(mode);
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
    const ayn::performance::PerformanceMode requested =
        static_cast<ayn::performance::PerformanceMode>(mode);
    const ayn::performance::PerformanceResponse changed =
        core_.SetMode(requested);
    if (changed.result == ayn::performance::PerformanceResult::kOk) {
      StoreMode(changed.active_mode);
    }
    *response = ToAidlResponse(changed);
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

  // Restore before anything can query the service, so the first getStatus
  // already reports the mode the owner left set. A stored mode that will not
  // apply is forgotten rather than retried, because a mode that fails every
  // boot is worse than one that is lost once.
  ayn::performance::PerformanceMode stored =
      ayn::performance::PerformanceMode::kSystemManaged;
  if (ReadStoredMode(&stored) &&
      stored != ayn::performance::PerformanceMode::kSystemManaged) {
    const ayn::performance::PerformanceResponse restored =
        service->SetModeDirect(stored);
    if (restored.result == ayn::performance::PerformanceResult::kOk) {
      LOG(INFO) << "restored performance mode " << AidlMode(stored);
    } else {
      LOG(WARNING) << "stored performance mode " << AidlMode(stored)
                   << " would not apply; result="
                   << AidlResult(restored.result) << ", forgetting it";
      ForgetStoredMode();
    }
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

  // The QTI perf stack resets these limits, so a chosen mode has to be held
  // rather than written once. The stock daemon reasserted every second and
  // skipped while the screen was off; both are worth keeping, the second
  // because there is no reason to hold raised minimums against a dark panel.
  std::thread([service]() mutable {
    while (true) {
      std::this_thread::sleep_for(
          std::chrono::seconds(kReassertIntervalSeconds));
      if (android::base::GetProperty(kScreenStateProperty, "") ==
          kScreenOffValue) {
        continue;
      }
      const ayn::performance::PerformanceResponse held = service->Reassert();
      if (held.result != ayn::performance::PerformanceResult::kOk) {
        LOG(WARNING) << "could not hold performance mode; result="
                     << AidlResult(held.result);
      }
    }
  }).detach();

  ABinderProcess_joinThreadPool();
  service->BeginShutdown();
  return EXIT_FAILURE;
}
