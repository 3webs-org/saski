#include "arch.h"
#include "serial.h"
#include "fdt.h"
#include "xnu_boot_args.h"
#include <stdint.h>

/* The aarch64 kernel's own boot path.
 *
 * Deliberately NOT a copy of kernel_boot: that function is built around the
 * xnu boot_args structure and ACPI discovery, neither of which exists on this
 * architecture. What an aarch64 boot actually has is the device tree, whose
 * address boot_aarch64.S captured from x0 and validated before C ran.
 *
 * Scope, stated plainly: this is the bring-up path, not a finished port. It
 * initialises the console, validates and reports the device tree, and halts.
 * Everything the x86 path does after its own console init -- capability
 * bootstrap, macho_load of the embedded init, the handoff -- is future work
 * gated on the aarch64 coreinit backend, which does not exist yet. Saying so
 * here rather than implying otherwise by silence: the serial output this
 * produces is the proof that the boot path works, and it stops there. */

/* Provided by arch/boot_aarch64.S: the DTB address captured from x0, and its
 * size, both zeroed by the stub if the blob failed validation. */
extern unsigned long long g_fdt_blob_phys;
extern unsigned long long g_fdt_blob_size;
/* The raw x0 at entry: a DTB (arm64 boot protocol: QEMU -kernel, the EFI
 * stub) or a struct xnu_boot_args_v2 * (GRUB's xnu_kernel64, the same
 * convention the x86_64 path boots by). */
extern unsigned long long g_boot_x0;

/* EFI_SYSTEM_TABLE (64-bit): NumberOfTableEntries at +96,
 * ConfigurationTable at +104; each entry is a 16-byte GUID then an 8-byte
 * VendorTable pointer. The FDT GUID is b1b621d5-f19c-41a5-830b-d9ea2ee3146c,
 * read here as two little-endian quadwords. Called after ExitBootServices
 * with the MMU off: firmware never called SetVirtualAddressMap for us, so
 * these pointers are physical. */
#define EFI_FDT_GUID_LO 0x41A5F19CB1B621D5ull
#define EFI_FDT_GUID_HI 0x6C14E32EEAD90B83ull

static uint64_t efi_find_fdt(uint64_t system_table)
{
    if (system_table == 0) {
        return 0;
    }
    const uint8_t *st = (const uint8_t *)(uintptr_t)system_table;
    uint64_t n = *(const uint64_t *)(st + 96);
    const uint8_t *ct = (const uint8_t *)(uintptr_t)*(const uint64_t *)(st + 104);
    if (ct == 0 || n > 256) {
        return 0; /* not a plausible configuration table */
    }
    for (uint64_t i = 0; i < n; i++) {
        const uint64_t *e = (const uint64_t *)(ct + i * 24);
        if (e[0] == EFI_FDT_GUID_LO && e[1] == EFI_FDT_GUID_HI) {
            return e[2];
        }
    }
    return 0;
}

/* Big-endian 32-bit read: the FDT header's own byte order. */
static uint32_t be32_at(uint64_t addr)
{
    const uint8_t *b = (const uint8_t *)(uintptr_t)addr;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

/* x0 was not a DTB. If it is GRUB's xnu boot-parameter block, find the
 * device tree the platform's way -- through the EFI configuration table the
 * block points at -- and adopt it exactly as if it had arrived in x0. */
static void resolve_xnu_boot_params(void)
{
    if (g_fdt_blob_phys != 0 || g_boot_x0 == 0) {
        return;
    }
    const struct xnu_boot_args_v2 *bp =
        (const struct xnu_boot_args_v2 *)(uintptr_t)g_boot_x0;
    if (bp->vermajor != 2) {
        return;
    }
    kprintf("[kernel] x0 = xnu boot params v2 (GRUB xnu_kernel64)\n");
    uint64_t fdt = efi_find_fdt(bp->efi_system_table);
    if (fdt == 0) {
        kprintf("[kernel] xnu boot params carry no EFI system table with an "
                "FDT (efi_system_table=0x%X)\n", (unsigned)bp->efi_system_table);
        return;
    }
    if (be32_at(fdt) != 0xd00dfeedu) {
        kprintf("[kernel] EFI config-table FDT at 0x%X has bad magic\n",
                (unsigned)fdt);
        return;
    }
    g_fdt_blob_phys = fdt;
    g_fdt_blob_size = be32_at(fdt + 4);
    kprintf("[kernel] DTB found via EFI configuration table\n");
}

void kernel_boot_aarch64(void)
{
    serial_init();

    kprintf("[kernel] aarch64 boot\n");
    resolve_xnu_boot_params();



    if (g_fdt_blob_phys == 0) {
        kprintf("[kernel] no DTB: the boot stub found no valid device tree\n");
    } else {
        /* The stub already checked the magic; fdt_valid re-checks the block
         * offsets against the size, which is the part the stub cannot do in
         * four instructions. A blob that passes both is safe to walk. */
        if (fdt_valid((const void *)(uintptr_t)g_fdt_blob_phys,
                      g_fdt_blob_size)) {
            kprintf("[kernel] DTB at 0x%X, %u bytes, valid\n",
                    (unsigned)g_fdt_blob_phys, (unsigned)g_fdt_blob_size);

            fdt_range_t mem[8];
            size_t n = fdt_memory_ranges((const void *)(uintptr_t)g_fdt_blob_phys,
                                         mem, 8);
            for (size_t i = 0; i < n; i++) {
                kprintf("[kernel]   memory: base 0x%X size 0x%X\n",
                        (unsigned)(mem[i].base >> 32),
                        (unsigned)(mem[i].base & 0xFFFFFFFF));
                kprintf("[kernel]           (low 0x%X, size 0x%X)\n",
                        (unsigned)(mem[i].base & 0xFFFFFFFF),
                        (unsigned)mem[i].size);
            }
            if (n == 0) {
                kprintf("[kernel]   memory: none declared\n");
            }
        } else {
            kprintf("[kernel] DTB at 0x%X failed full validation\n",
                    (unsigned)g_fdt_blob_phys);
        }
    }

    kprintf("[kernel] aarch64 bring-up complete; halting\n");
    halt_forever();
}
