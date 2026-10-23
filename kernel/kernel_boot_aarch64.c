#include "arch.h"
#include "cap.h"
#include "macho.h"
#include "../include/init_abi.h"
#include "../include/smp_layout.h"
#include "serial.h"
#include "fdt.h"
#include "xnu_boot_args.h"
#include <stdint.h>

/* aarch64 enters from a DTB (arm64 Image protocol) or XNU v2 boot args
 * (GRUB xnu_kernel64). Both feed the same init_boot_info and the same
 * build-selected Mach-O init; the kernel only supplies IPC and hands off. */

extern uint8_t g_embedded_init_start[];
extern uint8_t g_embedded_init_end[];
extern uint8_t _start[];
extern uint8_t _end[];
extern void kernel_boot_ap(uint64_t region_base); /* arch/boot_aarch64.S */
typedef void (*init_entry_t)(struct init_boot_info *);

_Static_assert(CORE_STACK_OFFSET + CORE_STACK_SIZE == 0x380000ull,
               "ARM AP assembly stack offset must match smp_layout.h");

/* Provided by arch/boot_aarch64.S: the DTB address captured from x0, and its
 * size, both zeroed by the stub if the blob failed validation. */
extern unsigned long long g_fdt_blob_phys;
extern unsigned long long g_fdt_blob_size;
/* The raw x0 at entry: a DTB (arm64 boot protocol: QEMU -kernel, the EFI
 * stub) or a struct xnu_boot_args_v2 * (GRUB's xnu_kernel64, the same
 * convention the x86_64 path boots by). */
extern unsigned long long g_boot_x0;

/* EFI_SYSTEM_TABLE (64-bit): Hdr(24), FirmwareVendor@24, FirmwareRevision@32
 * (+pad), ConsoleInHandle@40, ConIn@48, ConsoleOutHandle@56, ConOut@64,
 * StandardErrorHandle@72, StdErr@80, RuntimeServices@88, BootServices@96,
 * NumberOfTableEntries@104, ConfigurationTable@112; each entry is a 16-byte GUID then an 8-byte
 * VendorTable pointer. The FDT GUID is b1b621d5-f19c-41a5-830b-d9152c69aae0,
 * read here as two little-endian quadwords. Called after ExitBootServices
 * with the MMU off: firmware never called SetVirtualAddressMap for us, so
 * these pointers are physical. */
#define EFI_FDT_GUID_LO 0x41A5F19CB1B621D5ull
#define EFI_FDT_GUID_HI 0xE0AA692C15D90B83ull
/* EFI_ACPI_20_TABLE_GUID 8868e871-e4f1-11d3-bc22-0080c73c8881. AAVMF on a
 * standard (ACPI) platform publishes the RSDP here and deliberately hides
 * the device tree; the DT is published only when ACPI is off. Both are the
 * firmware's machine description: the kernel finds whichever exists and
 * hands it on, it does not parse ACPI itself. */
#define EFI_ACPI20_GUID_LO 0x11D3E4F18868E871ull
#define EFI_ACPI20_GUID_HI 0x81883CC7800022BCull

/* ACPI RSDP physical address, if the firmware described the machine with
 * ACPI rather than a device tree. 0 = none. Handed to init the same way
 * the x86_64 path hands early_rsdp_phys. */
unsigned long long g_rsdp_phys;

static uint64_t efi_find_table(uint64_t system_table, uint64_t guid_lo,
                               uint64_t guid_hi)
{
    if (system_table == 0) {
        return 0;
    }
    const uint8_t *st = (const uint8_t *)(uintptr_t)system_table;
    uint64_t n = *(const uint64_t *)(st + 104);
    const uint8_t *ct = (const uint8_t *)(uintptr_t)*(const uint64_t *)(st + 112);
    if (ct == 0 || n > 256) {
        kprintf("[kernel] EFI system table at 0x%X: implausible config table "
                "(%u entries at 0x%X)\n", (unsigned)system_table, (unsigned)n,
                (unsigned)(uintptr_t)ct);
        return 0;
    }
    for (uint64_t i = 0; i < n; i++) {
        const uint64_t *e = (const uint64_t *)(ct + i * 24);
        if (e[0] == guid_lo && e[1] == guid_hi) {
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
    uint64_t fdt = efi_find_table(bp->efi_system_table, EFI_FDT_GUID_LO,
                                  EFI_FDT_GUID_HI);
    if (fdt == 0) {
        g_rsdp_phys = efi_find_table(bp->efi_system_table, EFI_ACPI20_GUID_LO,
                                     EFI_ACPI20_GUID_HI);
        if (g_rsdp_phys != 0) {
            kprintf("[kernel] ACPI RSDP via EFI configuration table: 0x%X "
                    "(firmware describes this machine with ACPI, not a DTB)\n",
                    (unsigned)g_rsdp_phys);
        } else {
            kprintf("[kernel] EFI configuration table holds neither an FDT nor "
                    "an ACPI RSDP (efi_system_table=0x%X)\n",
                    (unsigned)bp->efi_system_table);
        }
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


    if (g_fdt_blob_phys == 0 && g_rsdp_phys != 0) {
        kprintf("[kernel] machine description: ACPI (RSDP 0x%X), no DTB\n",
                (unsigned)g_rsdp_phys);
    } else if (g_fdt_blob_phys == 0) {
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

    const uint64_t init_len = (uint64_t)(g_embedded_init_end - g_embedded_init_start);
    kprintf("[kernel] embedded init image: %u bytes\n", (unsigned)init_len);
    uint64_t entry = macho_load(g_embedded_init_start, 0);
    if (entry == 0) {
        kprintf("[kernel] PANIC: failed to Mach-O-load init\n");
        halt_forever();
    }
    kprintf("[kernel] init entry point: 0x%X\n", (unsigned)entry);

    static struct init_boot_info boot_info;
    boot_info.caps = kernel_cap_bootstrap();
    boot_info.kernel_invoke = kernel_invoke;
    boot_info.kernel_load_base = (uint64_t)(uintptr_t)_start;
    boot_info.pml4_phys = 0; /* ARM enters with the MMU off. */
    boot_info.init_image_addr = (uint64_t)(uintptr_t)g_embedded_init_start;
    boot_info.init_image_len = init_len;
    uint64_t image_len = (uint64_t)(_end - _start);
    uint64_t init_footprint = macho_image_size(g_embedded_init_start);
    if (image_len <= CORE_KERNEL_MAX_SIZE && init_footprint != 0 &&
        init_footprint <= CORE_INIT_MAX_SIZE) {
        boot_info.kernel_boot_ap_addr = (uint64_t)(uintptr_t)kernel_boot_ap;
        boot_info.kernel_image_addr = (uint64_t)(uintptr_t)_start;
        boot_info.kernel_image_len = image_len;
        kprintf("[kernel] AP image: %u bytes, init footprint %u bytes, entry 0x%X\n",
                (unsigned)image_len, (unsigned)init_footprint,
                (unsigned)boot_info.kernel_boot_ap_addr);
    } else {
        kprintf("[kernel] AP images do not fit assigned region "
                "(kernel %u/%u, init %u/%u bytes); SMP disabled\n",
                (unsigned)image_len, (unsigned)CORE_KERNEL_MAX_SIZE,
                (unsigned)init_footprint, (unsigned)CORE_INIT_MAX_SIZE);
    }
    boot_info.early_rsdp_phys = g_rsdp_phys;
    boot_info.fdt_blob_phys = g_fdt_blob_phys;
    if (g_boot_x0 != 0 && g_fdt_blob_phys == 0) {
        const struct xnu_boot_args_v2 *bp =
            (const struct xnu_boot_args_v2 *)(uintptr_t)g_boot_x0;
        if (bp->vermajor == 2) {
            boot_info.ram_size = bp->ram_size;
        }
    }
    if (boot_info.ram_size == 0 && g_fdt_blob_phys != 0 &&
        fdt_valid((const void *)(uintptr_t)g_fdt_blob_phys, g_fdt_blob_size)) {
        fdt_range_t ranges[8];
        size_t count = fdt_memory_ranges((const void *)(uintptr_t)g_fdt_blob_phys,
                                         ranges, 8);
        for (size_t i = 0; i < count; i++) {
            boot_info.ram_size += ranges[i].size;
        }
    }

    kprintf("[kernel] init boot info: fdt 0x%X, rsdp 0x%X, ram %u MiB\n",
            (unsigned)boot_info.fdt_blob_phys,
            (unsigned)boot_info.early_rsdp_phys,
            (unsigned)(boot_info.ram_size / (1024 * 1024)));
    kprintf("[kernel] handing off to init -- kernel does not run again after this\n");
    ((init_entry_t)(uintptr_t)entry)(&boot_info);
    kprintf("[kernel] PANIC: init returned\n");
    halt_forever();
}

/* Reached via PSCI CPU_ON in a fresh, byte-for-byte copy of this kernel.
 * arch/boot_aarch64.S sets this AP's stack from the region before C runs.
 * All references here must be PC-relative: the copied image is not at the
 * BSP's link address, and nobody processes relocation records for it. */
void kernel_boot_ap_aarch64(uint64_t region_base)
{
    serial_init();
    kprintf("[kernel] AP core booting at region 0x%X\n",
            (unsigned)region_base);

    /* Init's Mach-O declares where its BSP copy lives. Apply exactly the
     * slide between that address and the init slot in this assigned region;
     * target_base is a displacement, never another absolute vmaddr. */
    uint64_t slide = region_base + CORE_INIT_OFFSET - BSP_INIT_LOAD_BASE;
    uint64_t entry = macho_load(g_embedded_init_start, slide);
    if (entry == 0) {
        kprintf("[kernel] AP PANIC: failed to Mach-O-load init\n");
        halt_forever();
    }

    const uint64_t *discovery =
        (const uint64_t *)(uintptr_t)(region_base + CORE_DISCOVERY_OFFSET);
    static struct init_boot_info boot_info;
    boot_info.caps = kernel_cap_bootstrap();
    boot_info.kernel_invoke = kernel_invoke;
    boot_info.kernel_load_base = KERNEL_LOAD_BASE;
    boot_info.kernel_boot_ap_addr = (uint64_t)(uintptr_t)kernel_boot_ap;
    boot_info.pml4_phys = 0;
    /* AP init does not orchestrate another layer of AP bring-up. */
    boot_info.init_image_addr = 0;
    boot_info.init_image_len = 0;
    boot_info.kernel_image_addr = 0;
    boot_info.kernel_image_len = 0;
    boot_info.bsp_kernel_invoke_addr =
        discovery[CORE_DISCOVERY_BSP_INVOKE_OFF / 8];
    boot_info.mutex_manager_handle = discovery[CORE_DISCOVERY_MUTEX_MGR_OFF / 8];
    boot_info.memory_manager_handle = discovery[CORE_DISCOVERY_MEM_MGR_OFF / 8];
    boot_info.hardware_mutex_id = discovery[CORE_DISCOVERY_HW_MUTEX_ID_OFF / 8];
    boot_info.lapic_calibration_count =
        discovery[CORE_DISCOVERY_LAPIC_CALIBRATION_OFF / 8];
    boot_info.cluster_scheduler_handle =
        discovery[CORE_DISCOVERY_CLUSTER_SCHED_OFF / 8];
    boot_info.serial_resource_handle =
        discovery[CORE_DISCOVERY_SERIAL_RESOURCE_OFF / 8];
    boot_info.early_rsdp_phys = discovery[CORE_DISCOVERY_RSDP_OFF / 8];
    /* The BSP's physical DTB address is in the copied kernel's initialized
     * data (g_fdt_blob_phys). Firmware doesn't provide x0=DTB to an AP. */
    boot_info.fdt_blob_phys = g_fdt_blob_phys;
    boot_info.own_mailbox_base = region_base + CORE_MAILBOX_OFFSET;

    kprintf("[kernel] AP handing off to its own init copy at 0x%X\n",
            (unsigned)entry);
    ((init_entry_t)(uintptr_t)entry)(&boot_info);
    kprintf("[kernel] AP PANIC: init returned\n");
    halt_forever();
}
