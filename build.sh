#!/bin/sh
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"
export PATH="/usr/lib/llvm-18/bin:$PATH"

# The userspace init image must exist and be current -- it's embedded
# directly into the kernel binary (see boot.S's .incbin), not
# discovered as a separate module the way a multiboot-based kernel
# would (this project's own ELF/multiboot path was dropped once this
# one was confirmed working end to end -- Mach-O + GRUB's xnu_kernel64
# command, booted over EFI; see boot.S's own top-of-file comment for
# the full story of what that took).
#
# INIT_MACHO is a plain build-time choice, deliberately not something
# the kernel itself (kernel.c, boot.S) has any opinion about -- see
# include/init_abi.h's own note on why. This project's own reference
# init implementation lives in coreinit/ and is what build_coreinit.sh
# below produces; INIT_MACHO can be pointed at any other Mach-O image
# exposing the same one entry-point convention instead, with no change
# to anything under kernel/.
if [ -z "$INIT_MACHO" ]; then
    "$ROOT/build_coreinit.sh"
    INIT_MACHO="$BUILD/coreinit.macho"
fi

CC=clang-18
TARGET=x86_64-apple-darwin
CFLAGS="-target $TARGET -std=c11 -ffreestanding -fno-stack-protector -fpie -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -I$ROOT/kernel -I$ROOT/include -O1 -g"
"$CC" -target $TARGET -c "$ROOT/kernel/boot.S" \
    -DINIT_MACHO_PATH='"'"$INIT_MACHO"'"' \
    -o "$BUILD/boot.o"
"$CC" $CFLAGS -c "$ROOT/kernel/kernel.c" -o "$BUILD/kernel.o"
"$CC" $CFLAGS -c "$ROOT/kernel/serial.c" -o "$BUILD/serial.o"
"$CC" $CFLAGS -c "$ROOT/kernel/cap.c" -o "$BUILD/cap.o"
"$CC" $CFLAGS -c "$ROOT/kernel/capnp_validate.c" -o "$BUILD/capnp_validate.o"
"$CC" $CFLAGS -c "$ROOT/kernel/macho.c" -o "$BUILD/macho.o"

# -pagezero_size 0x1000000 (16MB, matching include/smp_layout.h's own
# KERNEL_LOAD_BASE -- kernel.c's boot_info.kernel_load_base needs the
# two to agree, or SMP bring-up's copy-to-an-AP's-region delta would be
# silently wrong): this kernel used to link (and load, confirmed at
# runtime the load base tracks link-time vmaddr exactly) at physical/
# virtual address 0 -- which directly collided with firmware-owned low
# memory. Confirmed the hard way with GDB: OVMF places the ACPI RSDP at
# a firmware-chosen address (observed as low as 0x2469b, as high as
# 0x777e000 depending on VM RAM size -- see coreinit/acpi.c and boot.S
# for the full story) that a vmaddr-0 kernel could easily collide with,
# and once did, silently overwriting it before init's own acpi_discover
# ever got a chance to find it. A real __PAGEZERO segment of this size
# pushes __TEXT's own vmaddr up to 0x1000000 (16MB), safely clear of
# every firmware-owned low-memory structure actually observed so far.
ld64.lld -arch x86_64 -platform_version macos 10.12 10.12 \
    -e _start -pagezero_size 0x1000000 \
    -o "$BUILD/saski_raw.macho" \
    "$BUILD/boot.o" "$BUILD/kernel.o" "$BUILD/serial.o" \
    "$BUILD/cap.o" "$BUILD/capnp_validate.o" "$BUILD/macho.o"

python3 "$ROOT/tools/macho_to_xnu.py" "$BUILD/saski_raw.macho" "$BUILD/saski.macho"

echo "built $BUILD/saski.macho"
file "$BUILD/saski.macho"
