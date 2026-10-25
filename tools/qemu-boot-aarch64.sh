#!/bin/sh
# SPDX-License-Identifier: 0BSD
#
# The aarch64 QEMU boot judge for saski.
#
# Boots the kernel through the STANDARD-platform convention -- AAVMF
# firmware, GRUB (arm64-efi), the EFI stub -- and grades the serial
# output by tier, exactly as tools/qemu-boot.sh does for x86_64.
#
# Stock grub-efi-arm64-bin has no xnu.mod, so this judge tests both the
# chainloaded EFI stub and (when available) otos-coreinit's arm64 build of
# GRUB's xnu loader. The stub copies the flat kernel to its link address,
# turns the MMU off and enters with the DTB pointer in x0. The xnu path
# instead enters with its v2 boot-parameter block in x0.
#
# PURELY ADDITIVE, like the x86_64 judge: it consumes build/saski.efi
# and never touches the kernel build or sources.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SASKI_BUILD="${SASKI_BUILD:-$ROOT/build}"
DEPS_DIR="${DEPS_DIR:-$SASKI_BUILD/arm-deps}"
WORK="${WORK:-$SASKI_BUILD/arm-judge}"
# Multi-core GICv3 under TCG can need longer before every node's timer
# milestone appears. A caller-supplied BOOT_TIMEOUT_S overrides both defaults.
BOOT_TIMEOUT_S="${BOOT_TIMEOUT_S:-}"
SMP_CPUS="${SMP_CPUS:-1}"
FETCH_DEPS="${FETCH_DEPS:-1}"
COREINIT_ROOT="${COREINIT_ROOT:-$ROOT/../otos-coreinit}"
# Default matrix gates both QEMU virt interrupt controllers. GIC_VERSION=3
# selects just v3 for a focused run; GIC_VERSIONS="2 3" is the full matrix.
GIC_VERSIONS="${GIC_VERSIONS:-${GIC_VERSION:-2 3}}"
# GRUB's own xnu loader built for arm64-efi (otos-coreinit
# tools/grub-xnu-arm64). When present, the judge ALSO boots
# build/saski-xnu.macho through xnu_kernel64 with the x86_64 judge's
# grub.cfg byte for byte -- the uniform-convention path. Absent: NOT RUN.
XNU_MOD="${XNU_MOD:-$ROOT/../otos-coreinit/tools/grub-xnu-arm64/build/xnu.mod}"

QEMU="${QEMU:-/nix/store/59hhc8zlvdlbcls7dkbhkhw9afwb6mps-qemu-11.1.2/bin/qemu-system-aarch64}"
AAVMF_CODE="${AAVMF_CODE:-$DEPS_DIR/qemu-efi-aarch64/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd}"
AAVMF_VARS="${AAVMF_VARS:-$DEPS_DIR/qemu-efi-aarch64/usr/share/AAVMF/AAVMF_VARS.fd}"
GRUB_EFI="${GRUB_EFI:-$DEPS_DIR/grub-efi-arm64-signed/usr/lib/grub/arm64-efi-signed/grubaa64.efi.signed}"
GRUB_MODULES_DIR="${GRUB_MODULES_DIR:-$DEPS_DIR/grub-efi-arm64-bin/usr/lib/grub/arm64-efi}"
MTOOLS_BIN="${MTOOLS_BIN:-/nix/store/nmcdf0f6c58r9g3l26104a5ak32m207p-mtools-4.0.49/bin}"

say() { printf '%s\n' "$*"; }
fail() { printf 'qemu-boot-aarch64.sh: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# STEP 0 -- fetch the firmware and loader artifacts if absent. Same pure-
# data .deb extraction as the x86_64 judge (a .deb is an ar archive
# containing a tar; no Linux binary is executed).
# ---------------------------------------------------------------------------
fetch_deps() {
    [ -d "$GRUB_MODULES_DIR" ] && [ -f "$GRUB_EFI" ] && [ -f "$AAVMF_CODE" ] && return 0
    [ "$FETCH_DEPS" = "1" ] || fail "deps missing and FETCH_DEPS=0"
    say "fetching aarch64 firmware and loader artifacts into $DEPS_DIR"
    mkdir -p "$DEPS_DIR"
    python3 - "$DEPS_DIR" <<'PY_EOF'
import os, sys, subprocess, gzip, tarfile, hashlib

dest = sys.argv[1]
base = "https://deb.debian.org/debian"
index = base + "/dists/trixie/main/binary-arm64/Packages.gz"
raw = subprocess.run(["curl", "-sSL", "--max-time", "180", index],
                     capture_output=True, check=True).stdout
text = gzip.decompress(raw).decode("utf-8", "replace")
want = ["grub-efi-arm64-bin", "grub-efi-arm64-signed", "qemu-efi-aarch64"]
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
        print("MISSING INDEX ENTRY: " + pkg); sys.exit(1)
    fn = entries[pkg]["filename"]
    url = base + "/" + fn.lstrip("/")
    out = os.path.join(dest, pkg + ".deb")
    if not os.path.exists(out):
        subprocess.run(["curl", "-sSL", "--max-time", "300", "-o", out, url], check=True)
    data = open(out, "rb").read()
    if data[:8] != b"!<arch>\n":
        print("NOT AN AR ARCHIVE: " + pkg); sys.exit(1)
    md5 = hashlib.md5(data).hexdigest()
    if "md5" in entries[pkg] and md5 != entries[pkg]["md5"]:
        print("MD5 MISMATCH " + pkg); sys.exit(1)
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
}

# ---------------------------------------------------------------------------
# STEP 1 -- pre-boot assertions.
# ---------------------------------------------------------------------------
check_image() {
    STUB="$SASKI_BUILD/saski.efi"
    [ -f "$STUB" ] || fail "no EFI stub at $STUB -- run cmake --build with -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake first"
    file "$STUB" | grep -q 'PE32+ executable (EFI application) Aarch64' \
        || fail "$STUB is not an Aarch64 EFI application"
    SNAPSHOT="$SASKI_BUILD/embedded-init.macho"
    SOURCE_INIT="${INIT_MACHO:-$COREINIT_ROOT/build/coreinit.macho}"
    [ -f "$SNAPSHOT" ] || fail "no $SNAPSHOT -- rebuild with the aarch64 toolchain file"
    [ -f "$SOURCE_INIT" ] || fail "no init at $SOURCE_INIT -- set INIT_MACHO to the image embedded by the cmake build"
    cmp -s "$SNAPSHOT" "$SOURCE_INIT" || fail "init image changed since $STUB was built -- rebuild saski before judging it"
    say "stub: $(file -b "$STUB" | sed 's/, for MS Windows//')"
}

# ---------------------------------------------------------------------------
# STEP 2 -- assemble the boot disk: FAT16 ESP, GRUB signed image at
# /EFI/BOOT/BOOTAA64.EFI, the module tree at /EFI/debian/arm64-efi (the
# prefix Debian's grubaa64.efi embeds), the stub at /saski.efi.
# ---------------------------------------------------------------------------
build_disk() {
    esp="$WORK/disk.img"
    rm -f "$esp"
    dd if=/dev/zero of="$esp" bs=512 count=65536 >/dev/null 2>&1
    "$MTOOLS_BIN/mformat" -i "$esp" -v SASKI :: >/dev/null
    fstype=$(file -b "$esp")
    case "$fstype" in
        *"FAT (32 bit)"*) fail "ESP came out FAT32; AAVMF will not boot it. Force FAT16." ;;
        *"FAT (16 bit)"*) : ;;
        *) fail "ESP is not a FAT filesystem: $fstype" ;;
    esac
    "$MTOOLS_BIN/mmd" -i "$esp" ::/EFI ::/EFI/BOOT ::/EFI/debian ::/EFI/debian/arm64-efi
    "$MTOOLS_BIN/mcopy" -i "$esp" "$GRUB_EFI" ::/EFI/BOOT/BOOTAA64.EFI
    "$MTOOLS_BIN/mcopy" -i "$esp" "$SASKI_BUILD/saski.efi" ::/saski.efi
    printf '%s\n' 'set prefix=(hd0)/EFI/debian
set root=(hd0)
chainloader /saski.efi
boot' > "$WORK/grub.cfg"
    "$MTOOLS_BIN/mcopy" -i "$esp" "$WORK/grub.cfg" ::/EFI/debian/grub.cfg
    for m in "$GRUB_MODULES_DIR"/*.mod; do
        "$MTOOLS_BIN/mcopy" -i "$esp" "$m" ::/EFI/debian/arm64-efi/ >/dev/null
    done
    say "wrote $esp (raw FAT16 superfloppy, no partition table)"
}

# ---------------------------------------------------------------------------
# STEP 3 -- boot and grade.
# ---------------------------------------------------------------------------
run_qemu() {
    disk="${1:-$WORK/disk.img}"
    vars_copy="$WORK/vars.fd"
    cp "$AAVMF_VARS" "$vars_copy"
    tag="$(basename "$disk" .img)-gic${GIC_VERSION}-${2:-stub}"
    log="$WORK/boot-$tag.log"
    qmp_socket="$WORK/qmp-$tag.sock"
    probe="${log%.log}.ap-timers"
    rm -f "$log" "$probe" "$qmp_socket"
    if [ "$SMP_CPUS" -gt 1 ]; then
        ap_addresses=$(python3 "$ROOT/tools/arm-ap-probe.py" addresses \
            "$SNAPSHOT" "$ROOT/include/smp_layout.h" "$SMP_CPUS") || return 1
    fi
    "$QEMU" -machine "virt,gic-version=${GIC_VERSION}${QEMU_MACHINE_OPTS:-}" -cpu cortex-a57 -m 512 -smp "$SMP_CPUS" \
        -drive if=pflash,format=raw,readonly=on,file="$AAVMF_CODE" \
        -drive if=pflash,format=raw,file="$vars_copy" \
        -drive file="$disk",format=raw,if=virtio \
        -display none -serial file:"$log" -monitor none \
        -qmp "unix:$qmp_socket,server,nowait" -no-reboot \
        > /dev/null 2>&1 &
    qpid=$!
    timeout_s="${BOOT_TIMEOUT_S:-60}"
    if [ "$SMP_CPUS" -gt 1 ]; then
        timeout_s="${BOOT_TIMEOUT_S:-100}"
        if [ "$GIC_VERSION" = 3 ]; then
            timeout_s="${BOOT_TIMEOUT_S:-120}"
        fi
    fi
    waited=0
    stable=0
    while [ "$waited" -lt "$timeout_s" ]; do
        kill -0 "$qpid" 2>/dev/null || break
        sleep 1
        waited=$((waited + 1))
        # QMP physical reads briefly stop TCG vCPUs. Let the guest boot
        # without probes first, then sample at most once per ten seconds.
        # Once all cores have 500 IRQ-object invocations, watch five more
        # seconds for delayed faults before ending this finite boot test.
        if [ "$SMP_CPUS" -gt 1 ] && [ -s "$probe" ]; then
            stable=$((stable + 1))
            [ "$stable" -lt 5 ] || break
        elif [ "$SMP_CPUS" -gt 1 ] && [ "$waited" -ge 30 ] && \
             [ $((waited % 10)) -eq 0 ] && [ -S "$qmp_socket" ]; then
            if python3 "$ROOT/tools/arm-ap-probe.py" sample "$qmp_socket" \
                "$ap_addresses" >"$probe.tmp" 2>"$probe.err"; then
                mv "$probe.tmp" "$probe"
            else
                rm -f "$probe.tmp"
            fi
        fi
    done
    kill "$qpid" 2>/dev/null || true
    sleep 1
    kill -9 "$qpid" 2>/dev/null || true
    wait "$qpid" 2>/dev/null || true
    printf '%s' "$log"
}

grade() {
    log="$1"
    say ""
    say "=== firmware + GRUB stage ==="
    sed -n '1,8p' "$log"
    say ""
    say "=== kernel stage ==="
    tail -20 "$log"
    say ""
    if grep -qF 'Synchronous Exception at' "$log" ||
       grep -qF 'UNHANDLED ARM exception' "$log" ||
       grep -qF '[kernel] PANIC' "$log"; then
        say "RESULT: FAIL (unhandled exception or kernel panic after boot)"
        return 1
    fi
    tier=0
    if grep -qF '[kernel] aarch64 boot' "$log"; then tier=1; fi
    if [ "$tier" = "1" ] && grep -qF 'DTB at 0x' "$log" && grep -qF 'valid' "$log"; then tier=2; fi
    if [ "$tier" = "2" ] && grep -qF 'memory: base' "$log"; then tier=3; fi
    if [ "$tier" = "3" ] && grep -qF '[kernel] embedded init image:' "$log" && \
       grep -qF '[kernel] handing off to init' "$log" && \
       grep -qF '[kernel] factory: created handle' "$log"; then tier=4; fi
    if [ "$tier" = "4" ] && grep -qF '[coreinit] stage 1:' "$log"; then tier=5; fi
    # The BSP resource-demo line may interleave with APs' early unguarded
    # serial writes. In SMP mode per-core timer milestones below are the
    # stronger evidence that scheduled work actually ran on every node.
    if [ "$tier" = "5" ] && \
       grep -qF '[coreinit] === job: timer bring-up ===' "$log" && \
       { [ "$SMP_CPUS" -gt 1 ] ||
         grep -qF '[coreinit] === job: resource declaration + reassignment demo ===' "$log"; }; then tier=6; fi
    if [ "$tier" = "6" ] && \
       grep -qF 'cross_core_signal: self-test PASS' "$log"; then tier=7; fi
    required_tier=7
    if [ "$SMP_CPUS" -gt 1 ]; then
        required_tier=8
        if [ "$tier" = "7" ] && [ -s "${log%.log}.ap-timers" ]; then tier=8; fi
    fi
    say "highest tier reached: T$tier"
    if [ "$tier" -ge "$required_tier" ]; then
        say "RESULT: PASS (T$tier: init handoff, self-IPI, $SMP_CPUS core(s) serving jobs)"
        return 0
    fi
    say "RESULT: FAIL (highest partial tier T$tier; T$required_tier required)"
    return 1
}

# The xnu_kernel64 path: same disk, plus xnu.mod and the Mach-O, and the
# x86_64 judge's real-boot grub.cfg unchanged. Graded on the entry
# contract the kernel can observe: it booted, and it recognised x0 as a v2
# boot-parameter block. Machine-description discovery through its EFI
# system table and a working self-IPI are both required.
xnu_boot() {
    [ -f "$XNU_MOD" ] || { say "xnu path: NOT RUN (no xnu.mod at $XNU_MOD)"; return 0; }
    [ -f "$SASKI_BUILD/saski-xnu.macho" ] || fail "no $SASKI_BUILD/saski-xnu.macho -- rebuild with the aarch64 toolchain file"
    xdisk="$WORK/disk-xnu.img"
    cp "$WORK/disk.img" "$xdisk"
    "$MTOOLS_BIN/mcopy" -o -i "$xdisk" "$XNU_MOD" ::/EFI/debian/arm64-efi/xnu.mod
    "$MTOOLS_BIN/mcopy" -o -i "$xdisk" "$SASKI_BUILD/saski-xnu.macho" ::/saski.macho
    printf '%s\n' 'set prefix=(hd0)/EFI/debian
set root=(hd0)
insmod xnu
echo "[grubcfg] root is hd0 ; loading saski kernel: /saski.macho"
xnu_kernel64 /saski.macho
echo "[grubcfg] kernel loaded, booting"
boot' > "$WORK/grub-xnu.cfg"
    "$MTOOLS_BIN/mcopy" -o -i "$xdisk" "$WORK/grub-xnu.cfg" ::/EFI/debian/grub.cfg
    # Two firmware personalities, both standard: AAVMF publishes ACPI and
    # hides the DT by default, and publishes the DT when ACPI is off. The
    # kernel must find the machine description either way.
    xnu_rc=0
    for mode in acpi dt; do
        if [ "$mode" = dt ]; then QEMU_MACHINE_OPTS=",acpi=off"; want='DTB found via EFI configuration table'
        else QEMU_MACHINE_OPTS=""; want='ACPI RSDP via EFI configuration table'; fi
        say ""
        say "=== booting under AAVMF ($mode, GICv$GIC_VERSION) + GRUB xnu_kernel64 ==="
        xlog=$(QEMU_MACHINE_OPTS="$QEMU_MACHINE_OPTS" run_qemu "$xdisk" "$mode")
        cp "$xlog" "$WORK/boot-xnu-$mode-gic$GIC_VERSION.log"
        sed -n '4,20p' "$xlog"
        if ! grep -qF 'Synchronous Exception at' "$xlog" && \
           ! grep -qF 'UNHANDLED ARM exception' "$xlog" && \
           ! grep -qF '[kernel] PANIC' "$xlog" && \
           grep -qF '[kernel] aarch64 boot' "$xlog" && \
           grep -qF 'x0 = xnu boot params v2' "$xlog" && \
           grep -qF "$want" "$xlog" && \
           grep -qF '[kernel] embedded init image:' "$xlog" && \
           grep -qF '[kernel] handing off to init' "$xlog" && \
           grep -qF '[kernel] factory: created handle' "$xlog" && \
           grep -qF '[coreinit] stage 1:' "$xlog" && \
           grep -qF '[coreinit] === job: timer bring-up ===' "$xlog" && \
           { [ "$SMP_CPUS" -gt 1 ] ||
             grep -qF '[coreinit] === job: resource declaration + reassignment demo ===' "$xlog"; } && \
           grep -qF 'cross_core_signal: self-test PASS' "$xlog" && \
           { [ "$SMP_CPUS" -le 1 ] || [ -s "${xlog%.log}.ap-timers" ]; }; then
            say "xnu path ($mode, GICv$GIC_VERSION): PASS ($SMP_CPUS core(s), $want, self-IPI)"
        else
            say "xnu path ($mode, GICv$GIC_VERSION): FAIL (wanted: $want, self-IPI and $SMP_CPUS core(s))"
            xnu_rc=1
        fi
    done
    return "$xnu_rc"
}

main() {
    mkdir -p "$WORK"
    fetch_deps
    check_image
    build_disk
    say ""
    rc=0
    for gic_version in $GIC_VERSIONS; do
        case "$gic_version" in
            2|3) GIC_VERSION="$gic_version" ;;
            *) fail "unsupported GIC version $gic_version (expected 2 or 3)" ;;
        esac
        say ""
        say "=== booting under AAVMF + GRUB (arm64-efi, GICv$GIC_VERSION) ==="
        klog=$(run_qemu)
        grade "$klog" || rc=1
        xnu_boot || rc=1
    done
    if [ "$rc" -eq 0 ]; then
        say "GIC matrix: PASS ($GIC_VERSIONS)"
    else
        say "GIC matrix: FAIL ($GIC_VERSIONS)"
    fi
    exit "$rc"
}

main "$@"
