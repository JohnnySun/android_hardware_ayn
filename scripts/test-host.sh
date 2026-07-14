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
  "$ROOT/src/rsinput_parser.cpp" \
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

if ! grep -qx 'on boot && property:ro.product.device=odin2_mini' "$ROOT/rsinputd.rc" ||
   ! grep -qx '    start rsinputd' "$ROOT/rsinputd.rc"; then
  echo "error: rsinputd must start once at boot before Setup Wizard on Odin2 Mini" >&2
  exit 1
fi

if ! grep -qx 'service odinfand /system/bin/odinfand' "$ROOT/odinfand.rc" ||
   ! grep -qx '    disabled' "$ROOT/odinfand.rc" ||
   grep -qx '    oneshot' "$ROOT/odinfand.rc"; then
  echo "error: odinfand must be a persistent disabled system service" >&2
  exit 1
fi

if grep -Eq '^[[:space:]]*on |^[[:space:]]*start odinfand' "$ROOT/odinfand.rc"; then
  echo "error: odinfand must not start before product and SELinux wiring are proven" >&2
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
