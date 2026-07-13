// SPDX-License-Identifier: Apache-2.0

#include "ayn/rsinput_mapping.h"
#include "ayn/rsinput_lifecycle.h"
#include "ayn/rsinput_parser.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <array>
#include <cerrno>
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

class FileDescriptor {
 public:
  explicit FileDescriptor(int fd = -1) : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

 private:
  int fd_;
};

class UinputDevice {
 public:
  explicit UinputDevice(int fd) : fd_(fd) {}
  ~UinputDevice() {
    if (created_) {
      ioctl(fd_, UI_DEV_DESTROY);
    }
  }

  void MarkCreated() { created_ = true; }

 private:
  int fd_;
  bool created_ = false;
};

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (size != 0) {
    const ssize_t written = write(fd, bytes, size);
    if (written < 0) {
      if (errno == EINTR) {
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

bool WriteInitializationFrame(void* context, const uint8_t* data, size_t size) {
  return WriteAll(*static_cast<int*>(context), data, size);
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

int ForwardStatusFrames(int uart_fd, int uinput_fd) {
  ayn::rsinput::Parser parser;
  EventEmitter emitter{uinput_fd};
  std::array<uint8_t, 256> buffer{};

  while (g_stop_requested == 0 && !emitter.failed) {
    const ssize_t received = read(uart_fd, buffer.data(), buffer.size());
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      PLOG(ERROR) << "RSInput UART read failed";
      return EXIT_FAILURE;
    }
    if (received == 0) {
      LOG(ERROR) << "RSInput UART closed";
      return EXIT_FAILURE;
    }
    parser.Feed(buffer.data(), static_cast<size_t>(received), EmitStatus,
                &emitter);
  }
  return emitter.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

int RunSupportedDevice(void*) {
  if (!InstallStopHandlers()) {
    return EXIT_FAILURE;
  }

  int uart_fd = -1;
  const ayn::rsinput::StartupResult open_result =
      ayn::rsinput::OpenUartUnlessStopped(StopWasRequested, nullptr,
                                         OpenRuntimeUart, nullptr, &uart_fd);
  FileDescriptor uart(uart_fd);
  if (open_result == ayn::rsinput::StartupResult::kStopped) {
    return EXIT_SUCCESS;
  }
  if (open_result == ayn::rsinput::StartupResult::kFailed) {
    return EXIT_FAILURE;
  }
  FileDescriptor uinput(open(kUinputPath, O_WRONLY | O_CLOEXEC));
  if (!uinput.valid()) {
    PLOG(ERROR) << "cannot open uinput";
    return EXIT_FAILURE;
  }
  if (!CreateUinputGamepad(uinput.get())) {
    return EXIT_FAILURE;
  }
  UinputDevice gamepad(uinput.get());
  gamepad.MarkCreated();

  const ayn::rsinput::StartupResult initialization_result =
      ayn::rsinput::SendInitializationFramesUnlessStopped(
          StopWasRequested, nullptr, WriteInitializationFrame, &uart_fd);
  if (initialization_result == ayn::rsinput::StartupResult::kStopped) {
    return EXIT_SUCCESS;
  }
  if (initialization_result == ayn::rsinput::StartupResult::kFailed) {
    return EXIT_FAILURE;
  }
  return ForwardStatusFrames(uart.get(), uinput.get());
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
