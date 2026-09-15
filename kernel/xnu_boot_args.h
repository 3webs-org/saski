#pragma once

#include <stdint.h>

/* Mirrors GRUB's own struct grub_xnu_boot_params_v2 (grub-core's
 * include/grub/i386/xnu.h) field-for-field, including field order --
 * this has to match exactly, since it's read directly out of memory
 * GRUB itself wrote, not something this project controls the layout
 * of. See kernel/boot.S's own _g_xnu_boot_args_addr comment for how
 * the physical address of one of these reaches this kernel at all
 * (EAX at entry, GRUB's own real XNU boot protocol), and
 * g_xnu_version_string for why GRUB emits this richer v2 shape
 * instead of the older v1 (which lacks ram_size and the EFI memory
 * map entirely). GRUB_PACKED there is __attribute__((packed)) here --
 * both mean "no compiler-inserted padding, lay out fields exactly as
 * written", which is why the field order below must match GRUB's own
 * struct exactly rather than being reordered for C convenience. */

struct xnu_boot_args_common {
    uint8_t cmdline[1024];

    /* Same shape as EFI's own GetMemoryMap() output -- efi_mmap points
     * at efi_mmap_size bytes of grub_efi_memory_descriptor_t entries,
     * each efi_mem_desc_size bytes apart (NOT necessarily
     * sizeof(descriptor) -- EFI's own convention allows the descriptor
     * to grow, so a consumer must always stride by this field, never
     * assume a fixed struct size). This is the real, current EFI
     * memory map GRUB itself obtained via GetMemoryMap() before exiting
     * boot services -- exactly the kind of real memory-map discovery
     * this project's own smp_layout.h admits it doesn't have yet. */
    uint32_t efi_mmap;
    uint32_t efi_mmap_size;
    uint32_t efi_mem_desc_size;
    uint32_t efi_mem_desc_version;

    uint32_t lfb_base;
    uint32_t lfb_mode;
    uint32_t lfb_line_len;
    uint32_t lfb_width;
    uint32_t lfb_height;
    uint32_t lfb_depth;

    /* Pointer to XNU's own proprietary device tree (NOT an FDT/DTB --
     * a different, older format specific to XNU's own boot protocol;
     * see grub-core/loader/xnu.c's own grub_xnu_writetree_toheap_real
     * for its actual shape) and its length in bytes. */
    uint32_t devtree;
    uint32_t devtreelen;

    uint32_t heap_start;
    uint32_t heap_size;
    uint32_t efi_runtime_first_page;
    uint32_t efi_runtime_npages;
} __attribute__((packed));

struct xnu_boot_args_v2 {
    uint16_t verminor;
    uint16_t vermajor; /* 2 for this struct shape; check before
                        * trusting anything below common, in case GRUB
                        * ever emitted v1 instead (see
                        * g_xnu_version_string's own comment for when
                        * that would happen). */
    uint8_t efi_uintnbits;
    uint8_t unused0[3];

    struct xnu_boot_args_common common;

    uint64_t efi_runtime_first_page_virtual;
    uint32_t efi_system_table; /* physical address of the EFI system
                                * table -- a real, GRUB-provided path to
                                * ACPI's RSDP via its own configuration
                                * tables, potentially a sturdier
                                * alternative to boot.S's own
                                * scan_for_rsdp32 workaround. Not yet
                                * used for that; noted for later. */
    uint32_t unused1[9];
    uint64_t ram_size; /* GRUB's own total_ram_hook sum across every
                        * GRUB_MEMORY_AVAILABLE range in its own memory
                        * map -- real, discovered RAM size, not this
                        * project's own guess. */
    uint64_t fsbfreq;
    uint32_t unused2[734];
} __attribute__((packed));

_Static_assert(sizeof(struct xnu_boot_args_v2) == 4096,
               "xnu_boot_args_v2 must match GRUB's own layout exactly "
               "(one page, per GRUB_XNU_PAGESIZE) -- a mismatch here "
               "means a field was misordered or mistyped relative to "
               "GRUB's own struct grub_xnu_boot_params_v2, and every "
               "offset after the mistake would silently read the wrong "
               "bytes");
