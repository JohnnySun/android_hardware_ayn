// SPDX-License-Identifier: Apache-2.0
#include "ayn/charge_lifecycle.h"

#include <android-base/logging.h>
#include <android-base/properties.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <unistd.h>

namespace {

constexpr size_t kMaximumSysfsValueBytes = 32;

volatile sig_atomic_t g_terminated = 0;

bool ReadPosixFile(void*, const std::string& path, std::string* value) {
  if (value == nullptr) {
    return false;
  }
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }

  char buffer[kMaximumSysfsValueBytes + 1];
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
    if (result.size() > kMaximumSysfsValueBytes) {
      close(fd);
      return false;
    }
  }
  close(fd);
  *value = result;
  return true;
}

bool WritePosixFile(void*, const std::string& path, const std::string& value) {
  if (value.empty() || value.size() > kMaximumSysfsValueBytes) {
    return false;
  }
  const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
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

bool StopRequested(void*) { return g_terminated != 0; }

bool SleepForSeconds(void* context, int seconds) {
  const sigset_t& termination_signals = *static_cast<sigset_t*>(context);
  struct timespec timeout = {seconds, 0};
  while (true) {
    const int signal_number =
        sigtimedwait(&termination_signals, nullptr, &timeout);
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
  const ayn::charge::SysfsPaths& paths = ayn::charge::StockSysfsPaths();

  // A restriction left behind by a killed daemon would otherwise outlive it and
  // quietly stop the battery charging.
  if (ayn::charge::ReleaseRestriction(product_device, paths, ReadPosixFile,
                                      nullptr, WritePosixFile, nullptr) ==
      ayn::charge::ApplyResult::kFailedClosed) {
    LOG(ERROR) << "could not clear a stale charge restriction for device '"
               << product_device << "'";
    return EXIT_FAILURE;
  }

  const ayn::charge::LimitSettings settings = {
      true,
      ayn::charge::kDefaultStopPercent,
      ayn::charge::kDefaultResumePercent,
  };

  const ayn::charge::LoopResult result = ayn::charge::RunLimitLoopUnlessStopped(
      product_device, paths, settings, StopRequested, nullptr, ReadPosixFile,
      nullptr, WritePosixFile, nullptr, SleepForSeconds, &termination_signals);

  if (result == ayn::charge::LoopResult::kFailedClosed) {
    LOG(ERROR) << "charge limit failed closed for device '" << product_device
               << "'; charging left unrestricted";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
