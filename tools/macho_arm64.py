#!/usr/bin/env python3
# SPDX-License-Identifier: 0BSD
"""Wraps the aarch64 kernel's flat image as the Mach-O GRUB's xnu_kernel64
expects on arm64: MH_EXECUTE, one __TEXT segment placing the image at its
link address, and an LC_UNIXTHREAD carrying ARM_THREAD_STATE64 whose pc is
the kernel entry -- the arm64 counterpart of what tools/macho_to_xnu.py
does for x86_64 (which emits x86_THREAD_STATE64).

The loader (otos-coreinit tools/grub-xnu-arm64) enters with x0 = struct
grub_xnu_boot_params_v2 *, MMU and D-cache off, sp set, IRQs masked, at
EL1 or EL2. kernel_boot_aarch64 distinguishes that from a DTB in x0.

The image length comes from the image's own arm64 Image header
(image_size at offset 16, = _end - _start), so .bss is covered by the
segment: the file is zero-padded to it rather than trusting any loader to
zero-fill a segment tail.

usage: macho_arm64.py <saski.bin> <load_base> <out.macho>"""
import struct, sys

PAGE = 0x1000
MH_MAGIC_64, CPU_TYPE_ARM64, MH_EXECUTE, MH_NOUNDEFS = 0xfeedfacf, 0x0100000c, 2, 1
LC_SEGMENT_64, LC_UNIXTHREAD, ARM_THREAD_STATE64 = 0x19, 0x5, 6

image = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 0)
assert image[56:60] == b'ARM\x64', 'no arm64 Image header: not a saski aarch64 image'
image_size = struct.unpack_from('<Q', image, 16)[0]
assert image_size >= len(image), (image_size, len(image))
image = image + b'\0' * (image_size - len(image))

# The header occupies the page below the image, so the image's first byte
# is at file offset PAGE and maps to vmaddr base.
seg_vmaddr = base - PAGE
filesize = PAGE + len(image)
vmsize = (filesize + PAGE - 1) & ~(PAGE - 1)
seg = struct.pack('<II16sQQQQiiII', LC_SEGMENT_64, 72, b'__TEXT',
                  seg_vmaddr, vmsize, 0, filesize, 7, 7, 0, 0)
# ARM_THREAD_STATE64: x0..x28, fp, lr, sp, pc, cpsr, pad (68 words).
state = struct.pack('<29QQQQQII', *([0] * 29), 0, 0, 0, base, 0x3c5, 0)
thread = struct.pack('<IIII', LC_UNIXTHREAD, 16 + len(state),
                     ARM_THREAD_STATE64, len(state) // 4) + state
cmds = seg + thread
hdr = struct.pack('<IiiIIIII', MH_MAGIC_64, CPU_TYPE_ARM64, 0, MH_EXECUTE,
                  2, len(cmds), MH_NOUNDEFS, 0)
assert len(hdr) + len(cmds) <= PAGE
out = bytearray(PAGE)
out[:len(hdr) + len(cmds)] = hdr + cmds
out += image
open(sys.argv[3], 'wb').write(out)
print('wrote %s: Mach-O arm64, __TEXT 0x%x+0x%x, entry 0x%x' %
      (sys.argv[3], seg_vmaddr, vmsize, base))
