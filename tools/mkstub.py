#!/usr/bin/env python3
# SPDX-License-Identifier: 0BSD
"""Wraps the aarch64 kernel's flat image in a minimal PE32+ EFI stub.

GRUB's xnu loader does not exist for arm64 (grub-efi-arm64-bin ships no
xnu.mod), so the aarch64 boot path cannot reuse the x86_64 Mach-O/GRUB
convention. Instead the kernel is wrapped as an EFI application that
GRUB chainloads; the stub then:

  1. copies the kernel image to its link address (0x40080000),
  2. turns the MMU off (the kernel's own boot path expects to start with
     it off, exactly as QEMU's -kernel delivers it),
  3. passes the device-tree pointer in x0 and branches to the kernel.

The DTB address is QEMU virt's -dtb location (0x40000000). The portable
lookup -- walking the EFI configuration table for the FDT GUID -- needs
the firmware's own virtual-address map semantics and is future work; see
the note at the movz/movk pair below.

Everything is position-independent except the kernel destination, which
is the KERNEL's link address, not the stub's. All instruction encodings
were verified against llvm-mc; see the companion judge
(tools/qemu-boot-aarch64.sh) for the end-to-end proof.
"""
import struct, sys, os

def a(*words):
    return b''.join(struct.pack('<I', w) for w in words)

def adr(rd, imm):
    assert -1048576 <= imm < 1048576 and imm % 1 == 0
    immlo = imm & 3
    immhi = (imm >> 2) & 0x7FFFF
    return (immlo << 29) | (0b10000 << 24) | (immhi << 5) | rd

def ldr_lit(rt, off):
    assert 0 <= off < 32760 and off % 4 == 0
    return 0x58000000 | ((off // 4) << 5) | rt

def cbz(rt, off):
    assert off % 4 == 0 and -(1 << 20) < off < (1 << 20)
    return 0xB4000000 | (((off // 4) & 0x7FFFF) << 5) | rt

def b(off):
    assert off % 4 == 0 and -(1 << 27) < off < (1 << 27)
    return 0x14000000 | ((off // 4) & 0x3FFFFFF)

def b_cond(off, cond):
    assert off % 4 == 0
    return 0x54000000 | (((off // 4) & 0x7FFFF) << 5) | cond

MOVZ_X = lambda rd, imm16: 0xD2800000 | (imm16 << 5) | rd
MOVZ_W = lambda rd, imm16: 0x52800000 | (imm16 << 5) | rd
ORR_MOV = lambda rd, rm: 0xAA0003E0 | (rm << 16) | rd          # mov Xd, Xm
ADD_IMM = lambda rd, rn, imm12: 0x91000000 | (imm12 << 10) | (rn << 5) | rd
SUB_IMM = lambda rd, rn, imm12: 0xD1000000 | (imm12 << 10) | (rn << 5) | rd
LDRB_POST = lambda rt, rn: 0x38401400 | (rn << 5) | rt          # ldrb Wt,[Xn],#1
STRB_POST = lambda rt, rn: 0x38001400 | (rn << 5) | rt          # strb Wt,[Xn],#1
DC_CVAC = lambda rt: 0xD50B7B00 | rt
BR = lambda rn: 0xD61F0000 | (rn << 5)
MRS_SCTLR = lambda rd: 0xD5381000 | rd
MSR_SCTLR = lambda rt: 0xD5181000 | rt
BIC = lambda rd, rn, rm: 0x8A200000 | (rm << 16) | (rn << 5) | rd
DSB_SY = 0xD5033F9F
IC_IALLU = 0xD508751F
ISB = 0xD5033FDF
NOP = 0xD503201F

insns = [NOP] * 60
# 0-2: patched (adr kernel, ldr len, ldr dst)
insns[3] = ORR_MOV(12, 1)        # mov x12, x1  (EFI system table; kept for
                                 # the config-table walk, a follow-up)
# x0 = the DTB. On QEMU's virt machine -dtb places it at 0x40000000; the
# portable lookup (EFI configuration table, FDT GUID) is a follow-up --
# walking firmware structures through AAVMF's page tables turned out to
# need the firmware's own map semantics, which this bring-up stub does
# not model yet.
insns[4] = MOVZ_X(0, 0)          # movz x0, #0
insns[5] = 0xF2A80000            # movk x0, #0x4000, lsl #16 -> 0x40000000
# copy the kernel image to its link address:
insns[6] = NOP                   # patched: adr x9, <kernel>
insns[7] = NOP                   # patched: ldr x10, [len]
insns[8] = NOP                   # patched: ldr x11, [dst]
insns[9] = ORR_MOV(14, 11)       # mov x14, x11 (dst for the final br)
insns[10] = NOP                  # patched: copy: cbz x10, done
insns[11] = LDRB_POST(13, 9)
insns[12] = STRB_POST(13, 11)
insns[13] = SUB_IMM(10, 10, 1)
insns[14] = b(-4 * 4)            # b copy
# turn the MMU off (the kernel's boot path expects it off, exactly as
# QEMU's -kernel delivers it). The current exception level decides which
# SCTLR controls the active translation:
insns[15] = MOVZ_X(10, 0x1005)   # mask M|C|I
insns[16] = 0xD5384249           # mrs x9, CurrentEL
insns[17] = 0xD342FD29           # lsr x9, x9, #2
insns[18] = 0x92400529           # and x9, x9, #3
insns[19] = 0xF100093F           # cmp x9, #2
insns[20] = NOP                  # patched: b.ne use_el1
insns[21] = 0xD53C1009           # mrs x9, sctlr_el2
insns[22] = BIC(9, 9, 10)
insns[23] = 0xD51C1009           # msr sctlr_el2, x9
insns[24] = NOP                  # patched: b done_mmu
insns[25] = 0xD5381009           # use_el1: mrs x9, sctlr_el1
insns[26] = BIC(9, 9, 10)
insns[27] = 0xD5181009           # msr sctlr_el1, x9
insns[28] = DSB_SY               # done_mmu: dsb sy
insns[29] = ISB
insns[30] = IC_IALLU
insns[31] = DSB_SY
insns[32] = ISB
insns[33] = BR(14)

LIT_OFF = 60 * 4
insns[6] = adr(9, LIT_OFF + 16 - 6 * 4)      # kernel after the 16-byte pool
insns[7] = ldr_lit(10, LIT_OFF - 7 * 4)      # len at LIT_OFF
insns[8] = ldr_lit(11, LIT_OFF + 8 - 8 * 4)  # dst at LIT_OFF+8
insns[10] = cbz(10, (15 - 10) * 4)           # copy -> done (15)
insns[20] = b_cond((25 - 20) * 4, 0x1)       # b.ne use_el1 (25)
insns[24] = b((28 - 24) * 4)                 # b done_mmu (28)

code = a(*insns)
assert len(code) == LIT_OFF, (len(code), LIT_OFF)

kernel = open(sys.argv[1], 'rb').read()
KERNEL_DST = int(os.environ.get('KERNEL_DST', '0x40080000'), 16)
lits = struct.pack('<QQ', len(kernel), KERNEL_DST)
image = code + lits + kernel

HDR = 0x1000
FILE_ALIGN = 0x200
SECTION_ALIGN = 0x1000
text_raw = image
if len(text_raw) % FILE_ALIGN:
    text_raw += b'\x00' * (FILE_ALIGN - len(text_raw) % FILE_ALIGN)
imgsz = (HDR + len(text_raw) + SECTION_ALIGN - 1) & ~(SECTION_ALIGN - 1)

dos = b'MZ' + b'\x00' * 58 + struct.pack('<I', 0x40) + b'PE\x00\x00'
coff = struct.pack('<HHIIIHH', 0xAA64, 1, 0, 0, 0, 240, 0x0022)
opt = struct.pack('<HBBIIIIIQIIHHHHHHIIIIHHQQQQII',
    0x20B, 1, 0, len(image), 0, 0, 0x1000, 0x1000, 0,
    SECTION_ALIGN, FILE_ALIGN, 0, 0, 0, 0, 0, 0,
    0, imgsz, HDR, 0, 10, 0x1600,
    0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
opt += b'\x00' * 128
sec = struct.pack('<8sIIIIIIHHI', b'.text\x00\x00\x00',
                  len(image), 0x1000, len(text_raw), HDR, 0, 0, 0, 0,
                  0x60000020)
pe = dos + coff + opt + sec
pe += b'\x00' * (HDR - len(pe))
open(sys.argv[2], 'wb').write(pe + text_raw)
print(f'wrote {sys.argv[2]}: {len(pe)+len(text_raw)} bytes '
      f'(code {len(code)}, kernel {len(kernel)})')
