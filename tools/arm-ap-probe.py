#!/usr/bin/env python3
# SPDX-License-Identifier: 0BSD
"""Read each AP's timer-object IPC count through QEMU's physical-memory QMP.

Serial output from four cores can interleave characters, so even a core with
thousands of delivered IRQs may never print an intact line. The reference
init's g_ticks_seen is per-instance BSS; Mach-O gives its BSP address and
smp_layout.h gives each AP's slide. This host-only probe changes no guest
code or kernel ABI.

  arm-ap-probe.py addresses <init.macho> <smp_layout.h> <cpu_count>
  arm-ap-probe.py sample <qmp-socket> <comma-separated-hex-addresses>
"""
import json
import os
import re
import socket
import subprocess
import sys
from pathlib import Path

MIN_TICKS = 500


def addresses(image, header, cpu_count):
    symbols = subprocess.run(
        ["nm", "-arch", "arm64", image], capture_output=True, text=True, check=True
    ).stdout
    matches = re.findall(r"^([0-9a-fA-F]+) [bB] _g_ticks_seen$", symbols, re.M)
    if len(matches) != 1:
        raise ValueError("init Mach-O must export exactly one _g_ticks_seen symbol")
    tick_addr = int(matches[0], 16)

    compiler = os.environ.get("CC") or "clang"
    defines = subprocess.run(
        [compiler, "-target", "aarch64-none-elf", "-E", "-dM", "-x", "c",
         "-I", str(Path(header).resolve().parent), "-"],
        input='#include "smp_layout.h"\n', capture_output=True, text=True, check=True,
    ).stdout

    def number(name):
        match = re.search(r"^#define " + name + r"\s+(0x[0-9a-fA-F]+)", defines, re.M)
        if not match:
            raise ValueError("missing ARM layout constant " + name)
        return int(match.group(1), 16)

    base = number("CORE_REGION_BASE")
    size = number("CORE_REGION_SIZE")
    init_offset = number("CORE_INIT_OFFSET")
    bsp_init_base = number("BSP_INIT_LOAD_BASE")
    result = [tick_addr]
    for core in range(1, cpu_count):
        region = base + (core - 1) * size
        result.append(tick_addr + region + init_offset - bsp_init_base)
    return result


def sample(path, addrs):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
        conn.settimeout(2)
        conn.connect(path)
        stream = conn.makefile("rwb", buffering=0)
        if "QMP" not in json.loads(stream.readline()):
            raise ValueError("missing QMP greeting")

        def query(request):
            stream.write(json.dumps(request).encode() + b"\n")
            while True:
                reply = json.loads(stream.readline())
                if "error" in reply:
                    raise ValueError("QMP error: " + str(reply["error"]))
                if "return" in reply:
                    return reply["return"]

        query({"execute": "qmp_capabilities"})
        counts = []
        for addr in addrs:
            text = query({"execute": "human-monitor-command",
                          "arguments": {"command-line": f"xp /1wx 0x{addr:x}"}})
            match = re.search(r":\s*0x([0-9a-fA-F]+)", text)
            if not match:
                raise ValueError(f"cannot read timer count at 0x{addr:x}: {text!r}")
            counts.append(int(match.group(1), 16))
        return counts


def main():
    if sys.argv[1] == "addresses":
        result = addresses(sys.argv[2], sys.argv[3], int(sys.argv[4]))
        print(",".join(f"{addr:x}" for addr in result))
        return 0
    if sys.argv[1] == "sample":
        values = sample(sys.argv[2], [int(part, 16) for part in sys.argv[3].split(",")])
        if all(value >= MIN_TICKS for value in values):
            print("AP timer-object IPC counts: " + ", ".join(
                f"core {i}={value}" for i, value in enumerate(values)))
            return 0
        return 1
    raise ValueError("expected addresses or sample mode")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"arm-ap-probe: {error}", file=sys.stderr)
        sys.exit(2)
