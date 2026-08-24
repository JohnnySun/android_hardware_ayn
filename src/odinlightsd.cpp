// SPDX-License-Identifier: Apache-2.0
#include "ayn/lights_lifecycle.h"

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

// Set once a termination signal has been observed, so the clamp stops before
// its next write rather than mid-pass.
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

// Doubles as the poll delay and the termination wait, so a stopping daemon does
// not sit out a full interval first.
bool SleepForMilliseconds(void* context, int milliseconds) {
  const sigset_t& termination_signals = *static_cast<sigset_t*>(context);
  struct timespec timeout = {
      milliseconds / 1000,
      static_cast<long>(milliseconds % 1000) * 1000000L,
  };
  while (true) {
    const int signal_number = sigtimedwait(&termination_signals, nullptr,
                                           &timeout);
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

  const ayn::lights::LoopResult result = ayn::lights::RunClampLoopUnlessStopped(
      product_device, ayn::lights::StockSysfsPaths(),
      ayn::lights::kDefaultChannelCap, StopRequested, nullptr, ReadPosixFile,
      nullptr, WritePosixFile, nullptr, SleepForMilliseconds,
      &termination_signals);

  if (result == ayn::lights::LoopResult::kFailedClosed) {
    LOG(ERROR) << "indicator clamp failed closed for device '" << product_device
               << "'; leaving the LED to the stock HAL";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
