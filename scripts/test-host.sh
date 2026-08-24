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

python3 "$ROOT/tests/aidl_contract_test.py"
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
  "$ROOT/src/rsinput_parser.cpp" \
  "$ROOT/src/rsinput_mapping.cpp" \
  "$ROOT/src/controller_profile.cpp" \
  "$ROOT/tests/controller_profile_test.cpp" \
  -pthread \
  -o "$BUILD_DIR/controller_profile_test"
"$BUILD_DIR/controller_profile_test"

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
  "$ROOT/src/rsinput_uart.cpp" \
  "$ROOT/tests/rsinput_uart_test.cpp" \
  -o "$BUILD_DIR/rsinput_uart_test"
"$BUILD_DIR/rsinput_uart_test"

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

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/charge_policy.cpp" \
  "$ROOT/tests/charge_policy_test.cpp" \
  -o "$BUILD_DIR/charge_policy_test"
"$BUILD_DIR/charge_policy_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/lights_policy.cpp" \
  "$ROOT/tests/lights_policy_test.cpp" \
  -o "$BUILD_DIR/lights_policy_test"
"$BUILD_DIR/lights_policy_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/lights_policy.cpp" \
  "$ROOT/src/lights_lifecycle.cpp" \
  "$ROOT/tests/lights_lifecycle_test.cpp" \
  -o "$BUILD_DIR/lights_lifecycle_test"
"$BUILD_DIR/lights_lifecycle_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/performance_service.cpp" \
  "$ROOT/tests/performance_service_test.cpp" \
  -o "$BUILD_DIR/performance_service_test"
"$BUILD_DIR/performance_service_test"

"$CXX" \
  -std=c++17 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/include" \
  "$ROOT/src/performance_adapter.cpp" \
  "$ROOT/tests/performance_adapter_test.cpp" \
  -o "$BUILD_DIR/performance_adapter_test"
"$BUILD_DIR/performance_adapter_test"

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

controller_module="$(sed -n \
  '/^[[:space:]]*cc_binary[[:space:]]*{/,/^}/p' "$ROOT/Android.bp" |
  sed -n '/name: "rsinputd"/,/^}/p')"
if ! printf '%s\n' "$controller_module" | grep -qx '    name: "rsinputd",' ||
   ! printf '%s\n' "$controller_module" | grep -qx \
      '        "com.ayn.controller-ndk",'; then
  echo "error: rsinputd must link the controller Binder interface" >&2
  exit 1
fi

if ! grep -q 'name: "com.ayn.controller"' "$ROOT/Android.bp" ||
   ! grep -qx 'interface IOdinController {' \
      "$ROOT/aidl/com/ayn/controller/IOdinController.aidl" ||
   ! grep -qx '    ControllerProfileResponse getProfile();' \
      "$ROOT/aidl/com/ayn/controller/IOdinController.aidl" ||
   ! grep -qx '    ControllerProfileResponse setProfile(int profile);' \
      "$ROOT/aidl/com/ayn/controller/IOdinController.aidl" ||
   ! grep -qx '    const int PROFILE_STANDARD = 0;' \
      "$ROOT/aidl/com/ayn/controller/ControllerProfileResponse.aidl" ||
   ! grep -qx '    const int PROFILE_FLIPPED_FACE = 1;' \
      "$ROOT/aidl/com/ayn/controller/ControllerProfileResponse.aidl"; then
  echo "error: controller profile AIDL contract is incomplete" >&2
  exit 1
fi

if ! grep -q 'com.ayn.controller.IOdinController/default' \
     "$ROOT/src/rsinputd.cpp" ||
   ! grep -q 'AServiceManager_addService' "$ROOT/src/rsinputd.cpp" ||
   ! grep -q 'persist.sys.ayn.controller.profile' \
     "$ROOT/include/ayn/controller_profile.h"; then
  echo "error: rsinputd controller Binder or persistence wiring is incomplete" >&2
  exit 1
fi

if grep -Eq '/dev/(tty|uinput|rscom)|/sys/|gpio|mcu|WritePosixFile' \
     "$ROOT/include/ayn/controller_profile.h" \
     "$ROOT/include/ayn/controller_profile_adapter.h" \
     "$ROOT/src/controller_profile.cpp" \
     "$ROOT/src/controller_profile_adapter.cpp"; then
  echo "error: controller profile backend must not add hardware control I/O" >&2
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
   ! grep -qx '    FanResponse setMode(int mode);' "$ROOT/aidl/com/ayn/fan/IOdinFan.aidl"; then
  echo "error: private unstable IOdinFan AIDL contract is incomplete" >&2
  exit 1
fi

if grep -Eq 'android\.os\.IBinder|owner' \
     "$ROOT/aidl/com/ayn/fan/IOdinFan.aidl" ||
   grep -Eq 'AIBinder_(link|unlink)ToDeath|OwnerDied' \
     "$ROOT/src/odinfand.cpp" \
     "$ROOT/include/ayn/fan_service.h" \
     "$ROOT/src/fan_service.cpp"; then
  echo "error: fan mode ownership must remain entirely inside odinfand" >&2
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

if [ -e "$ROOT/odinperformanced.rc" ]; then
  echo "error: performance scaffolding must not install an init rc" >&2
  exit 1
fi

performance_module="$(sed -n \
  '/^[[:space:]]*cc_binary[[:space:]]*{/,/^}/p' "$ROOT/Android.bp" |
  sed -n '/name: "odinperformanced"/,/^}/p')"
if ! printf '%s\n' "$performance_module" | grep -qx \
     '    name: "odinperformanced",' ||
   printf '%s\n' "$performance_module" | grep -q 'init_rc' ||
   printf '%s\n' "$performance_module" | grep -Eq \
     '^[[:space:]]*vendor:[[:space:]]*true'; then
  echo "error: odinperformanced must remain a system-only unwired binary" >&2
  exit 1
fi

if ! grep -q 'name: "com.ayn.performance"' "$ROOT/Android.bp" ||
   ! grep -q 'local_include_dir: "aidl"' "$ROOT/Android.bp" ||
   ! grep -q 'unstable: true' "$ROOT/Android.bp" ||
   ! grep -qx 'interface IOdinPerformance {' \
      "$ROOT/aidl/com/ayn/performance/IOdinPerformance.aidl" ||
   ! grep -qx '    PerformanceResponse getStatus();' \
      "$ROOT/aidl/com/ayn/performance/IOdinPerformance.aidl" ||
   ! grep -qx '    PerformanceResponse setMode(int mode);' \
      "$ROOT/aidl/com/ayn/performance/IOdinPerformance.aidl"; then
  echo "error: private unstable performance AIDL contract is incomplete" >&2
  exit 1
fi

if grep -Eq 'String.*path|String\[\]|long\[\]|int\[\]' \
     "$ROOT/aidl/com/ayn/performance/IOdinPerformance.aidl" \
     "$ROOT/aidl/com/ayn/performance/PerformanceResponse.aidl"; then
  echo "error: performance callers must not supply paths or raw node values" >&2
  exit 1
fi

if ! grep -q 'com.ayn.performance.IOdinPerformance/default' \
     "$ROOT/src/odinperformanced.cpp" ||
   ! grep -q 'AServiceManager_addService' "$ROOT/src/odinperformanced.cpp" ||
   ! grep -q 'ro.product.device' "$ROOT/src/odinperformanced.cpp" ||
   ! grep -q 'ro.product.name' "$ROOT/src/odinperformanced.cpp" ||
   ! grep -q 'ro.product.vendor.model' "$ROOT/src/odinperformanced.cpp" ||
   ! grep -q 'WritePosixFile' "$ROOT/src/odinperformanced.cpp" ||
   ! grep -q 'ControlPolicy::StockNormalOnly' \
      "$ROOT/src/odinperformanced.cpp"; then
  echo "error: performance daemon registration or exact identity inputs are incomplete" >&2
  exit 1
fi

if grep -Eq '(^|[^[:alnum:]_])(system|popen|execl?|execv|chmod|setenforce)[[:space:]]*\(|/bin/(sh|bash)|SetProperty|socket\(' \
     "$ROOT/include/ayn/performance_service.h" \
     "$ROOT/include/ayn/performance_adapter.h" \
     "$ROOT/src/performance_service.cpp" \
     "$ROOT/src/performance_adapter.cpp" \
     "$ROOT/src/odinperformanced.cpp"; then
  echo "error: performance service crossed its fixed sysfs/Binder boundary" >&2
  exit 1
fi

if grep -Eq 'O_RDWR' \
     "$ROOT/include/ayn/performance_adapter.h" \
     "$ROOT/src/performance_adapter.cpp" \
     "$ROOT/src/odinperformanced.cpp"; then
  echo "error: performance writer must remain write-only" >&2
  exit 1
fi

if grep -Eq 'persist\.vendor\.debug\.mode|/system/bin/pservice|1228800|2476800' \
     "$ROOT/src/odinperformanced.cpp" \
     "$ROOT/src/performance_adapter.cpp"; then
  echo "error: scaffolding must not embed or activate the stock writer policy" >&2
  exit 1
fi
