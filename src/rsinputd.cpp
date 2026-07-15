// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_mapping.h"
#include "ayn/rsinput_lifecycle.h"
#include "ayn/rsinput_parser.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
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
#include <string>

namespace {

constexpr char kUartPath[] = "/dev/ttyHS1";
constexpr char kUinputPath[] = "/dev/uinput";
constexpr char kGamepadName[] = "AYN Odin2 Gamepad";
constexpr uint32_t kHandshakeResponseTimeoutMs = 1000;
constexpr uint32_t kMcuPowerSettleMs = 100;
constexpr uint32_t kMcuCommandIntervalMs = 100;
constexpr uint32_t kInitialRetryDelayMs = 250;
constexpr uint32_t kMaxRetryDelayMs = 5000;
constexpr uint32_t kStopCheckIntervalMs = 50;

volatile sig_atomic_t g_stop_requested = 0;

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

bool WriteMcuPowerControl(void*, const char* path, const uint8_t* data,
                          size_t size) {
  const int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    PLOG(ERROR) << "cannot open RSInput MCU power control";
    return false;
  }
  const bool written = WriteAll(fd, data, size);
  if (close(fd) != 0) {
    PLOG(ERROR) << "cannot close RSInput MCU power control";
    return false;
  }
  return written;
}

bool SetRuntimeMcuPower(bool enabled) {
  if (ayn::rsinput::WriteMcuPowerState(WriteMcuPowerControl, nullptr,
                                       enabled)) {
    return true;
  }
  LOG(ERROR) << "cannot set RSInput MCU power state to "
             << (enabled ? "on" : "off");
  return false;
}

bool PowerOnRuntimeMcu(void*) {
  return SetRuntimeMcuPower(true);
}

bool PowerOffRuntimeMcu(void*) {
  return SetRuntimeMcuPower(false);
}

ayn::rsinput::StartupResult SettleAfterMcuPowerOn(void*) {
  uint32_t remaining_ms = kMcuPowerSettleMs;
  while (remaining_ms != 0 && g_stop_requested == 0) {
    const uint32_t wait_ms = std::min(remaining_ms, kStopCheckIntervalMs);
    const int result = poll(nullptr, 0, static_cast<int>(wait_ms));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      PLOG(ERROR) << "RSInput MCU power settle failed";
      return ayn::rsinput::StartupResult::kFailed;
    }
    remaining_ms -= wait_ms;
  }
  return g_stop_requested != 0 ? ayn::rsinput::StartupResult::kStopped
                               : ayn::rsinput::StartupResult::kCompleted;
}

int OpenConfiguredUart() {
  const int fd = open(kUartPath, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd < 0) {
    PLOG(ERROR) << "cannot open RSInput UART";
    return -1;
  }

  termios settings{};
  if (tcgetattr(fd, &settings) != 0) {
    PLOG(ERROR) << "cannot read RSInput UART settings";
    close(fd);
    return -1;
  }

  if (tcflush(fd, TCIFLUSH) != 0) {
    PLOG(ERROR) << "cannot flush RSInput UART";
    close(fd);
    return -1;
  }

  settings.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | IGNCR | IXON |
                        IXOFF | IXANY | INPCK | ISTRIP);
  settings.c_oflag = 0;
  settings.c_lflag = 0;
  settings.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
  settings.c_cflag |= CS8 | CLOCAL | CREAD;
  settings.c_cc[VMIN] = 1;
  settings.c_cc[VTIME] = 0;
  if (cfsetispeed(&settings, B115200) != 0 ||
      cfsetospeed(&settings, B115200) != 0 ||
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
    device.absmin[axis] = -32768;
    device.absmax[axis] = 32768;
  }
  for (uint16_t axis : {ayn::rsinput::kAbsZ, ayn::rsinput::kAbsRz}) {
    device.absmin[axis] = 0;
    device.absmax[axis] = 0x610;
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
  return WriteAll(*static_cast<int*>(context), data, size);
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

    const ssize_t received = read(uart_fd, data, capacity);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      PLOG(ERROR) << "RSInput UART handshake read failed";
      return ayn::rsinput::HandshakeReadResult::kFailed;
    }
    if (received == 0) {
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
  bool failed = false;
};

void EmitStatus(void* context, const ayn::rsinput::Status& status) {
  auto* emitter = static_cast<EventEmitter*>(context);
  if (emitter->failed) {
    return;
  }
  const auto events = ayn::rsinput::MapStatusToEvents(status);
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

ayn::rsinput::StartupResult ForwardStatusFrames(void*, int uart_fd,
                                                int uinput_fd) {
  ayn::rsinput::Parser parser;
  EventEmitter emitter{uinput_fd};
  std::array<uint8_t, 256> buffer{};

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
    if (received < 0) {
      if (errno == EINTR) {
        if (g_stop_requested != 0) {
          return ayn::rsinput::StartupResult::kStopped;
        }
        continue;
      }
      PLOG(ERROR) << "RSInput UART read failed";
      return ayn::rsinput::StartupResult::kFailed;
    }
    if (received == 0) {
      LOG(ERROR) << "RSInput UART closed";
      return ayn::rsinput::StartupResult::kFailed;
    }
    parser.Feed(buffer.data(), static_cast<size_t>(received), EmitStatus,
                &emitter);
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

int RunSupportedDevice(void*) {
  if (!InstallStopHandlers()) {
    return EXIT_FAILURE;
  }

  const ayn::rsinput::LifecycleCallbacks callbacks = {
      StopWasRequested,       PowerOnRuntimeMcu,    PowerOffRuntimeMcu,
      SettleAfterMcuPowerOn,  OpenRuntimeUart,      OpenRuntimeUinput,
      CloseRuntimeUart,       CloseRuntimeUinput,   InitializeRuntimeSession,
      ForwardStatusFrames,    WaitBeforeRetry,      nullptr,
  };
  const ayn::rsinput::StartupResult result = ayn::rsinput::RunReconnectLoop(
      callbacks, {kInitialRetryDelayMs, kMaxRetryDelayMs});
  return result == ayn::rsinput::StartupResult::kStopped ? EXIT_SUCCESS
                                                         : EXIT_FAILURE;
}

}  // namespace

int main() {
  const std::string product_device =
      android::base::GetProperty("ro.product.device", "");
  const int result = ayn::rsinput::RunIfSupportedDevice(
      product_device, RunSupportedDevice, nullptr);
  if (!ayn::rsinput::IsSupportedDevice(product_device)) {
    LOG(ERROR) << "rsinputd is disabled for this product device";
  }
  return result;
}
