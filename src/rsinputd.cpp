// SPDX-License-Identifier: Apache-2.0

#include "ayn/controller_profile.h"
#include "ayn/controller_profile_adapter.h"
#include "ayn/rsinput_mapping.h"
#include "ayn/rsinput_lifecycle.h"
#include "ayn/rsinput_parser.h"
#include "ayn/rsinput_uart.h"

#include <aidl/com/ayn/controller/BnOdinController.h>
#include <aidl/com/ayn/controller/ControllerProfileResponse.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

constexpr char kUartPath[] = "/dev/ttyHS1";
constexpr char kUinputPath[] = "/dev/uinput";
constexpr char kGamepadName[] = "AYN Odin2 Gamepad";
constexpr uint32_t kHandshakeResponseTimeoutMs = 1000;
constexpr uint32_t kMcuCommandIntervalMs = 100;
constexpr useconds_t kUartByteIntervalUs = 100;
constexpr uint32_t kInitialRetryDelayMs = 250;
constexpr uint32_t kMaxRetryDelayMs = 5000;
constexpr uint32_t kStopCheckIntervalMs = 50;
constexpr uint32_t kRuntimeStatusIdleTimeoutMs = 1000;
constexpr char kControllerServiceName[] =
    "com.ayn.controller.IOdinController/default";

volatile sig_atomic_t g_stop_requested = 0;

struct RuntimeContext {
  std::string product_device;
  ayn::rsinput::ControllerProfileService* profile_service = nullptr;
};

int32_t AidlProfile(ayn::rsinput::ControllerProfile profile) {
  return static_cast<int32_t>(profile);
}

int32_t AidlProfileResult(ayn::rsinput::ControllerProfileResult result) {
  return static_cast<int32_t>(result);
}

::aidl::com::ayn::controller::ControllerProfileResponse ToAidlResponse(
    const ayn::rsinput::ControllerProfileResponse& response) {
  ::aidl::com::ayn::controller::ControllerProfileResponse aidl_response;
  aidl_response.result = AidlProfileResult(response.result);
  aidl_response.requestedProfile = AidlProfile(response.requested_profile);
  aidl_response.activeProfile = AidlProfile(response.active_profile);
  return aidl_response;
}

class OdinControllerBinder final
    : public ::aidl::com::ayn::controller::BnOdinController {
 public:
  explicit OdinControllerBinder(ayn::rsinput::ControllerProfileService* core)
      : core_(core) {}

  ::ndk::ScopedAStatus getProfile(
      ::aidl::com::ayn::controller::ControllerProfileResponse* response)
      override {
    *response = ToAidlResponse(core_->GetProfile());
    return ::ndk::ScopedAStatus::ok();
  }

  ::ndk::ScopedAStatus setProfile(
      int32_t profile,
      ::aidl::com::ayn::controller::ControllerProfileResponse* response)
      override {
    *response = ToAidlResponse(core_->SetProfile(
        static_cast<ayn::rsinput::ControllerProfile>(profile)));
    return ::ndk::ScopedAStatus::ok();
  }

 private:
  ayn::rsinput::ControllerProfileService* const core_;
};

void RequestStop(int) {
  g_stop_requested = 1;
}

bool InstallStopHandlers() {
  struct sigaction action {};
  action.sa_handler = RequestStop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, nullptr) != 0 ||
      sigaction(SIGTERM, &action, nullptr) != 0) {
    PLOG(ERROR) << "cannot install stop handlers";
    return false;
  }
  return true;
}

static_assert(ayn::rsinput::kEventTypeSyn == EV_SYN);
static_assert(ayn::rsinput::kEventTypeKey == EV_KEY);
static_assert(ayn::rsinput::kEventTypeAbs == EV_ABS);
static_assert(ayn::rsinput::kSynReport == SYN_REPORT);
static_assert(ayn::rsinput::kBtnDpadUp == BTN_DPAD_UP);
static_assert(ayn::rsinput::kBtnDpadDown == BTN_DPAD_DOWN);
static_assert(ayn::rsinput::kBtnDpadLeft == BTN_DPAD_LEFT);
static_assert(ayn::rsinput::kBtnDpadRight == BTN_DPAD_RIGHT);
static_assert(ayn::rsinput::kBtnNorth == BTN_NORTH);
static_assert(ayn::rsinput::kBtnWest == BTN_WEST);
static_assert(ayn::rsinput::kBtnEast == BTN_EAST);
static_assert(ayn::rsinput::kBtnSouth == BTN_SOUTH);
static_assert(ayn::rsinput::kBtnTl == BTN_TL);
static_assert(ayn::rsinput::kBtnTr == BTN_TR);
static_assert(ayn::rsinput::kBtnSelect == BTN_SELECT);
static_assert(ayn::rsinput::kBtnStart == BTN_START);
static_assert(ayn::rsinput::kBtnThumbL == BTN_THUMBL);
static_assert(ayn::rsinput::kBtnThumbR == BTN_THUMBR);
static_assert(ayn::rsinput::kBtnMode == BTN_MODE);
static_assert(ayn::rsinput::kBtnBack == BTN_BACK);
static_assert(ayn::rsinput::kAbsX == ABS_X);
static_assert(ayn::rsinput::kAbsY == ABS_Y);
static_assert(ayn::rsinput::kAbsZ == ABS_Z);
static_assert(ayn::rsinput::kAbsRx == ABS_RX);
static_assert(ayn::rsinput::kAbsRy == ABS_RY);
static_assert(ayn::rsinput::kAbsRz == ABS_RZ);

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (size != 0) {
    const ssize_t written = write(fd, bytes, size);
    if (written < 0) {
      if (errno == EINTR) {
        if (g_stop_requested != 0) {
          return false;
        }
        continue;
      }
      PLOG(ERROR) << "write failed";
      return false;
    }
    if (written == 0) {
      LOG(ERROR) << "write returned zero";
      return false;
    }
    bytes += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool LeaveRuntimeMcuPowerUnchanged(void*) {
  return true;
}

ayn::rsinput::StartupResult SkipRuntimeMcuPowerSettle(void*) {
  return g_stop_requested != 0 ? ayn::rsinput::StartupResult::kStopped
                               : ayn::rsinput::StartupResult::kCompleted;
}

int OpenConfiguredUart() {
  const int fd = open(kUartPath, ayn::rsinput::RuntimeUartOpenFlags());
  if (fd < 0) {
    PLOG(ERROR) << "cannot open RSInput UART";
    return -1;
  }

  termios current_settings{};
  if (tcgetattr(fd, &current_settings) != 0) {
    PLOG(ERROR) << "cannot read RSInput UART settings";
    close(fd);
    return -1;
  }

  // Stock configures a fresh termios rather than preserving tty defaults.
  termios settings{};
  settings.c_iflag = 0;
  settings.c_oflag = 0;
  settings.c_lflag = 0;
  settings.c_cflag = CS8 | CLOCAL | CREAD;
  settings.c_cc[VMIN] = 16;
  settings.c_cc[VTIME] = 64;
  if (cfsetispeed(&settings, B115200) != 0 ||
      cfsetospeed(&settings, B115200) != 0 ||
      tcflush(fd, TCIFLUSH) != 0 ||
      tcsetattr(fd, TCSANOW, &settings) != 0) {
    PLOG(ERROR) << "cannot configure RSInput UART";
    close(fd);
    return -1;
  }
  return fd;
}

bool SetUinputCapability(int fd, unsigned long request, uint16_t code) {
  if (ioctl(fd, request, code) == 0) {
    return true;
  }
  PLOG(ERROR) << "cannot configure uinput capability";
  return false;
}

bool CreateUinputGamepad(int fd) {
  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0 ||
      ioctl(fd, UI_SET_EVBIT, EV_ABS) != 0) {
    PLOG(ERROR) << "cannot configure uinput event types";
    return false;
  }

  constexpr std::array<uint16_t, 16> kKeys = {
      ayn::rsinput::kBtnDpadUp,   ayn::rsinput::kBtnDpadDown,
      ayn::rsinput::kBtnDpadLeft, ayn::rsinput::kBtnDpadRight,
      ayn::rsinput::kBtnNorth,    ayn::rsinput::kBtnWest,
      ayn::rsinput::kBtnEast,     ayn::rsinput::kBtnSouth,
      ayn::rsinput::kBtnTl,       ayn::rsinput::kBtnTr,
      ayn::rsinput::kBtnSelect,   ayn::rsinput::kBtnStart,
      ayn::rsinput::kBtnThumbL,   ayn::rsinput::kBtnThumbR,
      ayn::rsinput::kBtnMode,     ayn::rsinput::kBtnBack,
  };
  for (uint16_t key : kKeys) {
    if (!SetUinputCapability(fd, UI_SET_KEYBIT, key)) {
      return false;
    }
  }

  constexpr std::array<uint16_t, 6> kAxes = {
      ayn::rsinput::kAbsX,  ayn::rsinput::kAbsY, ayn::rsinput::kAbsRx,
      ayn::rsinput::kAbsRy, ayn::rsinput::kAbsZ, ayn::rsinput::kAbsRz,
  };
  for (uint16_t axis : kAxes) {
    if (!SetUinputCapability(fd, UI_SET_ABSBIT, axis)) {
      return false;
    }
  }

  uinput_user_dev device{};
  std::snprintf(device.name, sizeof(device.name), "%s", kGamepadName);
  device.id.bustype = BUS_USB;
  device.id.vendor = 0x2020;
  device.id.product = 0x3001;
  device.id.version = 1;

  for (uint16_t axis : {ayn::rsinput::kAbsX, ayn::rsinput::kAbsY,
                        ayn::rsinput::kAbsRx, ayn::rsinput::kAbsRy}) {
    device.absmin[axis] = ayn::rsinput::kStickAxisMin;
    device.absmax[axis] = ayn::rsinput::kStickAxisMax;
    device.absflat[axis] = ayn::rsinput::kStickAxisFlat;
  }
  for (uint16_t axis : {ayn::rsinput::kAbsZ, ayn::rsinput::kAbsRz}) {
    device.absmin[axis] = ayn::rsinput::kTriggerAxisMin;
    device.absmax[axis] = ayn::rsinput::kTriggerAxisMax;
    device.absflat[axis] = ayn::rsinput::kTriggerAxisFlat;
  }

  if (!WriteAll(fd, &device, sizeof(device))) {
    return false;
  }
  if (ioctl(fd, UI_DEV_CREATE) != 0) {
    PLOG(ERROR) << "cannot create uinput gamepad";
    return false;
  }
  return true;
}

bool StopWasRequested(void*) {
  return g_stop_requested != 0;
}

int OpenRuntimeUart(void*) {
  return OpenConfiguredUart();
}

int OpenRuntimeUinput(void*) {
  const int fd = open(kUinputPath, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    PLOG(ERROR) << "cannot open uinput";
    return -1;
  }
  if (!CreateUinputGamepad(fd)) {
    close(fd);
    return -1;
  }
  return fd;
}

void CloseRuntimeUart(void*, int fd) {
  close(fd);
}

void CloseRuntimeUinput(void*, int fd) {
  if (ioctl(fd, UI_DEV_DESTROY) != 0) {
    PLOG(WARNING) << "cannot destroy uinput gamepad";
  }
  close(fd);
}

bool WriteInitializationFrame(void* context, const uint8_t* data, size_t size) {
  const int uart_fd = *static_cast<int*>(context);
  for (size_t index = 0; index < size; ++index) {
    if (!WriteAll(uart_fd, data + index, 1)) {
      return false;
    }
    while (usleep(kUartByteIntervalUs) != 0) {
      if (errno != EINTR || g_stop_requested != 0) {
        PLOG(ERROR) << "RSInput UART byte interval failed";
        return false;
      }
    }
  }
  return true;
}

bool WaitBeforeInitializationFrame(void*, uint32_t delay_ms) {
  uint32_t remaining_ms = delay_ms;
  while (remaining_ms != 0 && g_stop_requested == 0) {
    const uint32_t wait_ms = std::min(remaining_ms, kStopCheckIntervalMs);
    const int result = poll(nullptr, 0, static_cast<int>(wait_ms));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      PLOG(ERROR) << "RSInput MCU command interval failed";
      return false;
    }
    remaining_ms -= wait_ms;
  }
  return g_stop_requested == 0;
}

uint64_t MonotonicMilliseconds(void*) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

ayn::rsinput::HandshakeReadResult ReadHandshakeBytes(
    void* context, uint8_t* data, size_t capacity, size_t* size,
    uint32_t timeout_ms) {
  const int uart_fd = *static_cast<int*>(context);
  const uint64_t started_ms = MonotonicMilliseconds(nullptr);
  *size = 0;
  while (g_stop_requested == 0) {
    const uint64_t elapsed_ms = MonotonicMilliseconds(nullptr) - started_ms;
    if (elapsed_ms >= timeout_ms) {
      return ayn::rsinput::HandshakeReadResult::kTimeout;
    }
    const uint32_t remaining_ms =
        timeout_ms - static_cast<uint32_t>(elapsed_ms);
    const uint32_t wait_ms = std::min(remaining_ms, kStopCheckIntervalMs);
    pollfd uart_poll{uart_fd, POLLIN, 0};
    const int poll_result = poll(&uart_poll, 1, static_cast<int>(wait_ms));
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      PLOG(ERROR) << "RSInput UART handshake poll failed";
      return ayn::rsinput::HandshakeReadResult::kFailed;
    }
    if (poll_result == 0) {
      continue;
    }
    if ((uart_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      LOG(ERROR) << "RSInput UART disconnected during handshake";
      return ayn::rsinput::HandshakeReadResult::kFailed;
    }
    if ((uart_poll.revents & POLLIN) == 0) {
      continue;
    }

    // The stock VMIN=16 setting belongs to a dedicated RX thread. Startup TX
    // shares this loop, so a one-byte read keeps every 100 ms deadline live.
    const ssize_t received = read(uart_fd, data, std::min<size_t>(capacity, 1));
    const int read_error = errno;
    const ayn::rsinput::UartReadDisposition disposition =
        ayn::rsinput::ClassifyUartRead(received, read_error);
    if (disposition == ayn::rsinput::UartReadDisposition::kRetry) {
      continue;
    }
    if (disposition == ayn::rsinput::UartReadDisposition::kFailed) {
      errno = read_error;
      PLOG(ERROR) << "RSInput UART handshake read failed";
      return ayn::rsinput::HandshakeReadResult::kFailed;
    }
    if (disposition == ayn::rsinput::UartReadDisposition::kClosed) {
      LOG(ERROR) << "RSInput UART closed during handshake";
      return ayn::rsinput::HandshakeReadResult::kFailed;
    }
    *size = static_cast<size_t>(received);
    return ayn::rsinput::HandshakeReadResult::kData;
  }
  return ayn::rsinput::HandshakeReadResult::kFailed;
}

const char* HandshakeStateName(ayn::rsinput::HandshakeState state) {
  switch (state) {
    case ayn::rsinput::HandshakeState::kNotStarted:
      return "not-started";
    case ayn::rsinput::HandshakeState::kAwaitingType1:
      return "awaiting-type-1";
    case ayn::rsinput::HandshakeState::kSendConfiguration:
      return "send-configuration";
    case ayn::rsinput::HandshakeState::kAwaitingType2:
      return "awaiting-type-2";
    case ayn::rsinput::HandshakeState::kInitialized:
      return "initialized";
    case ayn::rsinput::HandshakeState::kFailed:
      return "failed";
  }
  return "unknown";
}

const char* HandshakeFailureName(ayn::rsinput::HandshakeFailure failure) {
  switch (failure) {
    case ayn::rsinput::HandshakeFailure::kNone:
      return "none";
    case ayn::rsinput::HandshakeFailure::kInvalidConfiguration:
      return "invalid-configuration";
    case ayn::rsinput::HandshakeFailure::kWrite:
      return "write";
    case ayn::rsinput::HandshakeFailure::kWait:
      return "wait";
    case ayn::rsinput::HandshakeFailure::kRead:
      return "read";
    case ayn::rsinput::HandshakeFailure::kTimeout:
      return "timeout";
    case ayn::rsinput::HandshakeFailure::kProtocol:
      return "protocol";
  }
  return "unknown";
}

struct EventEmitter {
  int uinput_fd;
  ayn::rsinput::ControllerProfileService* profile_service;
  bool failed = false;
};

void EmitStatus(void* context, const ayn::rsinput::Status& status) {
  auto* emitter = static_cast<EventEmitter*>(context);
  if (emitter->failed) {
    return;
  }
  const auto events = emitter->profile_service->MapStatusToEvents(status);
  for (const ayn::rsinput::InputEvent& event : events) {
    input_event linux_event{};
    linux_event.type = event.type;
    linux_event.code = event.code;
    linux_event.value = event.value;
    if (!WriteAll(emitter->uinput_fd, &linux_event, sizeof(linux_event))) {
      emitter->failed = true;
      return;
    }
  }
}

ayn::rsinput::StartupResult ForwardStatusFrames(void* context, int uart_fd,
                                                int uinput_fd) {
  ayn::rsinput::Parser parser;
  auto* runtime = static_cast<RuntimeContext*>(context);
  if (runtime == nullptr || runtime->profile_service == nullptr) {
    return ayn::rsinput::StartupResult::kFailed;
  }
  EventEmitter emitter{uinput_fd, runtime->profile_service};
  ayn::rsinput::RuntimeStreamWatchdog stream_watchdog(
      kRuntimeStatusIdleTimeoutMs);
  std::array<uint8_t, 256> buffer{};
  uint64_t last_watchdog_ms = MonotonicMilliseconds(nullptr);

  const auto status_stream_expired =
      [&stream_watchdog, &last_watchdog_ms](bool status_observed) {
        const uint64_t now_ms = MonotonicMilliseconds(nullptr);
        const uint64_t elapsed_ms = now_ms - last_watchdog_ms;
        last_watchdog_ms = now_ms;
        if (status_observed) {
          stream_watchdog.ObserveStatus();
          return false;
        }
        return stream_watchdog.ObserveElapsed(static_cast<uint32_t>(
            std::min<uint64_t>(elapsed_ms,
                               std::numeric_limits<uint32_t>::max())));
      };

  while (g_stop_requested == 0 && !emitter.failed) {
    pollfd uart_poll{uart_fd, POLLIN, 0};
    const int poll_result =
        poll(&uart_poll, 1, static_cast<int>(kStopCheckIntervalMs));
    if (poll_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      PLOG(ERROR) << "RSInput UART poll failed";
      return ayn::rsinput::StartupResult::kFailed;
    }
    if (poll_result == 0) {
      if (status_stream_expired(false)) {
        LOG(WARNING) << "RSInput UART status stream idle; reconnecting";
        return ayn::rsinput::StartupResult::kFailed;
      }
      continue;
    }
    if ((uart_poll.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      LOG(ERROR) << "RSInput UART disconnected";
      return ayn::rsinput::StartupResult::kFailed;
    }
    if ((uart_poll.revents & POLLIN) == 0) {
      continue;
    }

    const ssize_t received = read(uart_fd, buffer.data(), buffer.size());
    const int read_error = errno;
    const ayn::rsinput::UartReadDisposition disposition =
        ayn::rsinput::ClassifyUartRead(received, read_error);
    if (disposition == ayn::rsinput::UartReadDisposition::kRetry) {
      if (status_stream_expired(false)) {
        LOG(WARNING) << "RSInput UART status stream idle; reconnecting";
        return ayn::rsinput::StartupResult::kFailed;
      }
      if (g_stop_requested != 0) {
        return ayn::rsinput::StartupResult::kStopped;
      }
      continue;
    }
    if (disposition == ayn::rsinput::UartReadDisposition::kFailed) {
      errno = read_error;
      PLOG(ERROR) << "RSInput UART read failed";
      return ayn::rsinput::StartupResult::kFailed;
    }
    if (disposition == ayn::rsinput::UartReadDisposition::kClosed) {
      LOG(ERROR) << "RSInput UART closed";
      return ayn::rsinput::StartupResult::kFailed;
    }
    const size_t accepted_before = parser.stats().accepted_status_frames;
    parser.Feed(buffer.data(), static_cast<size_t>(received), EmitStatus,
                &emitter);
    const bool status_observed =
        parser.stats().accepted_status_frames != accepted_before;
    if (status_stream_expired(status_observed)) {
      LOG(WARNING) << "RSInput UART status stream idle; reconnecting";
      return ayn::rsinput::StartupResult::kFailed;
    }
  }
  return g_stop_requested != 0 ? ayn::rsinput::StartupResult::kStopped
                               : ayn::rsinput::StartupResult::kFailed;
}

ayn::rsinput::StartupResult InitializeRuntimeSession(void*, int uart_fd) {
  const ayn::rsinput::HandshakeCallbacks callbacks = {
      StopWasRequested, WriteInitializationFrame, ReadHandshakeBytes,
      WaitBeforeInitializationFrame, MonotonicMilliseconds, &uart_fd,
  };
  ayn::rsinput::HandshakeDiagnostics diagnostics;
  const ayn::rsinput::StartupResult result = ayn::rsinput::RunQ9Handshake(
      callbacks, kHandshakeResponseTimeoutMs, kMcuCommandIntervalMs,
      &diagnostics);
  if (result == ayn::rsinput::StartupResult::kFailed) {
    LOG(ERROR) << "RSInput Q9 handshake failed"
               << " reason=" << HandshakeFailureName(diagnostics.failure)
               << " state=" << HandshakeStateName(diagnostics.state)
               << " expected_response_type="
               << static_cast<unsigned int>(
                      diagnostics.expected_response_type)
               << " malformed_frames="
               << diagnostics.stats.malformed_frames
               << " unrelated_packets="
               << diagnostics.stats.unrelated_packets;
  }
  return result;
}

void WaitBeforeRetry(void*, uint32_t delay_ms) {
  LOG(WARNING) << "reconnecting RSInput in " << delay_ms << " ms";
  uint32_t remaining_ms = delay_ms;
  while (remaining_ms != 0 && g_stop_requested == 0) {
    const uint32_t wait_ms = remaining_ms > kStopCheckIntervalMs
                                 ? kStopCheckIntervalMs
                                 : remaining_ms;
    const int result = poll(nullptr, 0, static_cast<int>(wait_ms));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      PLOG(ERROR) << "RSInput retry wait failed";
      return;
    }
    remaining_ms -= wait_ms;
  }
}

int RunSupportedDevice(void* context) {
  auto* runtime = static_cast<RuntimeContext*>(context);
  if (runtime == nullptr ||
      !ayn::rsinput::IsSupportedControllerDevice(runtime->product_device)) {
    return EXIT_FAILURE;
  }
  if (!InstallStopHandlers()) {
    return EXIT_FAILURE;
  }

  auto profile_service = std::make_unique<ayn::rsinput::ControllerProfileService>(
      runtime->product_device, ayn::rsinput::ProductionControllerProfileStore());
  const ayn::rsinput::ControllerProfileResponse startup =
      profile_service->Initialize();
  if (startup.result != ayn::rsinput::ControllerProfileResult::kOk) {
    LOG(ERROR) << "cannot initialize controller profile; result="
               << AidlProfileResult(startup.result);
    return EXIT_FAILURE;
  }
  auto binder =
      ::ndk::SharedRefBase::make<OdinControllerBinder>(profile_service.get());
  ABinderProcess_setThreadPoolMaxThreadCount(1);
  ABinderProcess_startThreadPool();
  const binder_status_t registration = AServiceManager_addService(
      binder->asBinder().get(), kControllerServiceName);
  if (registration != STATUS_OK) {
    LOG(ERROR) << "failed to register " << kControllerServiceName
               << "; status=" << registration;
    return EXIT_FAILURE;
  }
  LOG(INFO) << "registered " << kControllerServiceName;
  runtime->profile_service = profile_service.get();

  const ayn::rsinput::LifecycleCallbacks callbacks = {
      StopWasRequested,       LeaveRuntimeMcuPowerUnchanged,
      LeaveRuntimeMcuPowerUnchanged, SkipRuntimeMcuPowerSettle,
      OpenRuntimeUart,        OpenRuntimeUinput,
      CloseRuntimeUart,       CloseRuntimeUinput,   InitializeRuntimeSession,
      ForwardStatusFrames,    WaitBeforeRetry,      runtime,
  };
  const ayn::rsinput::StartupResult result = ayn::rsinput::RunReconnectLoop(
      callbacks, {kInitialRetryDelayMs, kMaxRetryDelayMs});
  runtime->profile_service = nullptr;
  return result == ayn::rsinput::StartupResult::kStopped ? EXIT_SUCCESS
                                                         : EXIT_FAILURE;
}

}  // namespace

int main() {
  RuntimeContext runtime = {
      android::base::GetProperty("ro.product.device", ""),
      nullptr,
  };
  const int result = ayn::rsinput::RunIfSupportedDevice(
      runtime.product_device, RunSupportedDevice, &runtime);
  if (!ayn::rsinput::IsSupportedDevice(runtime.product_device)) {
    LOG(ERROR) << "rsinputd is disabled for this product device";
  }
  return result;
}
