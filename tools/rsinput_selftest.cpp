// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>

namespace {

constexpr char kUinputPath[] = "/dev/uinput";
constexpr char kDeviceName[] = "AYN Odin2 Gamepad Selftest";
constexpr uint16_t kVendorId = 0x2020;
constexpr uint16_t kProductId = 0x3001;

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (size != 0) {
    const ssize_t written = write(fd, bytes, size);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      errno = EIO;
      return false;
    }
    bytes += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool SleepMs(int milliseconds) {
  while (poll(nullptr, 0, milliseconds) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return true;
}

bool EnableCapability(int fd, unsigned long request, uint16_t code) {
  return ioctl(fd, request, code) == 0;
}

bool CreateGamepad(int fd) {
  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) != 0 ||
      ioctl(fd, UI_SET_EVBIT, EV_ABS) != 0) {
    return false;
  }

  constexpr std::array<uint16_t, 16> kKeys = {
      BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
      BTN_NORTH,   BTN_WEST,      BTN_EAST,      BTN_SOUTH,
      BTN_TL,      BTN_TR,        BTN_SELECT,    BTN_START,
      BTN_THUMBL,  BTN_THUMBR,    BTN_MODE,      BTN_BACK,
  };
  for (uint16_t key : kKeys) {
    if (!EnableCapability(fd, UI_SET_KEYBIT, key)) {
      return false;
    }
  }

  constexpr std::array<uint16_t, 6> kAxes = {ABS_X, ABS_Y, ABS_RX,
                                              ABS_RY, ABS_Z, ABS_RZ};
  for (uint16_t axis : kAxes) {
    if (!EnableCapability(fd, UI_SET_ABSBIT, axis)) {
      return false;
    }
  }

  uinput_user_dev device{};
  snprintf(device.name, sizeof(device.name), "%s", kDeviceName);
  device.id.bustype = BUS_USB;
  device.id.vendor = kVendorId;
  device.id.product = kProductId;
  device.id.version = 1;
  for (uint16_t axis : {ABS_X, ABS_Y, ABS_RX, ABS_RY}) {
    device.absmin[axis] = -32768;
    device.absmax[axis] = 32768;
  }
  for (uint16_t axis : {ABS_Z, ABS_RZ}) {
    device.absmin[axis] = 0;
    device.absmax[axis] = 0x610;
  }

  return WriteAll(fd, &device, sizeof(device)) &&
         ioctl(fd, UI_DEV_CREATE) == 0;
}

bool Emit(int fd, uint16_t type, uint16_t code, int32_t value) {
  input_event event{};
  event.type = type;
  event.code = code;
  event.value = value;
  return WriteAll(fd, &event, sizeof(event));
}

bool Sync(int fd) {
  return Emit(fd, EV_SYN, SYN_REPORT, 0);
}

bool EmitTestSequence(int fd) {
  // One face-button transition and non-edge axis values exercise both Android
  // keylayout and joystick motion without leaving any input held on exit.
  if (!Emit(fd, EV_KEY, BTN_SOUTH, 1) ||
      !Emit(fd, EV_ABS, ABS_X, 16384) ||
      !Emit(fd, EV_ABS, ABS_Y, -16384) || !Sync(fd) || !SleepMs(300)) {
    return false;
  }
  return Emit(fd, EV_KEY, BTN_SOUTH, 0) && Emit(fd, EV_ABS, ABS_X, 0) &&
         Emit(fd, EV_ABS, ABS_Y, 0) && Sync(fd);
}

}  // namespace

int main() {
  const int fd = open(kUinputPath, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    fprintf(stderr, "selftest: open %s failed: %s\n", kUinputPath,
            strerror(errno));
    return 1;
  }

  if (!CreateGamepad(fd)) {
    fprintf(stderr, "selftest: create gamepad failed: %s\n", strerror(errno));
    close(fd);
    return 1;
  }

  printf("selftest: device-ready name=%s vendor=%04x product=%04x\n",
         kDeviceName, kVendorId, kProductId);
  fflush(stdout);
  const bool passed = SleepMs(2000) && EmitTestSequence(fd) && SleepMs(2000);
  const int saved_errno = errno;
  if (ioctl(fd, UI_DEV_DESTROY) != 0) {
    fprintf(stderr, "selftest: destroy gamepad failed: %s\n", strerror(errno));
  }
  close(fd);

  if (!passed) {
    fprintf(stderr, "selftest: event sequence failed: %s\n",
            strerror(saved_errno));
    return 1;
  }
  puts("selftest: sequence-complete");
  return 0;
}
