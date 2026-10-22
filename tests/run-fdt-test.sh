#!/bin/sh
# SPDX-License-Identifier: 0BSD
#
# Host test for the device-tree reader.
#
# Compiles the same lib/fdt.c the kernel links, against tests/fdt_test.dts
# compiled to a blob, and runs assertions natively. This exists because a
# compile is not a test: the reader's failure mode is a wrong device address
# rather than a build error, and the only way to catch that is to assert the
# translated values.
#
# It is a HOST test, not a kernel test. It proves the reader; it says nothing
# about whether a kernel boots. tools/qemu-boot.sh is the judge for that.
#
# Everything is overridable because the toolchain here is not the toolchain on
# another machine: CC in particular is Apple clang on this host, since the
# stored clang-18 has no macOS SDK search path.

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/fdt-test"
# CC and the SDK search path are resolved together: a bare clang binary does
# not know where the system headers live on this host, so locating the compiler
# without also locating the SDK produces "stdio.h file not found" -- a message
# that reads like a missing library rather than a missing search path.
if [ -z "$CC" ]; then
    if command -v xcrun >/dev/null 2>&1; then
        CC=$(xcrun -f clang)
        SDKROOT="${SDKROOT:-$(xcrun --show-sdk-path 2>/dev/null || true)}"
        export SDKROOT
    else
        CC=cc
    fi
fi
DTC="${DTC:-}"

mkdir -p "$BUILD"

# Resolve dtc three ways, and say which failed rather than dying on a bare
# "command not found" that reads like a broken build.
if [ -z "$DTC" ]; then
    DTC=$(command -v dtc 2>/dev/null || true)
fi
if [ -z "$DTC" ]; then
    DTC=$(ls -d /nix/store/*-dtc-*/bin/dtc 2>/dev/null | head -1 || true)
fi
[ -n "$DTC" ] || { echo "run-fdt-test.sh: no dtc found; set DTC" >&2; exit 1; }

echo "cc  = $CC"
echo "dtc = $DTC"
echo
echo "compiling the test tree"
"$DTC" -I dts -O dtb -o "$BUILD/fdt_test.dtb" "$ROOT/tests/fdt_test.dts"

echo "building the test"
# -Wall -Wextra on the test itself, and the library is compiled with the same
# strictness the kernel uses, so a warning in the reader is a failure here.
"$CC" -std=c11 -O1 -g -Wall -Wextra -Werror \
    -I "$ROOT/include" \
    -o "$BUILD/fdt_test" \
    "$ROOT/tests/fdt_test.c" \
    "$ROOT/lib/fdt.c"

echo "running"
echo
"$BUILD/fdt_test" "$BUILD/fdt_test.dtb"
