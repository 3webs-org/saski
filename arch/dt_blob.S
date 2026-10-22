/* SPDX-License-Identifier: 0BSD */
/*
 * The build-time device tree, embedded into the kernel image as data.
 *
 * This file is compiled ONLY for targets that boot from a device tree. It is
 * not in the x86_64 compile list and never will be: that platform is described
 * by ACPI, has no use for this blob, and carrying it would be dead weight in
 * an image whose whole design point is that it contains nothing it does not
 * use. Making the embedding opt-in at the build level, rather than present and
 * unused everywhere, is what keeps the existing x86 build byte-for-byte what it
 * was.
 *
 * WHY THIS IS A SEPARATE ASSEMBLY FILE rather than an array initialiser in C:
 * .incbin takes a file at assembly time, so the blob is never turned into a
 * C array -- no per-byte initialiser, no recompiling a multi-kilobyte literal
 * whenever the tree changes, and the bytes stay read-only data exactly as dtc
 * emitted them. boot.S already embeds the init image this way for the same
 * reason; this is the same mechanism applied to a second input.
 *
 * The path is supplied by the build (DTB_PATH), never hardcoded here, so the
 * blob this describes is always the one the build actually produced. A stale
 * blob silently describing a different machine is exactly the bug that would
 * be hardest to see from a serial log.
 */

.data
.align 3
.global _g_embedded_dtb_start
_g_embedded_dtb_start:
.incbin DTB_PATH
.global _g_embedded_dtb_end
_g_embedded_dtb_end:

/* The length is derived here rather than computed at runtime from the blob's
 * own header: a caller that has to trust the blob to tell it how big the blob
 * is cannot bounds-check anything, and this is untrusted-input-adjacent data.
 * Two labels and a subtraction give the reader a size it can rely on. */
.data
.align 3
.global _g_embedded_dtb_size
_g_embedded_dtb_size:
.quad _g_embedded_dtb_end - _g_embedded_dtb_start
