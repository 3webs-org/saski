#!/bin/sh
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD"
LLVM_BIN="${LLVM_BIN:-/usr/lib/llvm-18/bin}"
export PATH="$LLVM_BIN:$PATH"

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
# include/init_abi.h's own note on why. The reference init implementation
# is no longer in this repo at all -- it lives in a separate repository,
# located here via COREINIT_ROOT (default ../otos-coreinit), and is built
# by that repo's own build_coreinit.sh, whose output is
# $COREINIT_ROOT/build/coreinit.macho; INIT_MACHO can still be pointed at
# any other Mach-O image exposing the same one entry-point convention
# instead, with no change to anything under kernel/.
if [ -z "$INIT_MACHO" ]; then
    COREINIT_ROOT="${COREINIT_ROOT:-$ROOT/../otos-coreinit}"
    # This message is the first thing a newcomer hits when the sibling repo
    # sits somewhere other than the default or is absent, so it names the
    # variable and the path actually tried; it is deliberately symmetric with
    # otos-coreinit's own SASKI_ROOT error, so the cross-repo handshake reads
    # as one convention rather than two dialects.
    if [ ! -f "$COREINIT_ROOT/build_coreinit.sh" ]; then
        echo "build.sh: COREINIT_ROOT not found -- no build_coreinit.sh at $COREINIT_ROOT/build_coreinit.sh. Set COREINIT_ROOT to the path of the otos-coreinit repository (default: $ROOT/../otos-coreinit)." >&2
        exit 1
    fi
    sh "$COREINIT_ROOT/build_coreinit.sh"
    INIT_MACHO="$COREINIT_ROOT/build/coreinit.macho"
fi

# Two INDEPENDENT build axes. Do not couple them.
#
# ARCH selects the compile list and the boot path: which architecture-specific
# sources are built and how the kernel is entered. Defaults to x86_64, and the
# x86_64 path below is untouched by anything in this block -- adding an arch
# must never perturb an existing one, which is why the default branch is the
# old branch.
ARCH="${ARCH:-x86_64}"
if [ "$ARCH" = "aarch64" ]; then
    # .incbin must see exactly the bytes the judge later checks. A sibling
    # init build can finish while this build is compiling; snapshot it once
    # rather than embedding a moving file and judging a different image.
    cp "$INIT_MACHO" "$BUILD/embedded-init.macho"
    INIT_MACHO="$BUILD/embedded-init.macho"
fi

# EMBEDDED selects whether data a platform would otherwise be HANDED at boot is
# instead compiled into the image. This is orthogonal to ARCH, deliberately:
# an aarch64 image can be built to trust a firmware-supplied device tree, or to
# carry its own and trust nothing; the same choice exists on any platform with
# an equivalent input. Deciding it by ARCH would be wrong in exactly the case
# that matters -- an aarch64 target whose firmware does hand over a good DTB and
# which therefore has no reason to spend flash on a copy.
#
# When EMBEDDED=1, DTS_SOURCE explicitly chooses the board's source tree;
# dtc compiles it and arch/dt_blob.S .incbin's the result. The QEMU virt tree
# lives under tests/ and is never selected implicitly for a real build.
# When EMBEDDED=0 no blob is built or linked.
#
# Default 0, for every arch. That is what keeps this change invisible to the
# existing build: with no flags set, the compile list and the link inputs are
# exactly what they were.
EMBEDDED="${EMBEDDED:-0}"

# The aarch64 boot path is arch/boot_aarch64.S and is NOT in the x86_64 link.
# It captures the DTB address from x0, validates the blob's magic before
# trusting its declared size, and calls kernel_boot_aarch64, which does not
# exist yet -- so this target assembles but does not link. Saying that here
# rather than in a commit message nobody will read: ARCH=aarch64 is a
# work-in-progress path, and selecting it will fail at link with an undefined
# kernel_boot_aarch64. The x86_64 default is unaffected and remains the only
# path that boots.

CC="${CC:-clang-18}"
LD64="${LD64:-ld64.lld}"
# The aarch64 kernel's single link address, as an offset from QEMU virt's
# RAM base (0x40000000). 16 MiB in: clear of the 1 MiB firmware-supplied
# device tree AAVMF places at the bottom of RAM (GRUB's xnu loader allocates
# the kernel's own range from EFI, so it must be free), and clear of the
# reference init's declared vmaddr (otos-coreinit, 0x40200000). Every
# aarch64 boot path -- QEMU -kernel (via the Image header's text_offset),
# GRUB xnu_kernel64 (via the Mach-O vmaddr), the EFI stub (via its copy
# destination) -- derives from this one value.
AARCH64_RAM_BASE=0x40000000
AARCH64_TEXT_OFFSET=0x1000000
AARCH64_LOAD_BASE=$(printf '0x%x' $((AARCH64_RAM_BASE + AARCH64_TEXT_OFFSET)))
if [ "$ARCH" = "aarch64" ]; then
    TARGET=aarch64-none-elf
else
    TARGET=x86_64-apple-darwin
fi
# Per-architecture flags. The x86_64 flags are the ones this build has always
# used; the aarch64 flags drop the x86-specific ones (-mno-red-zone,
# -mno-sse, -mno-mmx are x86 concepts with no aarch64 meaning) and add
# -mgeneral-regs-only, because a freestanding kernel must not depend on the
# floating-point unit being enabled: firmware may leave it off, and touching
# an FP register with it off is an exception with no handler.
if [ "$ARCH" = "aarch64" ]; then
    # APs run a raw copy at an assigned address. On this AArch64 ELF target,
    # -fno-pic uses PC-relative ADRP/ADD references to local symbols.
    # -fpie instead emits GOT slots containing absolute BSP addresses; a raw
    # AP copy has no dyld to rebase those slots. The BSP assembly entry can
    # still use absolute literals because APs enter kernel_boot_ap directly.
    CFLAGS="-target $TARGET -std=c11 -ffreestanding -fno-stack-protector -fno-pic -mgeneral-regs-only -Wall -Wextra -I$ROOT/kernel -I$ROOT/include -O1 -g"
else
    CFLAGS="-target $TARGET -std=c11 -ffreestanding -fno-stack-protector -fpie -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra -I$ROOT/kernel -I$ROOT/include -O1 -g"
fi
if [ "$ARCH" = "aarch64" ]; then
    "$CC" -target $TARGET -c "$ROOT/arch/boot_aarch64.S" \
        -DKERNEL_TEXT_OFFSET=$AARCH64_TEXT_OFFSET \
        -DINIT_MACHO_PATH='"'"$INIT_MACHO"'"' \
        -o "$BUILD/boot.o"
else
"$CC" -target $TARGET -c "$ROOT/kernel/boot.S" \
    -DINIT_MACHO_PATH='"'"$INIT_MACHO"'"' \
    -o "$BUILD/boot.o"
fi
"$CC" $CFLAGS -c "$ROOT/kernel/kernel.c" -o "$BUILD/kernel.o"
# The serial transport is per-architecture: serial.c is the arch-neutral
# formatter and calls serial_putc, which the arch's own backend provides. The
# backend file is selected here rather than by #ifdef inside a shared file,
# because a file-level split means adding an architecture is one line here and
# cannot affect the other arch's compile at all. serial_x86_64.c carries its
# own #error guard in case the selection and the file ever disagree."
if [ "$ARCH" = "aarch64" ]; then
    "$CC" $CFLAGS -c "$ROOT/kernel/serial_aarch64.c" -o "$BUILD/serial.o"
else
    "$CC" $CFLAGS -c "$ROOT/kernel/serial_x86_64.c" -o "$BUILD/serial.o"
fi
# serial.c is the arch-neutral formatter (kprintf); the transport backend
# selected above provides serial_init/serial_putc. Both are needed: the
# formatter calls into the transport, so both must be in the link.
"$CC" $CFLAGS -c "$ROOT/kernel/serial.c" -o "$BUILD/serial_fmt.o"
"$CC" $CFLAGS -c "$ROOT/kernel/cap.c" -o "$BUILD/cap.o"
"$CC" $CFLAGS -c "$ROOT/kernel/capnp_validate.c" -o "$BUILD/capnp_validate.o"
"$CC" $CFLAGS -c "$ROOT/kernel/macho.c" -o "$BUILD/macho.o"

if [ "$ARCH" = "aarch64" ]; then
    "$CC" $CFLAGS -c "$ROOT/kernel/kernel_boot_aarch64.c" -o "$BUILD/kernel_boot_aarch64.o"
    # The aarch64 boot path reads the device tree, so lib/fdt.c joins the
    # link for this arch only. The x86_64 compile list is unchanged: that
    # platform is described by ACPI and has no caller for the FDT reader.
    "$CC" $CFLAGS -c "$ROOT/lib/fdt.c" -o "$BUILD/fdt.o"
fi

# -pagezero_size 0x1000000 (16MB, matching include/smp_layout.h's own
# KERNEL_LOAD_BASE -- kernel.c's boot_info.kernel_load_base needs the
# two to agree, or SMP bring-up's copy-to-an-AP's-region delta would be
# silently wrong): this kernel used to link (and load, confirmed at
# runtime the load base tracks link-time vmaddr exactly) at physical/
# virtual address 0 -- which directly collided with firmware-owned low
# memory. Confirmed the hard way with GDB: OVMF places the ACPI RSDP at
# a firmware-chosen address (observed as low as 0x2469b, as high as
# 0x777e000 depending on VM RAM size -- see the otos-coreinit repository's acpi.c and this project's own boot.S
# for the full story) that a vmaddr-0 kernel could easily collide with,
# and once did, silently overwriting it before init's own acpi_discover
# ever got a chance to find it. A real __PAGEZERO segment of this size
# pushes __TEXT's own vmaddr up to 0x1000000 (16MB), safely clear of
# every firmware-owned low-memory structure actually observed so far.
# The device tree, when this build is asked to carry one. Gated on EMBEDDED
# alone -- not on ARCH -- because whether a kernel trusts a firmware-supplied
# tree or carries its own is a deployment decision, not a property of the
# instruction set. See the axes block at the top.
DROPIN=""
if [ "$EMBEDDED" = "1" ]; then
    [ -n "${DTS_SOURCE:-}" ] || { echo "build.sh: EMBEDDED=1 requires DTS_SOURCE (tests/qemu-virt.dts is a test fixture)" >&2; exit 1; }
    case "$DTS_SOURCE" in
        /*) ;;
        *) DTS_SOURCE="$ROOT/$DTS_SOURCE" ;;
    esac
    [ -f "$DTS_SOURCE" ] || { echo "build.sh: no device-tree source at $DTS_SOURCE" >&2; exit 1; }
    DTC="${DTC:-dtc}"
    "$DTC" -I dts -O dtb -o "$BUILD/saski.dtb" "$DTS_SOURCE" \
        || { echo "build.sh: dtc failed on $DTS_SOURCE" >&2; exit 1; }
    "$CC" -target $TARGET -c "$ROOT/arch/dt_blob.S" \
        -DDTB_PATH='"'"$BUILD/saski.dtb"'"' \
        -o "$BUILD/dt_blob.o"
    DROPIN="$BUILD/dt_blob.o"
    echo "embedded device tree: $BUILD/saski.dtb ($(wc -c < "$BUILD/saski.dtb" | tr -d ' ') bytes)"
fi

if [ "$ARCH" = "aarch64" ]; then
    # The aarch64 link is a plain ELF link with no Mach-O-specific flags.
    # ld64.lld handles ELF for aarch64 as well as Mach-O for x86_64.
    # ld64.lld is the Mach-O linker; it does not do ELF. The ELF linker is
    # ld.lld from the same lld package, which is what a freestanding aarch64
    # kernel needs. Resolved the same way as LD64 so one env var set covers
    # both.
    LDELFD="${LDELFD:-$(dirname "$LD64")/ld.lld}"
    [ -x "$LDELFD" ] || { echo "build.sh: no ld.lld next to $LD64; set LDELFD" >&2; exit 1; }
    # AArch64 ELF's default 64 KiB segment alignment puts 128 KiB of
    # zero-filled gaps between .text, .rodata and .data in the flat Image.
    # QEMU virt uses 4 KiB pages; 4 KiB segment alignment keeps the same
    # load addresses while eliminating 120 KiB of those file gaps.
    "$LDELFD" -e _start -Ttext $AARCH64_LOAD_BASE -z max-page-size=4096 \
        -o "$BUILD/saski_raw.macho" \
        "$BUILD/boot.o" "$BUILD/kernel.o" "$BUILD/serial.o" "$BUILD/serial_fmt.o" \
        "$BUILD/cap.o" "$BUILD/capnp_validate.o" "$BUILD/macho.o" \
        "$BUILD/kernel_boot_aarch64.o" "$BUILD/fdt.o" $DROPIN
    # macho_to_xnu.py is an x86_64 Mach-O converter; an aarch64 image does not
    # go through it. The raw ELF is the artifact for -kernel boots; the EFI
    # stub (below) is the artifact for GRUB/AAVMF boots.
    cp "$BUILD/saski_raw.macho" "$BUILD/saski.macho"
    # The flat binary + EFI stub: GRUB's xnu loader does not exist for
    # arm64 (grub-efi-arm64-bin ships no xnu.mod), so the aarch64 path
    # cannot reuse the x86_64 Mach-O/GRUB convention. Instead the flat
    # image is wrapped in a minimal EFI application that GRUB
    # chainloads; the stub copies the kernel to its link address, turns
    # the MMU off, and jumps to it with the DTB pointer in x0 -- the
    # standard arm64 boot protocol. tools/qemu-boot-aarch64.sh is the
    # judge for this path.
    OBJCOPY="${OBJCOPY:-$(command -v llvm-objcopy || echo /nix/store/ak5yjnphlhn8lx51vinhx1wiyjxg1fsp-llvm-21.1.8/bin/llvm-objcopy)}"
    "$OBJCOPY" -O binary "$BUILD/saski.macho" "$BUILD/saski.bin"
    KERNEL_DST=$AARCH64_LOAD_BASE python3 "$ROOT/tools/mkstub.py" "$BUILD/saski.bin" "$BUILD/saski.efi"
    # The same image as a Mach-O for GRUB's xnu_kernel64 -- the boot
    # convention the x86_64 path uses, now available on arm64 through
    # otos-coreinit's arm64 build of GRUB's own xnu loader. saski.macho
    # stays the ELF (for -kernel); saski-xnu.macho is the xnu artifact.
    python3 "$ROOT/tools/macho_arm64.py" "$BUILD/saski.bin" \
        "$AARCH64_LOAD_BASE" "$BUILD/saski-xnu.macho"
else
"$LD64" -arch x86_64 -platform_version macos 10.12 10.12 \
    -e _start -pagezero_size 0x1000000 \
    -o "$BUILD/saski_raw.macho" \
    "$BUILD/boot.o" "$BUILD/kernel.o" "$BUILD/serial.o" "$BUILD/serial_fmt.o" \
    "$BUILD/cap.o" "$BUILD/capnp_validate.o" "$BUILD/macho.o" $DROPIN
fi

if [ "$ARCH" != "aarch64" ]; then
    python3 "$ROOT/tools/macho_to_xnu.py" "$BUILD/saski_raw.macho" "$BUILD/saski.macho"
fi

echo "built $BUILD/saski.macho"
file "$BUILD/saski.macho"
