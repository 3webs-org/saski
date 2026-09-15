#!/usr/bin/env python3
"""macho_to_xnu.py -- rewrite an ld64.lld-produced Mach-O64's load
commands so GRUB's xnu_kernel64 can actually boot it.

GRUB's Mach-O loader (grub-core/loader/machoXX.c) is deliberately
minimal: it only understands two load commands at all --
LC_SEGMENT_64 (which it loads verbatim, by fileoff/vmaddr/filesize/
vmsize) and LC_THREAD/LC_UNIXTHREAD (which is the ONLY place it looks
for an entry point -- it never reads LC_MAIN, which is what ld64.lld
emits by default for an ordinary executable). Everything else --
LC_SYMTAB, LC_DYSYMTAB, LC_LOAD_DYLINKER, LC_DYLD_INFO_ONLY, LC_UUID,
LC_VERSION_MIN_MACOSX, LC_FUNCTION_STARTS, LC_DATA_IN_CODE -- is
silently ignored by GRUB's own load-command iterator, so it's simply
dropped here rather than carried forward.

This does NOT move any segment's actual file bytes. It only replaces
the load-commands preamble (mach_header_64 + load commands), which
this project's own LC_SEGMENT_64 for __TEXT always starts at fileoff 0
and is large enough to hold the new, SMALLER set of commands (dropping
everything but the LC_SEGMENT_64s more than makes room for the larger
LC_UNIXTHREAD, which needs a full register-state block LC_MAIN never
carried) -- so every segment's own fileoff/vmaddr/filesize/vmsize
stays byte-for-byte identical to what ld64.lld produced. Only the
header and command list are rewritten in place.
"""
import struct
import sys

MH_MAGIC_64 = 0xfeedfacf
LC_SEGMENT_64 = 0x19
LC_MAIN = 0x80000028
LC_UNIXTHREAD = 0x5
X86_THREAD_STATE64 = 4


def read_load_commands(data):
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, _res = \
        struct.unpack_from('<IiIIIIII', data, 0)
    if magic != MH_MAGIC_64:
        raise SystemExit(f"not a 64-bit Mach-O (magic=0x{magic:x})")
    cmds = []
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', data, off)
        cmds.append((cmd, cmdsize, off))
        off += cmdsize
    return cputype, cpusubtype, filetype, cmds


def convert(in_path, out_path, extra_stack_bytes=0x8000):
    with open(in_path, 'rb') as f:
        data = bytearray(f.read())

    cputype, cpusubtype, filetype, cmds = read_load_commands(data)

    segments = []  # (segname, vmaddr, vmsize, fileoff, filesize, raw_bytes)
    entryoff = None
    for cmd, cmdsize, off in cmds:
        if cmd == LC_SEGMENT_64:
            raw = bytes(data[off:off + cmdsize])
            segments.append(raw)
        elif cmd == LC_MAIN:
            entryoff, _stacksize = struct.unpack_from('<QQ', data, off + 8)

    if entryoff is None:
        raise SystemExit("no LC_MAIN found -- was this really built with "
                         "ld64.lld's normal entry convention?")

    # __TEXT's vmaddr + entryoff is the entry point, since __TEXT's
    # fileoff is always 0 (covers the file from the very start, headers
    # included) -- found by segname now, not by position: with a
    # nonzero -pagezero_size (see build.sh -- needed so this
    # kernel's own load address doesn't collide with firmware-owned low
    # memory, e.g. where OVMF places the ACPI RSDP), __PAGEZERO is
    # segments[0] instead, not __TEXT.
    text_vmaddr = None
    for seg in segments:
        segname = seg[8:24].rstrip(b'\x00')
        if segname == b'__TEXT':
            text_vmaddr = struct.unpack_from('<Q', seg, 24)[0]
            break
    if text_vmaddr is None:
        raise SystemExit("no __TEXT segment found")
    entry_rip = text_vmaddr + entryoff

    # Stack: place it at the end of the LAST segment's own vmaddr
    # range, in a fresh, dedicated area appended past it -- simplest
    # to just reuse extra_stack_bytes of room the caller guarantees
    # exists there (the real kernel's linker script/segment layout
    # reserves this; see boot.S).
    last_seg = segments[-1]
    last_vmaddr, last_vmsize = struct.unpack_from('<QQ', last_seg, 24)
    stack_top = last_vmaddr + last_vmsize  # caller must ensure this
                                            # range is mapped and free
    # SysV x86-64 requires RSP to be 16-byte aligned immediately before
    # every `call` -- meaning it must ALREADY be 16-aligned right here,
    # at the very first instruction, before any call has pushed a
    # return address yet (matching what a normal function would see if
    # it had genuinely been `call`ed by something with a 16-aligned
    # RSP of its own). Round down rather than up: the reserved stack
    # space this relies on (see boot.S's own stack_bottom/
    # stack_top comment) only guarantees room BELOW the segment's own
    # end, not above it.
    stack_top &= ~0xF

    # Build the new LC_UNIXTHREAD.
    state = struct.pack('<21Q',
        0, 0, 0, 0, 0, 0, 0, stack_top,   # rax..rbp, rsp
        0, 0, 0, 0, 0, 0, 0, 0,           # r8..r15
        entry_rip,                         # rip
        0x2,                                # rflags
        0, 0, 0)                           # cs, fs, gs
    thread_cmdsize = 8 + 8 + len(state)
    thread = struct.pack('<IIII', LC_UNIXTHREAD, thread_cmdsize,
                         X86_THREAD_STATE64, len(state) // 4) + state

    new_ncmds = len(segments) + 1
    new_cmds_bytes = b''.join(segments) + thread
    new_sizeofcmds = len(new_cmds_bytes)

    header = struct.pack('<IiIIIIII',
        MH_MAGIC_64, cputype, cpusubtype, filetype,
        new_ncmds, new_sizeofcmds, 0, 0)

    preamble = header + new_cmds_bytes
    if len(preamble) > entryoff:
        raise SystemExit(
            f"new preamble ({len(preamble)} bytes) doesn't fit before "
            f"entryoff ({entryoff}) -- would need to shift segment file "
            f"offsets, not implemented")

    # Zero-pad the gap between the new (shorter) preamble and the
    # original entryoff, then keep everything from there on
    # byte-for-byte untouched.
    out = bytearray(preamble)
    out.extend(b'\x00' * (entryoff - len(preamble)))
    out.extend(data[entryoff:])

    with open(out_path, 'wb') as f:
        f.write(out)

    print(f"entry_rip=0x{entry_rip:x} stack_top=0x{stack_top:x} "
         f"segments={len(segments)} preamble={len(preamble)}/{entryoff} bytes")


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} in.macho out.macho", file=sys.stderr)
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
