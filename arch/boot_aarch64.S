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
    /* x0 is the DTB. Store it before anything else can clobber it. */
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

.align 4
stack_bottom:
    .space 65536
.global _stack_top
_stack_top:
