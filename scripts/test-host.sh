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

if ! grep -qx '    disabled' "$ROOT/rsinputd.rc" ||
   ! grep -qx '    oneshot' "$ROOT/rsinputd.rc"; then
  echo "error: rsinputd must remain disabled and oneshot" >&2
  exit 1
fi

if ! grep -qx 'service rsinputd /system/bin/rsinputd' "$ROOT/rsinputd.rc" ||
   grep -Eq '^[[:space:]]*vendor:[[:space:]]*true' "$ROOT/Android.bp"; then
  echo "error: system-only bring-up must install rsinputd in system" >&2
  exit 1
fi

if ! grep -qx 'on late-init && property:ro.product.device=odin2_mini' "$ROOT/rsinputd.rc" ||
   ! grep -qx '    start rsinputd' "$ROOT/rsinputd.rc"; then
  echo "error: rsinputd must start once before Setup Wizard on Odin2 Mini" >&2
  exit 1
fi
