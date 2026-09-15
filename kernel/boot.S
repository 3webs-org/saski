/* SPDX-License-Identifier: 0BSD */

/* This kernel's entry point: loaded and run as a Mach-O by GRUB's
 * xnu_kernel64 command, over EFI (i386-pc/BIOS is out of scope --
 * clang's toolchain has no 32-bit Mach-O support at all, a genuine
 * toolchain gap, not a design choice this project made). This is the
 * only boot path this kernel has; an earlier ELF/multiboot path was
 * dropped once this one was confirmed working end to end, including
 * ACPI discovery and the full coreinit demo sequence.
 *
 * IMPORTANT, established the hard way: despite its name, xnu_kernel64
 * does NOT hand off in 64-bit long mode. Confirmed empirically via GDB
 * attached to QEMU: EFER.LME is clear at entry, CS is a 32-bit
 * protected-mode selector. GRUB parses/loads the file as a 64-bit
 * Mach-O (segments, LC_UNIXTHREAD's register layout) but the actual
 * CPU handoff mode matches what a real XNU kernel's own i386_init/
 * start.s would expect to do its OWN 32-to-64-bit transition from --
 * so that transition is this file's own job, all of it: identity map,
 * PAE, EFER.LME, paging, a 64-bit GDT, a far jump. LC_UNIXTHREAD
 * already leaves ESP valid (see tools/macho_to_xnu.py), so there's no
 * stack setup needed before any of that.
 *
 * This file starts in .code32 for exactly that reason: everything
 * before the far jump into long_mode_entry is ordinary 32-bit
 * protected-mode code. Absolute (not RIP-relative -- RIP-relative
 * addressing doesn't exist in 32-bit mode at all, confirmed as the
 * actual root cause of a real bug along the way) 32-bit immediates are
 * used for symbol addresses in this section; this only stays correct
 * because this kernel's own build (see build.sh) reliably loads at
 * runtime address 0x1000000 (16MB, via -pagezero_size), matching
 * link-time vmaddr exactly, confirmed repeatedly via GDB -- there is
 * no dyld, so nothing would ever rebase these if that stopped being
 * true. A second, subtler bug along the way: LLVM's integrated
 * assembler doesn't actually honor .code32 for an x86_64-apple-darwin
 * target -- it silently promotes some ambiguous instruction forms
 * (direct-symbol memory operands especially) to their 64-bit-mode
 * equivalents regardless, which don't exist at all in the 32-bit mode
 * this code actually runs in. Worked around throughout by using
 * instruction forms that aren't ambiguous between the two (load an
 * address as an immediate into a register, then use the register,
 * rather than referencing a symbol directly as a memory operand).
 *
 * Mach-O assembler syntax throughout, not ELF's: no .section/.type/
 * .size (matching coreinit/job_exec_switch.S's own convention), and
 * .data with explicit .space rather than .bss -- xnu_kernel64 passes
 * GRUB_MACHO_NOBSS to GRUB's own Mach-O loader, which silently skips
 * any segment whose filesize is 0 (true BSS), never even counting it
 * in the loaded memory range -- so the page tables, stack, and GDT
 * below need to be real, file-present zero/data bytes, not true
 * zero-fill. */

.data
.align 12 /* 4096 bytes, as a power of 2 -- Mach-O .align takes log2 */
.global _boot_pml4
_boot_pml4:
    .space 4096
boot_pdpt:
    .space 4096
boot_pd0:
    .space 4096
boot_pd1:
    .space 4096
boot_pd2:
    .space 4096
boot_pd3:
    .space 4096
.align 4
stack_bottom:
    .space 65536
.global _stack_top
_stack_top:
/* stack_top's exact position doesn't need to be the literal last byte
 * of the whole segment -- tools/macho_to_xnu.py places the initial
 * RSP/ESP at the END of the last LC_SEGMENT_64's own vmsize, which
 * only needs THIS reservation to make that segment's total vmsize
 * extend far enough to cover a real stack somewhere within it. */

.align 4
gdt64:
    .quad 0                                    /* null descriptor */
gdt64_code:
    .quad 0x00AF9A000000FFFF                   /* 64-bit code: L=1,P=1,S=1,exec/read */
gdt64_data:
    .quad 0x00AF92000000FFFF                   /* 64-bit data: P=1,S=1,read/write */
gdt64_end:
gdt64_pointer:
    .word gdt64_end - gdt64 - 1
    .long gdt64

/* The userspace init image's own Mach-O, embedded directly into this
 * kernel's own binary -- GRUB's xnu loader has no module list to
 * discover it from the way multiboot would, so it travels WITH the
 * kernel instead. kernel_boot (kernel.c) hands these bytes to
 * macho_load() exactly as given; that function doesn't know or care
 * where the bytes it's reading came from, or which init they even are
 * -- see include/init_abi.h's own note on why this kernel has no
 * opinion about that. The path is supplied by build.sh's own
 * INIT_MACHO variable -- this project's own reference implementation
 * (coreinit/, built by build_coreinit.sh) is simply build.sh's current
 * default, not something this file or kernel.c hardcodes. */
.align 4
.global _g_embedded_init_start
_g_embedded_init_start:
.incbin INIT_MACHO_PATH
.global _g_embedded_init_end
_g_embedded_init_end:

.data
.align 4
/* Physical address of XNU's own boot_args structure -- GRUB's REAL
 * boot protocol for xnu_kernel64, not something this project invented.
 * Confirmed by reading GRUB's own source (grub-core/loader/i386/xnu.c,
 * grub_xnu_boot): it always enters via grub_relocator32_boot (32-bit
 * protected mode, regardless of grub_xnu_is_64bit -- matching what
 * this file's own top-of-file comment already found empirically via
 * GDB, for exactly this reason: real XNU kernels are entered this way
 * and are expected to do their own 32-to-64 transition, which is
 * genuinely what this file already does), with EAX set to
 * grub_xnu_arg1, which is bootparams_target -- the physical address of
 * a struct grub_xnu_boot_params_v1 or _v2 (include/grub/i386/xnu.h)
 * GRUB itself allocated and filled in: command line, EFI memory map,
 * a pointer to XNU's own device tree, and -- v2 only -- total RAM
 * size, directly from GRUB's own memory-map discovery. This is
 * exactly "a pointer given to us externally" in the sense of this
 * project's own no-kernel-side-allocation principle (see
 * include/init_abi.h), just via XNU's actual protocol instead of a
 * project-specific convention. Which struct version GRUB uses depends
 * on whether it finds the literal ASCII string "Darwin Kernel Version "
 * followed by a number >= 11 anywhere in this kernel's own loaded
 * segments (grub-core/loader/machoXX.c's own do_load) -- see
 * g_xnu_version_string below, which exists purely to be found by that
 * scan. 0 here would mean EAX was somehow 0 at entry; never actually
 * observed, but kernel.c checks rather than assumes. */
.global _g_xnu_boot_args_addr
_g_xnu_boot_args_addr:
    .long 0

/* Exists only to be found by GRUB's own darwin_version string scan
 * (grub-core/loader/machoXX.c's do_load) -- never read by this
 * kernel's own code. "11" is the minimum that gets GRUB to use the
 * richer v2 boot_args format (grub_xnu_darwin_version >= 11 in
 * grub-core/loader/i386/xnu.c), which is the only version carrying
 * ram_size and the full EFI memory map GRUB itself discovered --
 * without this string, GRUB defaults to darwin_version 0, uses v1,
 * and ram_size is simply absent from the structure entirely. */
.global g_xnu_version_string
g_xnu_version_string:
    .ascii "Darwin Kernel Version 11.0.0\0"

.text
.code32
.global _start
_start:
    /* Capture boot_args (EAX) as the literal first instruction --
     * before the CR4 read just below (the next thing that touches
     * EAX), before anything else. See _g_xnu_boot_args_addr's own
     * comment for what this is and why it has to happen this early:
     * nothing downstream preserves EAX, and this value only ever
     * exists in that one register, this one time. */
    movl $_g_xnu_boot_args_addr, %ebx
    movl %eax, (%ebx)

    /* EFI/OVMF's own pre-boot state may still have interrupts enabled
     * -- and this kernel has no IDT of its own yet at this exact
     * point. Disable interrupts immediately, before anything else,
     * rather than assume they already are. */
    cli

    /* Straight into the identity map and the 32-to-64-bit transition
     * -- nothing else needs to happen in 32-bit mode. ACPI RSDP
     * discovery used to happen here too (see this file's own git
     * history / the transcript this was developed in), worked around
     * with a scan_for_rsdp32 that ran before this kernel's own CR3
     * reload, based on an empirical (never fully diagnosed) finding
     * that the RSDP was reliably present before the transition and
     * reliably gone after. Removed once boot_args -> the EFI system
     * table's own configuration table (kernel.c's own
     * xnu_boot_args_find_rsdp) turned out to be both a more principled
     * and more debuggable way to find it -- real C code, real
     * iteration, real kprintf, rather than a 32-bit asm scan that can
     * only be inspected with GDB one instruction at a time. Long mode
     * first, then whatever discovery needs doing, is simply the
     * right order: nothing about finding the RSDP requires being in
     * 32-bit mode at all, it just happened to be built that way
     * before boot_args was understood well enough to make this the
     * obvious alternative. */
    call setup_identity_map32

    /* Enable PAE (CR4 bit 5) -- required before entering long mode. */
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4

    /* Load CR3 with our own identity map's PML4 physical address. */
    movl $_boot_pml4, %eax
    movl %eax, %cr3

    /* Set EFER.LME (bit 8) via MSR 0xC0000080. */
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x100, %eax
    wrmsr

    /* Enable paging (CR0 bit 31). With LME already set, this
     * transitions the CPU into IA-32e compatibility mode -- still
     * executing 32-bit code until the far jump below loads a 64-bit
     * code segment. */
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0

    lgdt gdt64_pointer

    /* Far jump into the 64-bit code segment -- the actual 32-to-64-bit
     * fork point. */
    ljmp $0x08, $long_mode_entry

/* Builds a full, contiguous 4GB identity map (four 1GB PDs) covering
 * GB0-GB3 -- GB3 specifically for LAPIC/IOAPIC MMIO (0xFEE00000/
 * 0xFEC00000). The full 4GB, not a narrower range: GRUB's xnu
 * relocator chooses where to actually place this Mach-O in physical
 * memory, not something this project controls -- covering the whole
 * low 4GB avoids the kernel's own currently-executing code falling
 * outside a narrower map the instant CR3 changes. Ordinary 32-bit
 * protected-mode code throughout -- no REX prefixes, no 64-bit
 * registers, since none of those are valid yet at this point. */
setup_identity_map32:
    movl $_boot_pml4, %edi
    movl $6144, %ecx /* 6 * 4096 bytes / 4 = 6144 dwords -- PML4 + PDPT +
                      * 4 PDs, all zeroed together since they're
                      * contiguous in .data */
    xorl %eax, %eax
    rep stosl

    movl $boot_pdpt, %eax
    orl $0x3, %eax
    movl %eax, _boot_pml4

    movl $boot_pd0, %eax
    orl $0x3, %eax
    movl %eax, boot_pdpt
    movl $boot_pd1, %eax
    orl $0x3, %eax
    movl %eax, boot_pdpt + 8
    movl $boot_pd2, %eax
    orl $0x3, %eax
    movl %eax, boot_pdpt + 16
    movl $boot_pd3, %eax
    orl $0x3, %eax
    movl %eax, boot_pdpt + 24

    movl $boot_pd0, %edi
    xorl %eax, %eax
    call fill_pd32
    movl $boot_pd1, %edi
    movl $0x40000000, %eax
    call fill_pd32
    movl $boot_pd2, %edi
    movl $0x80000000, %eax
    call fill_pd32
    movl $boot_pd3, %edi
    movl $0xC0000000, %eax
    call fill_pd32

    ret

/* Fills one 512-entry PD, %edi = PD base, %eax = starting physical
 * address (2MB-aligned) -- clobbers %eax, %ecx, %edi. Each entry is 8
 * bytes (a 64-bit PTE), written as two 32-bit stores since this is
 * still 32-bit code. */
fill_pd32:
    orl $0x83, %eax
    movl $512, %ecx
.fill_pd32_loop:
    movl %eax, (%edi)
    movl $0, 4(%edi)
    addl $0x200000, %eax
    addl $8, %edi
    loop .fill_pd32_loop
    ret

.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movabsq $_stack_top, %rsp

    call _kernel_boot

    cli
.hang:
    hlt
    jmp .hang
