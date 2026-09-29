#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="${1:?usage: run-usb-open-retry.sh LIBFPRINT_LIBRARY_DIRECTORY}"
TEST_BIN="$(mktemp --tmpdir egis057e-open-retry.XXXXXX)"
trap 'rm -f "$TEST_BIN"' EXIT

cc "$SCRIPT_DIR/usb-open-retry.c" -o "$TEST_BIN" \
  $(pkg-config --cflags --libs libfprint-2)

LD_LIBRARY_PATH="$LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$TEST_BIN"
