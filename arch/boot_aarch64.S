/* SPDX-License-Identifier: 0BSD */
/*
 * aarch64 entry point.
 *
 * NOT in the x86_64 compile list and never will be: this file is the whole of
 * the aarch64 boot path and adding it must not perturb the x86 one. It is
 * compiled when ARCH=aarch64.
 *
 * THE ARM64 BOOT PROTOCOL, stated because the whole file follows from it:
 * the kernel is entered at EL1 or EL2 with the MMU off, caches in an
 * implementation-defined state, and:
 *   x0 = physical address of the device tree blob
 *   x1 = 0 (reserved)
 *   x2 = 0 (reserved)
 *   x3 = 0 (reserved)
 * x0 is the ONLY description of the machine this kernel is given. It is also
 * untrusted: it comes from whatever loaded us, so it is validated before a
 * byte of it is dereferenced.
 *
 * WHY THE CAPTURE IS THE FIRST INSTRUCTION. Nothing downstream preserves x0.
 * The x86 path had the same constraint with EAX and captures it in the opening
 * instruction of _start for exactly this reason; the failure mode otherwise is
 * a subtly wrong pointer that only shows up as device discovery failing much
 * later, which reads as a discovery bug rather than a lost register.
 *
 * WHY VALIDATION HAPPENS HERE, IN ASSEMBLY, RATHER THAN IN C. The blob's
 * address may be outside whatever identity map this early stub has, and a
 * dereference that faults before the exception vectors exist is a silent hang
 * with no output at all -- undiagnosable from a serial log. So the header is
 * checked by hand against the four bytes of magic and its own declared
 * totalsize BEFORE the kernel proper is entered, and an invalid blob is
traded for a zero, which C code can report. Magic is checked first because
 * totalsize is the blob's own claim about its own extent: trusting it before
 * the magic means a hostile blob sets the bounds it is checked against.
 */

#define FDT_MAGIC 0xd00dfeed

.text
.global _start
_start:
    /* The arm64 Image header (Documentation/arm64/booting.rst). It is what
     * lets ONE link address serve every boot path: QEMU's -kernel honours
     * text_offset when image_size is non-zero, so the flat image lands at
     * its link address rather than QEMU's default ram_base + 0x80000;
     * GRUB's xnu loader and the EFI stub place the image by its declared
     * address and never read this header. code0 branches over the header,
     * so entry at _start is the same instruction stream on every path. */
    b    real_start               /* code0 */
    .word 0                       /* code1 */
    .quad KERNEL_TEXT_OFFSET      /* text_offset from the start of RAM */
    .quad _end - _start           /* image_size: non-zero, so text_offset is honoured */
    .quad 0xa                     /* flags: LE, 4K pages, place anywhere */
    .quad 0, 0, 0                 /* res2..res4 */
    .ascii "ARM\x64"             /* magic */
    .word 0                       /* res5 */
real_start:
    /* x0 captured raw first: the DTB on the arm64 boot protocol, or a
     * pointer to GRUB's xnu boot-parameter block on the xnu path. Which
     * one it is gets decided in C (kernel_boot_aarch64), never assumed. */
    ldr  x9, =g_boot_x0
    str  x0, [x9]
    /* x0 as a DTB. */
    /* ldr =sym rather than adrp/add: both should work, but ldr =sym is one
     * instruction and puts the address in a literal pool the linker fills,
     * which is easier to audit than a PC-relative pair whose correctness
     * depends on the linker processing the relocation. */
    ldr  x9, =g_fdt_blob_phys
    str  x0, [x9]

    /* Validate, guarded: a null blob, or one whose first word is not the
     * magic, is recorded as absent rather than dereferenced. */
    cbz x0, no_fdt
    ldr w10, [x0]                 /* magic, big-endian */
    /* 0xd00dfeed is not a logical immediate, so it cannot be a single MOV.
     * Building it from two halves is also clearer about what is being
     * compared than a literal would be. */
    movz w11, #0xfeed
    movk w11, #0xd00d, lsl #16
    /* The blob is big-endian and this core may be either; compare byte-wise
     * rather than via a byte-swap so the check does not depend on the
     * current endianness setting. */
    rev w10, w10
    cmp w10, w11
    b.ne no_fdt

    /* Magic is good, so totalsize may now be believed -- and only now. Read
     * it, and use it as the size passed to fdt_valid in C, which re-checks
     * every block offset against it. */
    ldr w12, [x0, #4]
    rev w12, w12
    ldr  x9, =g_fdt_blob_size
    str  x12, [x9]
    b    fdt_ok

no_fdt:
    /* Zero both, so C sees "no blob" rather than a garbage length. */
    ldr  x9, =g_fdt_blob_phys
    str  xzr, [x9]
    ldr  x9, =g_fdt_blob_size
    str  xzr, [x9]

fdt_ok:
    /* x1..x3 are reserved zero per the protocol; do not rely on that, but do
     * not preserve them either -- nothing in this kernel reads them. */

    /* Stack. The linker places this image at its own vmaddr; the stack lives
     * at the end of the image's own data so it needs no separate allocation
     * and cannot collide with the blob, which sits wherever firmware put it. */
    /* Zero .bss: every loader here copies file bytes only, so nothing
     * else guarantees it. */
    ldr  x9, =_edata
    ldr  x10, =_end
1:  cmp  x9, x10
    b.hs 2f
    strb wzr, [x9], #1
    b    1b
2:
    ldr  x9, =_stack_top
    mov  sp, x9

    /* Enter C. The aarch64 kernel_boot is not the x86 one: that function is
     * built around boot_args and ACPI and has no meaning here. See
     * kernel/kernel.c's own note. */
    bl   kernel_boot_aarch64

    /* kernel_boot_aarch64 is not expected to return. If it does, halt rather
     * than run off into whatever follows in the image. */
hang:
    wfi
    b hang

/* The new boot-protocol entry point (see otos-coreinit's own
 * include/boot_protocol.h for the full five-register contract this
 * reads). A SEPARATE entry from _start above, not a replacement --
 * GRUB's xnu loader and the arm64 boot protocol's own -kernel path
 * both still enter at _start, with its own x0-only contract,
 * completely unchanged. This symbol is reached only by a loader that
 * implements the new protocol (otos-grub, otos-boot), which branches
 * here directly instead of to _start, because the new protocol's
 * x1-x4 have nothing to do with _start's own "x1..x3 reserved zero"
 * contract.
 *
 * x0 = a real FDT physical address (same meaning as _start's own x0;
 * validated the same way, byte for byte -- see that validation's own
 * comment above, not re-explained here). x1 = MachineDescription
 * object's physical address (opaque; forwarded into init_boot_info
 * unread). x2 = InitDescription object's physical address (opaque;
 * forwarded unread). x3 = SaskiBootParams struct's physical address --
 * saski's own data, read directly by the C side, not forwarded blind.
 * x4 = init's already-resolved entry point, called directly: this
 * path never parses init's own Mach-O at all (kernel_boot_aarch64's
 * own macho_load call, above, has nothing to do here), because the
 * loader already resolved it -- the whole point of this protocol. */
.global boot_protocol_start
boot_protocol_start:
    ldr  x9, =g_boot_x0
    str  x0, [x9]
    ldr  x9, =g_fdt_blob_phys
    str  x0, [x9]
    ldr  x9, =g_machine_description_phys
    str  x1, [x9]
    ldr  x9, =g_init_description_phys
    str  x2, [x9]
    ldr  x9, =g_saski_boot_params_phys
    str  x3, [x9]
    ldr  x9, =g_init_entry_phys
    str  x4, [x9]

    /* Validate the FDT exactly like _start does above -- same bytes,
     * same reasoning, duplicated rather than shared because _start's
     * own copy runs before this symbol could be called from it. */
    cbz x0, boot_protocol_no_fdt
    ldr w10, [x0]
    movz w11, #0xfeed
    movk w11, #0xd00d, lsl #16
    rev w10, w10
    cmp w10, w11
    b.ne boot_protocol_no_fdt
    ldr w12, [x0, #4]
    rev w12, w12
    ldr  x9, =g_fdt_blob_size
    str  x12, [x9]
    b    boot_protocol_fdt_ok
boot_protocol_no_fdt:
    ldr  x9, =g_fdt_blob_phys
    str  xzr, [x9]
    ldr  x9, =g_fdt_blob_size
    str  xzr, [x9]
boot_protocol_fdt_ok:
    /* Zero .bss and set up the stack exactly like _start does above --
     * same image, same layout, this is just a different starting
     * point into it. */
    ldr  x9, =_edata
    ldr  x10, =_end
1:  cmp  x9, x10
    b.hs 2f
    strb wzr, [x9], #1
    b    1b
2:
    ldr  x9, =_stack_top
    mov  sp, x9

    bl   kernel_boot_aarch64_boot_protocol
boot_protocol_hang:
    wfi
    b boot_protocol_hang

/* PSCI CPU_ON starts a secondary core at this entry in a byte-for-byte
 * copy of the BSP kernel image. x0 is the assigned region_base, not a DTB.
 * Only PC-relative instructions may execute here: the copy lives at a
 * different physical address from the BSP image linked at 0x41000000.
 * CORE_STACK_OFFSET + CORE_STACK_SIZE = 0x380000 (asserted in C). */
.global kernel_boot_ap
kernel_boot_ap:
    add x9, x0, #0x380, lsl #12
    mov sp, x9
    bl kernel_boot_ap_aarch64
ap_hang:
    wfi
    b ap_hang

.data
.align 3
/* Physical address of the DTB, as delivered in x0. Zero means "none" or
 * "invalid" -- the two are reported distinctly by the C side, which
 * re-validates with fdt_valid and prints what it found. */
.global g_fdt_blob_phys
 g_fdt_blob_phys:
    .quad 0
.global g_fdt_blob_size
 g_fdt_blob_size:
    .quad 0
/* The raw x0 at entry, whatever the loader meant by it. */
.global g_boot_x0
 g_boot_x0:
    .quad 0
/* The new boot protocol's own x1-x4, captured raw by boot_protocol_start
 * above -- see that symbol's own comment for what each one means. Zero
 * on every boot path that still enters at _start (GRUB's xnu loader,
 * the arm64 boot protocol's own -kernel path): nothing on those paths
 * ever writes these, and kernel_boot_aarch64 (not
 * kernel_boot_aarch64_boot_protocol) never reads them either. */
.global g_machine_description_phys
 g_machine_description_phys:
    .quad 0
.global g_init_description_phys
 g_init_description_phys:
    .quad 0
.global g_saski_boot_params_phys
 g_saski_boot_params_phys:
    .quad 0
.global g_init_entry_phys
 g_init_entry_phys:
    .quad 0

.align 4
stack_bottom:
    /* The reference init's BSP stack peaked at 3312 bytes in a sentinel
     * SMP4 boot (including securinit). Keep 16 KiB file-backed for every
     * loader path: nearly five times that measured depth. APs use their
     * separate region stacks, not this BSP reservation. */
    .space 16384
.global _stack_top
_stack_top:

/* The same build-selected init Mach-O embedded by the x86_64 boot.S.
 * No bootloader module list is needed on either architecture. */
.section .rodata
.balign 16
.global g_embedded_init_start
g_embedded_init_start:
.incbin INIT_MACHO_PATH
.global g_embedded_init_end
g_embedded_init_end:
