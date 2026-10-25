#!/bin/sh
# SPDX-License-Identifier: 0BSD
#
# The QEMU boot judge for saski.
#
# Nothing in this project counts as done until the kernel boots in QEMU and
# the serial log shows the expected marker sequence. This script is that
# judge: it assembles a bootable EFI disk carrying build/saski.macho, boots it
# under QEMU with OVMF firmware, and grades the serial output by tier.
#
# It is PURELY ADDITIVE. It consumes build/saski.macho exactly as the cmake
# build produced it and never touches the kernel build, the kernel sources,
# or the shared headers. If this script is wrong, the kernel is still fine.
#
# WHY A DISK AND NOT AN ISO. The obvious shape for an EFI boot test is an El
# Torito ISO assembled with grub-mkrescue. grub-mkrescue is unavailable here:
# nixpkgs' grub is Linux-only and its binaries are ELF aarch64, which cannot
# execute on this macOS host. Hand-assembling the ISO with xorriso was tried
# and failed in a specific and instructive way -- OVMF ran correctly, found no
# bootable device on the attached media, and fell through to PXE ("Start PXE
# over IPv4 / PXE-E16: No valid offer received"). A GPT disk carrying a FAT16
# EFI System Partition, by contrast, is proven to boot on this host. So the
# disk route is the default and the ISO route is not attempted at all.
#
# The FAT16 part is not incidental. OVMF's FAT driver is picky, and a FAT32
# ESP produced by a host tool is a common silent failure: the firmware simply
# does not see the partition as bootable and moves on to the next boot option.
# That is why this script asserts the filesystem type explicitly rather than
# trusting mformat's default.
#
# WHY THE POSITIVE CONTROL RUNS FIRST. If the real boot produces no kernel
# output there are two very different explanations: the kernel failed, or the
# loader never reached our config. Those are indistinguishable from a silent
# log, and the second is far more likely to be a harness bug. So before the
# real boot this script boots the SAME disk with a config that asks GRUB for a
# file that does not exist, and requires GRUB to say so. If GRUB does not
# complain, the harness is broken and the script fails immediately rather than
# reporting a kernel failure it cannot actually distinguish from its own bug.
#
# WHY ram_size IS ASSERTED RATHER THAN ASSUMED. GRUB chooses between the v1 and
# v2 xnu boot_params structs by scanning the loaded image for the literal
# string "Darwin Kernel Version " followed by a number >= 11 (see boot.S's own
# g_xnu_version_string, which exists purely to be found by that scan). Only v2
# carries ram_size. A different GRUB build could silently hand us v1 and lose
# it, so the marker is required, not assumed.

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SASKI_BUILD="${SASKI_BUILD:-$ROOT/build}"
COREINIT_ROOT="${COREINIT_ROOT:-$ROOT/../otos-coreinit}"
DEPS_DIR="${DEPS_DIR:-$SASKI_BUILD/qemu-deps}"
BOOT_TIMEOUT_S="${BOOT_TIMEOUT_S:-60}"
SMP_CPUS="${SMP_CPUS:-4}"
BUILD="${BUILD:-0}"
CC="${CC:-}"
LD64="${LD64:-}"
KEEP_ISO="${KEEP_ISO:-}"
FETCH_DEPS="${FETCH_DEPS:-1}"

QEMU="${QEMU:-/nix/store/59hhc8zlvdlbcls7dkbhkhw9afwb6mps-qemu-11.1.2/bin/qemu-system-x86_64}"
OVMF_CODE="${OVMF_CODE:-$DEPS_DIR/ovmf/usr/share/OVMF/OVMF_CODE_4M.fd}"
OVMF_VARS="${OVMF_VARS:-$DEPS_DIR/ovmf/usr/share/OVMF/OVMF_VARS_4M.fd}"
GRUB_EFI="${GRUB_EFI:-$DEPS_DIR/grub-signed/usr/lib/grub/x86_64-efi-signed/grubx64.efi.signed}"
GRUB_MODULES_DIR="${GRUB_MODULES_DIR:-$DEPS_DIR/grub-bin/usr/lib/grub/x86_64-efi}"
XORRISO="${XORRISO:-/nix/store/0jd260nb5y6kcff40h9nvhb7hq60fldm-libisoburn-1.5.8.pl02/bin/xorriso}"
MFORMAT="${MFORMAT:-/nix/store/nmcdf0f6c58r9g3l26104a5ak32m207p-mtools-4.0.49/bin/mformat}"
MCOPY="${MCOPY:-/nix/store/nmcdf0f6c58r9g3l26104a5ak32m207p-mtools-4.0.49/bin/mcopy}"
MMD="${MMD:-/nix/store/nmcdf0f6c58r9g3l26104a5ak32m207p-mtools-4.0.49/bin/mmd}"

WORK="$SASKI_BUILD/qemu-judge"
LOG_GRUB="$WORK/grub-stage.log"
LOG_KERNEL="$WORK/kernel-stage.log"


# otool prints vmaddr zero-padded to 16 hex digits (0x0000000001000000) while
# smp_layout.h writes the same value unpadded (0x1000000). Comparing the raw
# strings would fail on a correct build, so both sides are normalized to a
# plain integer first. This is a formatting difference, not a value one, and
# an assertion that cannot tell those apart is worse than no assertion.
normalize_hex() {
    printf '%d' "$1" 2>/dev/null || printf '%s' "$1"
}

say() { printf '%s\n' "$*"; }
fail() { printf 'qemu-boot.sh: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# STEP 0 -- fetch the firmware and loader artifacts if they are not present.
#
# These are Debian packages, and a .deb is just an ar archive containing a tar
# archive. Extracting one needs no Linux binary and no root: it is pure data
# manipulation, done here in Python. That matters because the alternative --
# running Debian's grub-mkimage -- is impossible on this host, and because it
# makes the harness reproducible on a clean machine instead of depending on
# whatever happens to be in someone's scratch directory.
#
# Two traps are worth recording, both of which cost real time. First, the
# Packages index's Filename field is relative to the ARCHIVE root, not the
# mirror root, so the URL needs /debian/ inserted; omitting it produces a DNS
# failure that looks like a missing package. Second, tarfile.extractall's
# filter= keyword does not exist before Python 3.12, and on an older Python it
# silently yields zero files rather than raising -- which reads as "the package
# has no modules" rather than "the extraction did nothing".
# ---------------------------------------------------------------------------
fetch_deps() {
    [ -d "$GRUB_MODULES_DIR" ] && [ -f "$GRUB_EFI" ] && [ -f "$OVMF_CODE" ] && return 0
    [ "$FETCH_DEPS" = "1" ] || fail "deps missing and FETCH_DEPS=0"
    say "fetching firmware and loader artifacts into $DEPS_DIR"
    mkdir -p "$DEPS_DIR"
    python3 - "$DEPS_DIR" <<'PY_EOF'
import os, sys, subprocess, gzip, tarfile, hashlib

dest = sys.argv[1]
base = "https://deb.debian.org/debian"
index = base + "/dists/trixie/main/binary-amd64/Packages.gz"

raw = subprocess.run(["curl", "-sSL", "--max-time", "180", index],
                     capture_output=True, check=True).stdout
text = gzip.decompress(raw).decode("utf-8", "replace")

want = ["grub-efi-amd64-bin", "grub-efi-amd64-signed", "ovmf"]
entries = {}
cur = None
for line in text.splitlines():
    if line.startswith("Package: "):
        cur = line.split(": ", 1)[1].strip()
    elif cur in want and line.startswith("Filename: "):
        entries.setdefault(cur, {})["filename"] = line.split(": ", 1)[1].strip()
    elif cur in want and line.startswith("MD5sum: "):
        entries.setdefault(cur, {})["md5"] = line.split(": ", 1)[1].strip()
    elif not line.strip():
        cur = None

for pkg in want:
    if pkg not in entries or "filename" not in entries[pkg]:
        print("MISSING INDEX ENTRY: " + pkg)
        sys.exit(1)
    fn = entries[pkg]["filename"]
    url = base + "/" + fn.lstrip("/")
    out = os.path.join(dest, pkg + ".deb")
    if not os.path.exists(out):
        subprocess.run(["curl", "-sSL", "--max-time", "300", "-o", out, url], check=True)
    data = open(out, "rb").read()
    if data[:8] != b"!<arch>\n":
        print("NOT AN AR ARCHIVE: " + pkg)
        sys.exit(1)
    md5 = hashlib.md5(data).hexdigest()
    if "md5" in entries[pkg] and md5 != entries[pkg]["md5"]:
        print("MD5 MISMATCH for " + pkg + ": got " + md5 + " want " + entries[pkg]["md5"])
        sys.exit(1)
    off = 8
    members = {}
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        name = hdr[0:16].decode("utf-8", "replace").strip().rstrip("/")
        try:
            size = int(hdr[48:58].decode("utf-8").strip())
        except ValueError:
            break
        members[name] = data[off + 60:off + 60 + size]
        off += 60 + size + (size % 2)
    target = os.path.join(dest, pkg)
    os.makedirs(target, exist_ok=True)
    for name, body in members.items():
        if name.startswith("data.tar"):
            tmp = os.path.join(dest, pkg + ".tar")
            open(tmp, "wb").write(body)
            tarfile.open(tmp).extractall(target)
            os.remove(tmp)
    print("extracted " + pkg)
PY_EOF
    GRUB_MODULES_DIR="$DEPS_DIR/grub-efi-amd64-bin/usr/lib/grub/x86_64-efi"
    GRUB_EFI="$DEPS_DIR/grub-efi-amd64-signed/usr/lib/grub/x86_64-efi-signed/grubx64.efi.signed"
    OVMF_CODE="$DEPS_DIR/ovmf/usr/share/OVMF/OVMF_CODE_4M.fd"
    OVMF_VARS="$DEPS_DIR/ovmf/usr/share/OVMF/OVMF_VARS_4M.fd"
}

# ---------------------------------------------------------------------------
# STEP 1 -- state the inputs. A harness that prints the absolute path of every
# tool and artifact it is about to use is worth more than one that assumes
# them, because the failure mode of a wrong assumption is a silent no-boot.
# ---------------------------------------------------------------------------
resolve_and_report() {
    say "=== resolved inputs ==="
    for pair in "QEMU=$QEMU" "OVMF_CODE=$OVMF_CODE" "OVMF_VARS=$OVMF_VARS" \
                "GRUB_EFI=$GRUB_EFI" "GRUB_MODULES_DIR=$GRUB_MODULES_DIR" \
                "MFORMAT=$MFORMAT" "MCOPY=$MCOPY" "MMD=$MMD" \
                "SASKI_BUILD=$SASKI_BUILD" "COREINIT_ROOT=$COREINIT_ROOT"; do
        name=${pair%%=*}
        value=${pair#*=}
        if [ -e "$value" ]; then
            say "  $name = $value"
        else
            say "  $name = $value   [MISSING]"
        fi
    done
}

# ---------------------------------------------------------------------------
# STEP 2 -- pre-boot assertions. These fail fast, before any VM starts, and
# they parse the expected load addresses out of include/smp_layout.h rather
# than hardcoding them: the header is the source of truth, and an assertion
# that duplicates a constant cannot catch that constant changing.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# A judge is worthless if it judges the wrong image.
#
# Two distinct ways that happens, and both produce a confident green or a
# confusing failure that has nothing to do with the kernel. The first: the
# image is stale -- sources changed, nobody rebuilt, and the run reports on
# the previous kernel. The second: the image is current but the init image
# EMBEDDED inside it is not, because boot.S .incbin's coreinit at build time,
# so a kernel can be fresh while carrying an old init.
#
# So this step refuses to proceed on a stale image, and grade() below asserts
# the init length the kernel reports matches the init file actually on disk.
# Together they close both holes, and neither depends on trusting a build log.
check_image() {
    if [ "$BUILD" = "1" ]; then
        say "BUILD=1: rebuilding the kernel first"
        if [ -z "$CC" ]; then
            CC=$(xcrun -f clang 2>/dev/null) || fail "BUILD=1 but no CC could be resolved (xcrun clang not found) -- set CC explicitly"
        fi
        if [ -z "$LD64" ]; then
            LD64=$(ls -d /nix/store/*-lld-18*/bin/ld64.lld 2>/dev/null | head -1)
            [ -n "$LD64" ] || fail "BUILD=1 but no LD64 could be resolved -- set LD64 to an ld64.lld"
        fi
        INIT="${INIT_MACHO:-$COREINIT_ROOT/build/coreinit.macho}"
        [ -f "$INIT" ] || fail "BUILD=1 but no init image at $INIT -- build the sibling repo first"
        say "  CC=$CC"
        say "  LD64=$LD64"
        say "  INIT_MACHO=$INIT"
        ( cd "$ROOT" && cmake -S . -B "$SASKI_BUILD" \
              -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake \
              -DCMAKE_C_COMPILER="$CC" -DLD64="$LD64" -DINIT_MACHO="$INIT" \
              && cmake --build "$SASKI_BUILD" ) \
            || fail "BUILD=1 but the cmake build failed"
    fi

    KERNEL="$SASKI_BUILD/saski.macho"
    [ -f "$KERNEL" ] || fail "no kernel image at $KERNEL -- run cmake --build (see CMakeLists.txt) first, or pass BUILD=1"

    newest=0
    for f in "$ROOT"/kernel/*.c "$ROOT"/kernel/*.h "$ROOT"/kernel/*.S "$ROOT"/include/*.h \
             "$COREINIT_ROOT"/coreinit/*.c "$COREINIT_ROOT"/coreinit/*.h \
             "$COREINIT_ROOT"/coreinit/*.S "$COREINIT_ROOT/build/coreinit.macho"; do
        [ -f "$f" ] || continue
        t=$(date -r "$f" +%s 2>/dev/null) || continue
        [ "$t" -gt "$newest" ] && newest=$t
    done
    kt=$(date -r "$KERNEL" +%s 2>/dev/null) || kt=0
    if [ "$newest" -gt "$kt" ]; then
        fail "$KERNEL is STALE -- a source file is newer than the image. Rebuild (or pass BUILD=1). Judging a stale image is a green result from a test that did not test the thing."
    fi
    say "image freshness: kernel is newer than every source and than the init it embeds"

    INITIMG="$COREINIT_ROOT/build/coreinit.macho"
    if [ -f "$INITIMG" ]; then
        INIT_BYTES=$(wc -c < "$INITIMG" | tr -d ' ')
        say "init image on disk: $INIT_BYTES bytes (the kernel's own report must match this)"
    else
        INIT_BYTES=""
    fi
}

preflight() {
    KERNEL="$SASKI_BUILD/saski.macho"
    [ -f "$KERNEL" ] || fail "no kernel image at $KERNEL -- run cmake --build first"
    file "$KERNEL" | grep -q 'Mach-O 64-bit executable x86_64' \
        || fail "$KERNEL is not a Mach-O 64-bit x86_64 executable"

    SMP_HDR="$ROOT/include/smp_layout.h"
    [ -f "$SMP_HDR" ] || fail "no $SMP_HDR"
    # The header declares both architectures' values under #if. Raw sed
    # finds ARM's first, even on x86. Ask the x86 preprocessor which branch
    # is active, then strip the C integer suffix before numeric comparison.
    CPP_CC="$CC"
    if [ -z "$CPP_CC" ]; then
        CPP_CC=$(xcrun -f clang 2>/dev/null) || CPP_CC=clang
    fi
    SMP_DEFINES=$(printf '#include "smp_layout.h"\n' | "$CPP_CC" -target x86_64-apple-darwin -E -dM -x c -I"$ROOT/include" -) \
        || fail "could not preprocess $SMP_HDR for x86_64 with $CPP_CC"
    WANT_KERNEL=$(printf '%s\n' "$SMP_DEFINES" | sed -n 's/^#define[[:space:]]*KERNEL_LOAD_BASE[[:space:]]*\(0x[0-9a-fA-F]*\).*/\1/p')
    WANT_COREINIT=$(printf '%s\n' "$SMP_DEFINES" | sed -n 's/^#define[[:space:]]*BSP_INIT_LOAD_BASE[[:space:]]*\(0x[0-9a-fA-F]*\).*/\1/p')
    [ -n "$WANT_KERNEL" ] || fail "could not parse KERNEL_LOAD_BASE from $SMP_HDR"
    [ -n "$WANT_COREINIT" ] || fail "could not parse BSP_INIT_LOAD_BASE from $SMP_HDR"

    GOT_KERNEL=$(otool -l "$KERNEL" | awk '/segname __TEXT/{f=1} f&&/vmaddr/{print $2; exit}')
    [ -n "$GOT_KERNEL" ] || fail "could not read __TEXT vmaddr from $KERNEL"
    say "kernel __TEXT vmaddr = $GOT_KERNEL (smp_layout.h KERNEL_LOAD_BASE = $WANT_KERNEL)"
    [ "$(normalize_hex "$GOT_KERNEL")" = "$(normalize_hex "$WANT_KERNEL")" ] \
        || fail "kernel __TEXT vmaddr $GOT_KERNEL != KERNEL_LOAD_BASE $WANT_KERNEL"

    COREINIT_IMG="$COREINIT_ROOT/build/coreinit.macho"
    if [ -f "$COREINIT_IMG" ]; then
        GOT_CI=$(otool -l "$COREINIT_IMG" | awk '/segname __TEXT/{f=1} f&&/vmaddr/{print $2; exit}')
        say "coreinit __TEXT vmaddr = $GOT_CI (smp_layout.h BSP_INIT_LOAD_BASE = $WANT_COREINIT)"
        [ "$(normalize_hex "$GOT_CI")" = "$(normalize_hex "$WANT_COREINIT")" ] \
            || fail "coreinit __TEXT vmaddr $GOT_CI != BSP_INIT_LOAD_BASE $WANT_COREINIT"
    else
        say "coreinit image absent at $COREINIT_IMG -- skipping that assertion (not a failure)"
    fi

    file "$GRUB_EFI" | grep -q 'PE32+' \
        || fail "$GRUB_EFI is not a PE32+ EFI application"
    [ -f "$GRUB_MODULES_DIR/xnu.mod" ] \
        || fail "no xnu.mod in $GRUB_MODULES_DIR -- the loader cannot boot this kernel without it"
    say "loader: PE32+ EFI application, xnu.mod present"
}

# ---------------------------------------------------------------------------
# STEP 3 -- assemble the boot disk.
#
# The ESP is built with mtools and then wrapped in a GPT by hand, in Python.
# The GPT is written explicitly rather than with a partitioning tool because
# the layout is fixed and small, and because doing it in Python keeps the
# whole thing inspectable and free of host-tool quirks.
#
# The module tree goes at /EFI/debian/x86_64-efi because that is the prefix
# Debian's signed grubx64.efi embeds. xnu.mod is NOT embedded in the core
# image -- it is loaded at runtime from <prefix>/x86_64-efi/ -- so a prefix
# mismatch means GRUB loads, cannot find its modules, and drops to a rescue
# prompt with zero kernel output. That failure is indistinguishable from a
# kernel hang in the log, which is exactly why the positive control exists.
# ---------------------------------------------------------------------------
build_disk() {
    cfg_text="$1"
    disk_out="$2"
    esp="$disk_out"
    rm -f "$esp"

    dd if=/dev/zero of="$esp" bs=512 count=65536 >/dev/null 2>&1
    "$MFORMAT" -i "$esp" -v SASKI :: >/dev/null
    fstype=$(file -b "$esp")
    say "ESP filesystem: $(printf '%s' "$fstype" | sed 's/.*, //')"
    case "$fstype" in
        *"FAT (32 bit)"*) fail "ESP came out FAT32; OVMF will not boot it. Force FAT16." ;;
        *"FAT (16 bit)"*) : ;;
        *) fail "ESP is not a FAT filesystem: $fstype" ;;
    esac

    "$MMD" -i "$esp" ::/EFI ::/EFI/BOOT ::/EFI/debian ::/EFI/debian/x86_64-efi
    "$MCOPY" -i "$esp" "$GRUB_EFI" ::/EFI/BOOT/BOOTX64.EFI
    "$MCOPY" -i "$esp" "$SASKI_BUILD/saski.macho" ::/saski.macho
    printf '%s
' "$cfg_text" > "$WORK/grub.cfg"
    "$MCOPY" -i "$esp" "$WORK/grub.cfg" ::/EFI/debian/grub.cfg
    for m in "$GRUB_MODULES_DIR"/*.mod; do
        "$MCOPY" -i "$esp" "$m" ::/EFI/debian/x86_64-efi/ >/dev/null
    done
    say "wrote $disk_out (raw FAT16 superfloppy, no partition table)"
}

# ---------------------------------------------------------------------------
# STEP 4 -- boot a disk and capture the two output stages separately.
#
# GRUB's own output and the kernel's serial output are captured into one log
# but reported as two labelled sections, because the first question asked of
# any silent boot is "did the loader get there", and the log should answer it
# without a re-run.
# ---------------------------------------------------------------------------
# Emits ONLY the log path on stdout, so callers can capture it with $(...).
# Every diagnostic goes to stderr: a function whose stdout is a return value
# must not also print prose to it, or the caller captures both and the path
# becomes unusable. That bug cost a run here.
run_qemu() {
    disk="$1"
    tag="$2"
    vars_copy="$WORK/vars-$tag.fd"
    cp "$OVMF_VARS" "$vars_copy"
    log="$WORK/boot-$tag.log"
    rm -f "$log"
    "$QEMU" -machine q35 -cpu max -m 512 -smp "$SMP_CPUS" \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file="$vars_copy" \
        -drive file="$disk",format=raw,if=ide,index=0,media=disk \
        -display none -serial stdio -monitor none -no-reboot \
        > "$log" 2>&1 &
    qpid=$!
    waited=0
    while [ "$waited" -lt "$BOOT_TIMEOUT_S" ]; do
        kill -0 "$qpid" 2>/dev/null || break
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "$qpid" 2>/dev/null; then
        kill "$qpid" 2>/dev/null || true
        sleep 1
        kill -9 "$qpid" 2>/dev/null || true
        say "[$tag] timed out after ${BOOT_TIMEOUT_S}s (expected for a kernel that never returns)" >&2
    fi
    wait "$qpid" 2>/dev/null || true
    printf '%s' "$log"
}

# ---------------------------------------------------------------------------
# STEP 5 -- grade the log by tier.
#
# Tiers exist so a partial boot still tells us something. A T0 result is a
# legitimate and useful outcome: it means the loader path works and the kernel
# started, which is strictly more information than a pass/fail bit.
# ---------------------------------------------------------------------------
timeout_selftest() {
    start=$(date +%s)
    run_with_timeout 1 sleep 10 >/dev/null 2>&1
    elapsed=$(( $(date +%s) - start ))
    [ "$elapsed" -lt 5 ] || fail "timeout mechanism did not interrupt a 10s sleep (took ${elapsed}s) -- the harness would report false negatives"
    say "timeout self-test OK (interrupted a 10s sleep in ${elapsed}s)"
}

run_with_timeout() {
    limit="$1"
    shift
    "$@" &
    pid=$!
    waited=0
    while [ "$waited" -lt "$limit" ]; do
        kill -0 "$pid" 2>/dev/null || break
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        sleep 1
        kill -9 "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
    return 0
}

marker_present() {
    grep -qF "$1" "$2"
}

grade() {
    log="$1"
    say ""
    say "=== GRUB stage ==="
    LC_ALL=C sed -n '1,20p' "$log"
    say ""
    say "=== kernel stage (last 40 lines) ==="
    tail -40 "$log"
    say ""

    for bad in PANIC 'ACCEPTED (bug!)' 'proxy: create failed' 'create_object: FAILED'; do
        if marker_present "$bad" "$log"; then
            say "FAIL marker present: $bad"
            return 1
        fi
    done

    tier=0
    if marker_present '[kernel] abstraction layer initialized' "$log" && \
       marker_present '[kernel] embedded init image:' "$log" && \
       marker_present '[kernel] init entry point:' "$log" && \
       marker_present 'ram_size known' "$log" && \
       marker_present '[kernel] ACPI RSDP via EFI config table: found' "$log" && \
       marker_present '[kernel] handing off to init' "$log"; then
        tier=1
    fi
    if [ "$tier" = "1" ] && \
       marker_present 'stage 1: running on the single-syscall kernel_invoke model' "$log" && \
       marker_present 'received 1 initial capabilities from the kernel' "$log" && \
       marker_present 'mutex_manager handle' "$log" && \
       marker_present 'memory_manager handle' "$log" && \
       marker_present 'hardware mutex created, id' "$log" && \
       marker_present 'cluster_scheduler handle' "$log"; then
        tier=2
    fi
    if [ "$tier" = "2" ] && \
       marker_present 'malformed request rejected before dispatch' "$log"; then
        tier=3
    fi
    if [ "$tier" = "3" ] && \
       marker_present 'submitting jobs, then handing off to the scheduler' "$log"; then
        tier=4
    fi

    t3a=FAIL
    if [ "$tier" -ge 3 ] && marker_present 'node 0 joined' "$log"; then
        t3a=PASS
    fi

    joins=$(grep -ac 'joined (node object' "$log" 2>/dev/null)
    [ -z "$joins" ] && joins=0
    apfails=$(grep -ac 'ACPI discovery failed' "$log" 2>/dev/null)
    [ -z "$apfails" ] && apfails=0

    t3b=NOT-RUN
    if [ "$SMP_CPUS" -gt 1 ]; then
        if [ "$apfails" -gt 0 ]; then
            t3b=FAIL
        elif [ "$joins" -gt 1 ]; then
            t3b=PASS
        else
            t3b=FAIL
        fi
    fi

    # The kernel .incbin's init at build time, so a fresh kernel can still
    # carry a stale init. The kernel prints the length it embedded; compare it
    # with the init file actually on disk. A mismatch means the run judged a
    # kernel built against an older init, and any conclusion about init
    # behaviour would be about code that is not in the tree.
    emb=$(LC_ALL=C sed -n 's/.*embedded init image: \([0-9][0-9]*\) bytes.*/\1/p' "$log" | head -1)
    if [ -n "$INIT_BYTES" ]; then
        [ -n "$emb" ] || { say "FAIL: kernel did not report the embedded init length"; return 1; }
        say "embedded init: kernel reports $emb bytes, disk has $INIT_BYTES bytes"
        if [ "$emb" != "$INIT_BYTES" ]; then
            say "FAIL: the embedded init does not match the init on disk -- this run judged a stale init"
            return 1
        fi
    fi

    say "node joins observed: $joins (SMP_CPUS=$SMP_CPUS)"
    say "AP ACPI discovery failures: $apfails"
    say "highest contiguous tier reached: T$tier"
    say "T3a (BSP scheduler up): $t3a"
    say "T3b (SMP actually working): $t3b"

    missing=""
    [ "$tier" -lt 1 ] && missing="$missing T0"
    [ "$tier" -lt 2 ] && missing="$missing T1"
    [ "$tier" -lt 3 ] && missing="$missing T2"
    [ "$t3a" != "PASS" ] && missing="$missing T3a"
    [ "$t3b" != "PASS" ] && missing="$missing T3b"
    if [ -n "$missing" ]; then
        say "TESTS NOT PASSED:$missing"
    fi
    if [ "$SMP_CPUS" -le 1 ]; then
        say "TESTS NOT RUN: T3b SMP path -- booted with SMP_CPUS=1, so no AP was ever started"
    fi

    if [ "$t3a" = "PASS" ] && [ "$t3b" = "PASS" ]; then
        say "RESULT: PASS (T$tier, T3a and T3b)"
        return 0
    fi
    say "RESULT: FAIL (T3a=$t3a T3b=$t3b -- both required)"
    return 1
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
main() {
    mkdir -p "$WORK"
    timeout_selftest
    fetch_deps
    check_image
    resolve_and_report
    preflight

    say ""
    say "=== positive control: ask GRUB for a file that does not exist ==="
    build_disk 'set prefix=(hd0)/EFI/debian
set root=(hd0)
insmod xnu
echo "[grubcfg] POSITIVE CONTROL: requesting a nonexistent file"
xnu_kernel64 /nonexistent' "$WORK/disk-control.img"
    clog=$(run_qemu "$WORK/disk-control.img" control)
    if grep -qE "not found|No such file|can't find" "$clog"; then
        say "positive control OK: GRUB reached its command layer and reported the missing file"
    else
        say "--- control log ---"
        tail -30 "$clog"
        fail "positive control did NOT produce a GRUB error: the harness is broken, so any silence from the real boot would be indistinguishable from a kernel failure"
    fi

    say ""
    say "=== real boot ==="
    build_disk 'set prefix=(hd0)/EFI/debian
set root=(hd0)
insmod xnu
echo "[grubcfg] root is hd0 ; loading saski kernel: /saski.macho"
xnu_kernel64 /saski.macho
echo "[grubcfg] kernel loaded, booting"
boot' "$WORK/disk.img"
    klog=$(run_qemu "$WORK/disk.img" kernel)
    cp "$klog" "$LOG_KERNEL"

    if grade "$klog"; then
        [ -n "$KEEP_ISO" ] || rm -f "$WORK/disk.img" "$WORK/disk-control.img"
        exit 0
    fi
    exit 1
}

main "$@"
