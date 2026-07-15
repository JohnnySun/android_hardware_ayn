#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ -z "${CXX:-}" ]; then
  if command -v clang++ >/dev/null 2>&1; then
    CXX=clang++
  elif command -v c++ >/dev/null 2>&1; then
    CXX=c++
  else
    echo "error: no clang++ or c++ compiler found" >&2
    exit 1
  fi
fi

BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/rsinputd-host-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT INT TERM

echo "CXX=$CXX"
"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/rsinput_parser.cpp" \
  "$ROOT/tests/rsinput_parser_test.cpp" \
  -o "$BUILD_DIR/rsinput_parser_test"
"$BUILD_DIR/rsinput_parser_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/rsinput_parser.cpp" \
  "$ROOT/src/rsinput_protocol.cpp" \
  "$ROOT/src/rsinput_mapping.cpp" \
  "$ROOT/tests/rsinputd_policy_test.cpp" \
  -o "$BUILD_DIR/rsinputd_policy_test"
"$BUILD_DIR/rsinputd_policy_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/rsinput_poll.cpp" \
  "$ROOT/tests/rsinput_poll_test.cpp" \
  -o "$BUILD_DIR/rsinput_poll_test"
"$BUILD_DIR/rsinput_poll_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/rsinput_parser.cpp" \
  "$ROOT/src/rsinput_poll.cpp" \
  "$ROOT/src/rsinput_protocol.cpp" \
  "$ROOT/src/rsinput_lifecycle.cpp" \
  "$ROOT/tests/rsinput_lifecycle_test.cpp" \
  -o "$BUILD_DIR/rsinput_lifecycle_test"
"$BUILD_DIR/rsinput_lifecycle_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/fan_policy.cpp" \
  "$ROOT/src/fan_lifecycle.cpp" \
  "$ROOT/tests/fan_service_test.cpp" \
  -o "$BUILD_DIR/fan_service_test"
"$BUILD_DIR/fan_service_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -pthread \
  -I"$ROOT/include" \
  "$ROOT/src/fan_policy.cpp" \
  "$ROOT/src/fan_lifecycle.cpp" \
  "$ROOT/src/fan_service.cpp" \
  "$ROOT/tests/fan_transaction_test.cpp" \
  -o "$BUILD_DIR/fan_transaction_test"
"$BUILD_DIR/fan_transaction_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/fan_policy.cpp" \
  "$ROOT/src/fan_lifecycle.cpp" \
  "$ROOT/src/fan_adapter.cpp" \
  "$ROOT/tests/fan_adapter_test.cpp" \
  -o "$BUILD_DIR/fan_adapter_test"
"$BUILD_DIR/fan_adapter_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/fan_status.cpp" \
  "$ROOT/tests/fan_status_test.cpp" \
  -o "$BUILD_DIR/fan_status_test"
"$BUILD_DIR/fan_status_test"

if grep -Eq \
     'SysfsWriter|writer_context|write_file|::write|(^|[^[:alnum:]_])p?write[[:space:]]*\(' \
     "$ROOT/include/ayn/fan_status.h" "$ROOT/src/fan_status.cpp"; then
  echo "error: fan status core must not expose or invoke a write surface" >&2
  exit 1
fi

fan_status_module="$(sed -n \
  '/name: "libayn_fan_status_core"/,/^}/p' "$ROOT/Android.bp")"
if printf '%s\n' "$fan_status_module" | grep -q 'libayn_fan_service_core'; then
  echo "error: fan status core must not link the write-capable fan service" >&2
  exit 1
fi
if printf '%s\n' "$fan_status_module" | grep -q 'libayn_fan_policy'; then
  echo "error: fan status core must not inherit unvalidated write policy" >&2
  exit 1
fi

if ! grep -qx '    disabled' "$ROOT/rsinputd.rc" ||
   ! grep -qx '    oneshot' "$ROOT/rsinputd.rc"; then
  echo "error: daemon-owned retries require rsinputd to remain disabled and oneshot" >&2
  exit 1
fi

if grep -Eq \
     '^[[:space:]]*(restart_period|critical|reboot_on_failure|onrestart)([[:space:]]|$)|^[[:space:]]*restart[[:space:]]+rsinputd([[:space:]]|$)' \
     "$ROOT/rsinputd.rc"; then
  echo "error: init must not add an independent rsinputd restart policy" >&2
  exit 1
fi

if ! grep -qx 'service rsinputd /system/bin/rsinputd' "$ROOT/rsinputd.rc" ||
   grep -Eq '^[[:space:]]*vendor:[[:space:]]*true' "$ROOT/Android.bp"; then
  echo "error: system-only bring-up must install rsinputd in system" >&2
  exit 1
fi

selftest_module="$(sed -n \
  '/^cc_test {/,/^}/p' "$ROOT/Android.bp" |
  sed -n '/name: "ayn_rsinput_selftest"/,/^}/p')"
if ! printf '%s\n' "$selftest_module" | grep -qx '    name: "ayn_rsinput_selftest",' ||
   ! printf '%s\n' "$selftest_module" | grep -qx '    srcs: \["tools/rsinput_selftest.cpp"\],' ||
   printf '%s\n' "$selftest_module" | grep -Eq 'init_rc|required|product_packages'; then
  echo "error: RSInput selftest must remain manually built and product-excluded" >&2
  exit 1
fi

if grep -Eq '/dev/tty|/dev/rscom|/sys/|mcupower|gpio|fan' \
     "$ROOT/tools/rsinput_selftest.cpp"; then
  echo "error: RSInput selftest must not touch UART, MCU, sysfs, or fan controls" >&2
  exit 1
fi

if ! grep -qx 'on boot && property:ro.product.device=odin2_mini' "$ROOT/rsinputd.rc" ||
   ! grep -qx '    start rsinputd' "$ROOT/rsinputd.rc"; then
  echo "error: rsinputd must start once at boot before Setup Wizard on Odin2 Mini" >&2
  exit 1
fi

runtime_callbacks="$(sed -n \
  '/const ayn::rsinput::LifecycleCallbacks callbacks = {/,/};/p' \
  "$ROOT/src/rsinputd.cpp")"
if ! printf '%s\n' "$runtime_callbacks" | grep -q \
     'LeaveRuntimeMcuPowerUnchanged' ||
   printf '%s\n' "$runtime_callbacks" | grep -Eq \
     'PowerOnRuntimeMcu|PowerOffRuntimeMcu|SettleAfterMcuPowerOn'; then
  echo "error: stock startup must not toggle or settle MCU power" >&2
  exit 1
fi

if ! grep -q 'read(uart_fd, data, std::min<size_t>(capacity, 1))' \
     "$ROOT/src/rsinputd.cpp"; then
  echo "error: stock VMIN must not block the read-driven startup TX cadence" >&2
  exit 1
fi

for assignment in \
  'device.absmin[axis] = ayn::rsinput::kStickAxisMin;' \
  'device.absmax[axis] = ayn::rsinput::kStickAxisMax;' \
  'device.absflat[axis] = ayn::rsinput::kStickAxisFlat;' \
  'device.absmin[axis] = ayn::rsinput::kTriggerAxisMin;' \
  'device.absmax[axis] = ayn::rsinput::kTriggerAxisMax;' \
  'device.absflat[axis] = ayn::rsinput::kTriggerAxisFlat;'; do
  if ! grep -Fqx "    $assignment" "$ROOT/src/rsinputd.cpp"; then
    echo "error: rsinputd must wire the tested Odin axis profile into uinput" >&2
    exit 1
  fi
done

odinfand_expected="$BUILD_DIR/odinfand.rc.expected"
cat >"$odinfand_expected" <<'EOF'
# SPDX-License-Identifier: Apache-2.0

service odinfand /system/bin/odinfand
    class late_start
    user system
    group system
EOF
if ! cmp -s "$odinfand_expected" "$ROOT/odinfand.rc"; then
  echo "error: odinfand rc must exactly define the persistent late_start system service" >&2
  exit 1
fi

if grep -Eq '^[[:space:]]*on |^[[:space:]]*start odinfand' "$ROOT/odinfand.rc"; then
  echo "error: odinfand must rely only on its product-selected late_start class" >&2
  exit 1
fi

odinfand_module="$(sed -n '/^[[:space:]]*cc_binary[[:space:]]*{/,/^}/p' "$ROOT/Android.bp" |
  sed -n '/name: "odinfand"/,/^}/p')"
if ! printf '%s\n' "$odinfand_module" | grep -qx '    name: "odinfand",' ||
   ! printf '%s\n' "$odinfand_module" | grep -qx '    srcs: \["src/odinfand.cpp"\],' ||
   ! printf '%s\n' "$odinfand_module" | grep -qx '    init_rc: \["odinfand.rc"\],' ||
   printf '%s\n' "$odinfand_module" | grep -Eq '^[[:space:]]*vendor:[[:space:]]*true'; then
  echo "error: system odinfand binary and init rc wiring must remain exact" >&2
  exit 1
fi

if ! grep -q 'name: "com.ayn.fan"' "$ROOT/Android.bp" ||
   ! grep -q 'local_include_dir: "aidl"' "$ROOT/Android.bp" ||
   ! grep -q 'unstable: true' "$ROOT/Android.bp" ||
   ! grep -A8 'java: {' "$ROOT/Android.bp" | grep -q 'enabled: true' ||
   ! grep -qx 'interface IOdinFan {' "$ROOT/aidl/com/ayn/fan/IOdinFan.aidl" ||
   ! grep -qx '    FanResponse getStatus();' "$ROOT/aidl/com/ayn/fan/IOdinFan.aidl" ||
   ! grep -qx '    FanResponse setMode(int mode, in IBinder owner);' "$ROOT/aidl/com/ayn/fan/IOdinFan.aidl"; then
  echo "error: private unstable IOdinFan AIDL contract is incomplete" >&2
  exit 1
fi

if grep -Eq 'String.*path|int.*(duty|period)' "$ROOT/aidl/com/ayn/fan/IOdinFan.aidl"; then
  echo "error: IOdinFan callers must not supply paths, duty, or period" >&2
  exit 1
fi

if ! grep -q 'com.ayn.fan.IOdinFan/default' "$ROOT/src/odinfand.cpp" ||
   ! grep -q 'AServiceManager_addService' "$ROOT/src/odinfand.cpp" ||
   grep -Eq 'SetProperty|socket\(|AF_UNIX' "$ROOT/src/odinfand.cpp"; then
  echo "error: odinfand must register Binder directly without property or UDS control" >&2
  exit 1
fi
