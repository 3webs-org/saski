#include "arch_x86_64.h"
#include "cap.h"
#include "macho.h"
#include "mode.h"
#include "serial.h"
#include "xnu_boot_args.h"
#include "../include/init_abi.h"
#include "../include/smp_layout.h"
#include <stdint.h>

extern uint8_t boot_pml4[]; /* boot.S's own identity-map root */

typedef void (*init_entry_t)(struct init_boot_info *info);

extern uint32_t g_xnu_boot_args_addr; /* boot.S's own capture of EAX at
                                       * entry -- see that symbol's own
                                       * comment; physical address of a
                                       * struct xnu_boot_args_v2, or 0 */

/* Reads ram_size out of GRUB's own boot_args, if it's actually there.
 * Returns 0 (never asserts/panics) if boot_args wasn't captured, or
 * wasn't the v2 shape this expects -- e.g. an older GRUB, or the
 * "Darwin Kernel Version" string scan (see g_xnu_version_string)
 * somehow missing. Callers already treat 0 as "unknown, fall back",
 * never "confirmed zero RAM". */
static uint64_t xnu_boot_args_ram_size(void)
{
    if (g_xnu_boot_args_addr == 0) {
        return 0;
    }
    const struct xnu_boot_args_v2 *args =
        (const struct xnu_boot_args_v2 *)(uintptr_t)g_xnu_boot_args_addr;
    if (args->vermajor != 2) {
        return 0;
    }
    return args->ram_size;
}

/* Real UEFI memory descriptor layout (UEFI spec, EFI_MEMORY_DESCRIPTOR)
 * -- Type(u32) is followed by 4 bytes of compiler-inserted padding
 * before PhysicalStart, since the u64 fields need 8-byte alignment;
 * every source confirms this same layout independent of vendor. This
 * struct is deliberately NOT used as a stride unit when walking the
 * map below -- see that comment for why: the UEFI spec explicitly
 * permits a DescriptorSize larger than sizeof(this struct), for
 * forward compatibility, and mandates striding by the returned
 * DescriptorSize (here, args->common.efi_mem_desc_size) instead. This
 * struct exists only to give the fields at the START of each
 * descriptor a name; anything past Attribute in a given descriptor,
 * if the stride is larger, is simply never read. */
struct efi_memory_descriptor {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} __attribute__((packed));

/* Real UEFI memory types (UEFI spec, EFI_MEMORY_TYPE) this cares
 * about -- boot services have already been exited by the time GRUB
 * hands off to this kernel (grub_xnu_boot's own
 * grub_autoefi_finish_boot_services, called before the jump), so
 * EfiBootServicesCode/Data are exactly as reusable as
 * EfiConventionalMemory at this point; this matches how other real
 * EFI-aware kernels (e.g. Linux's own EFI stub) treat them. */
#define EFI_LOADER_CODE 1
#define EFI_LOADER_DATA 2
#define EFI_BOOT_SERVICES_CODE 3
#define EFI_BOOT_SERVICES_DATA 4
#define EFI_CONVENTIONAL_MEMORY 7

static int efi_type_is_free(uint32_t type)
{
    return type == EFI_CONVENTIONAL_MEMORY ||
           type == EFI_BOOT_SERVICES_CODE ||
           type == EFI_BOOT_SERVICES_DATA;
}

/* Walks GRUB's own real EFI memory map (from boot_args -- see
 * xnu_boot_args_ram_size's own comment for where this structure comes
 * from) and finds the largest free range starting at or above
 * min_addr. Returns 1 and fills *out_base and *out_size if one was
 * found, 0 otherwise (an empty/absent map, or nothing free above
 * min_addr -- both checked explicitly, never assumed present).
 * min_addr exists so callers can stay clear of a region they already
 * know is occupied
 * (e.g. this kernel's own image) without this function needing to
 * know about that itself. */
static int xnu_boot_args_find_largest_free(uint64_t min_addr,
                                           uint64_t *out_base,
                                           uint64_t *out_size)
{
    if (g_xnu_boot_args_addr == 0) {
        return 0;
    }
    const struct xnu_boot_args_v2 *args =
        (const struct xnu_boot_args_v2 *)(uintptr_t)g_xnu_boot_args_addr;
    if (args->vermajor != 2) {
        return 0;
    }
    if (args->common.efi_mmap == 0 || args->common.efi_mem_desc_size == 0) {
        return 0;
    }

    const uint8_t *base = (const uint8_t *)(uintptr_t)args->common.efi_mmap;
    uint32_t stride = args->common.efi_mem_desc_size;
    uint32_t count = args->common.efi_mmap_size / stride;

    uint64_t best_base = 0, best_size = 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct efi_memory_descriptor *desc =
            (const struct efi_memory_descriptor *)(base + (uint64_t)i * stride);
        if (!efi_type_is_free(desc->type)) {
            continue;
        }
        uint64_t region_base = desc->physical_start;
        uint64_t region_size = desc->number_of_pages * 4096ull;
        if (region_base < min_addr) {
            uint64_t clip = min_addr - region_base;
            if (clip >= region_size) {
                continue;
            }
            region_base = min_addr;
            region_size -= clip;
        }
        if (region_size > best_size) {
            best_size = region_size;
            best_base = region_base;
        }
    }

    if (best_size == 0) {
        return 0;
    }
    *out_base = best_base;
    *out_size = best_size;
    return 1;
}

/* Real UEFI system/configuration table layout (UEFI spec) -- native
 * 64-bit field widths throughout (OVMF and GRUB itself are both
 * x86_64 UEFI; the firmware's own struct layout doesn't shrink just
 * because it hands its payload off in 32-bit mode -- see
 * efi_system_table_64's own comment for how this was confirmed and
 * why an earlier, 32-bit-field version of this struct was wrong).
 * Used to find ACPI's RSDP via a real, spec-defined lookup instead of
 * blindly scanning memory for a signature -- see
 * xnu_boot_args_find_rsdp's own comment for the mechanism. */
struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
} __attribute__((packed));

struct efi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
} __attribute__((packed));

/* EFI_CONFIGURATION_TABLE and EFI_SYSTEM_TABLE, with their REAL,
 * native field widths -- 64-bit pointers/UINTN throughout, naturally
 * aligned (no packed attribute), matching how OVMF (and GRUB itself,
 * an x86_64-efi build) actually laid these out in memory. Confirmed
 * empirically this session: a first version of this struct assumed
 * 32-bit pointer fields, matching this boot path's own 32-bit *address
 * space* -- but the firmware that built these structures is x86_64
 * UEFI regardless of what mode it hands its payload off in, so its own
 * struct layout never shrinks. Caught via Hdr.HeaderSize (which gives
 * the real size of the whole table, not just the header): read as
 * 0x78 (120 bytes) against a 72-byte 32-bit-field guess -- the exact
 * 48-byte gap this corrected layout accounts for (12 pointer-sized
 * fields after Hdr, each 4 bytes wider than assumed). The pointer
 * VALUES themselves still only ever hold addresses that fit in 32
 * bits on this VM -- read as uint64_t here because that's the real
 * field width, truncated to uint32_t by every caller once translated,
 * since everything downstream of this boot path stays 32-bit. */
struct efi_configuration_table {
    struct efi_guid vendor_guid;
    uint64_t vendor_table;
};

struct efi_system_table_64 {
    struct efi_table_header hdr;
    uint64_t firmware_vendor;
    uint32_t firmware_revision;
    uint32_t _pad0;
    uint64_t console_in_handle;
    uint64_t con_in;
    uint64_t console_out_handle;
    uint64_t con_out;
    uint64_t standard_error_handle;
    uint64_t std_err;
    uint64_t runtime_services;
    uint64_t boot_services;
    uint64_t number_of_table_entries;
    uint64_t configuration_table;
};
_Static_assert(sizeof(struct efi_system_table_64) == 120,
              "efi_system_table_64 must be 120 bytes (0x78), matching "
              "this table's own real Hdr.HeaderSize, confirmed "
              "empirically -- a mismatch here means a field's width or "
              "the struct's alignment assumption is wrong again");

static int efi_guid_eq(const struct efi_guid *g, uint32_t d1, uint16_t d2,
                       uint16_t d3, uint8_t d4_0, uint8_t d4_1, uint8_t d4_2,
                       uint8_t d4_3, uint8_t d4_4, uint8_t d4_5, uint8_t d4_6,
                       uint8_t d4_7)
{
    return g->data1 == d1 && g->data2 == d2 && g->data3 == d3 &&
           g->data4[0] == d4_0 && g->data4[1] == d4_1 &&
           g->data4[2] == d4_2 && g->data4[3] == d4_3 &&
           g->data4[4] == d4_4 && g->data4[5] == d4_5 &&
           g->data4[6] == d4_6 && g->data4[7] == d4_7;
}

/* Translates a possibly-remapped EFI "virtual" address back to the
 * physical address it actually corresponds to, using GRUB's own EFI
 * memory map. Necessary because GRUB's grub_xnu_boot calls the real
 * EFI SetVirtualAddressMap() runtime service and hands XNU's boot_args
 * addresses (efi_system_table here) expressed in THAT remapped scheme
 * -- standard UEFI convention for any OS that wants continued access
 * to runtime services after boot, confirmed empirically this session
 * by walking the actual memory map: efi_system_table (0x1206018 in one
 * observed run) fell inside an EfiRuntimeServicesData descriptor whose
 * virtual_start (0x1107000) differed from its physical_start
 * (0x1F4ED000) -- reading 0x1206018 directly, under this kernel's own
 * plain identity map, hit unbacked memory (0xFFFFFFFF) and would have
 * walked off into an unmapped page and hung, since nothing here builds
 * the matching virtual mapping GRUB expects an EFI-runtime-aware OS to
 * build. Translating through the map instead of replicating that
 * mapping is deliberate: this kernel has no ongoing use for EFI
 * runtime services themselves (no reboot/settime/variable calls
 * planned), only for the addresses reachable through them at this one
 * moment, during boot -- so a one-time address translation is the
 * right amount of work, not a fixed set of extra page-table entries
 * that would need to persist for a capability nothing here exercises.
 * Returns 0 if virt isn't found in any EFI memory map descriptor
 * (which includes the un-remapped case where virtual_start ==
 * physical_start already -- returning virt unchanged, correctly, since
 * that arithmetic is an identity transform either way). */
static uint32_t xnu_efi_translate_virt(const struct xnu_boot_args_v2 *args,
                                       uint32_t virt)
{
    if (args->common.efi_mmap == 0 || args->common.efi_mem_desc_size == 0) {
        return 0;
    }
    const uint8_t *base = (const uint8_t *)(uintptr_t)args->common.efi_mmap;
    uint32_t stride = args->common.efi_mem_desc_size;
    uint32_t count = args->common.efi_mmap_size / stride;

    for (uint32_t i = 0; i < count; i++) {
        const struct efi_memory_descriptor *d =
            (const struct efi_memory_descriptor *)(base + (uint64_t)i * stride);
        /* Only descriptors GRUB actually remapped are relevant here --
         * every other descriptor (Conventional Memory, ACPI, etc.) has
         * virtual_start == physical_start, meaning its "virtual" range
         * IS its physical range. Checking those too would match
         * whichever one's PHYSICAL range happens to numerically
         * contain virt -- a real bug found this session: with this
         * check absent, a Conventional Memory descriptor's own
         * physical range matched before the actual remapped
         * RuntimeServicesData descriptor ever got considered, silently
         * returning virt unchanged instead of translating it. */
        if (d->virtual_start == d->physical_start) {
            continue;
        }
        /* Mask off the high-half kernel prefix (observed as
         * 0xFFFFFF80_00000000, x86-64's own canonical-form convention
         * for a kernel-space virtual address -- the same shape Linux's
         * own -2GB kernel base uses) that GRUB applies to
         * virtual_start whenever grub_xnu_is_64bit is set (see
         * grub-core/loader/i386/xnu.c's own grub_xnu_boot: `if
         * (SIZEOF_OF_UINTN == 8 && grub_xnu_is_64bit) curdesc->
         * virtual_start |= 0xffffff8000000000ULL;`). That flag tracks
         * which GRUB *command* loaded the kernel (xnu_kernel64, in
         * this project's case) -- a statement about the boot
         * protocol's intended shape, not the actual CPU mode at
         * handoff, which stays 32-bit regardless (see boot.S's own
         * top-of-file comment). Comparing a real 64-bit XNU kernel's
         * own canonical-form pointer against this would need no mask
         * at all; comparing our own plain 32-bit virt against it
         * without masking silently fails every range check, found the
         * hard way this session by tracing the comparison's own inputs
         * down to this exact bit pattern. Everything in this boot
         * path -- our own addresses, and everything we read out of
         * boot_args -- is genuinely 32-bit, so the low 32 bits are the
         * entirety of what's meaningful here. */
        uint64_t vstart = d->virtual_start & 0xFFFFFFFFull;
        uint64_t size = d->number_of_pages * 4096ull;
        if ((uint64_t)virt >= vstart && (uint64_t)virt < vstart + size) {
            return (uint32_t)(d->physical_start + ((uint64_t)virt - vstart));
        }
    }
    /* Not found among remapped descriptors -- presumably already a
     * physical address (never remapped in the first place, e.g. an
     * ACPI-region pointer, which SetVirtualAddressMap never touches).
     * Returned unchanged rather than treated as an error; the caller's
     * own use of the result still gets validated by whatever reads it
     * next (a signature check, a checksum, capnp shape validation --
     * this project's own consistent pattern of validating data rather
     * than trusting a source unconditionally). */
    return virt;
}

/* Finds ACPI's RSDP via GRUB's own boot_args -> EFI system table ->
 * configuration table array -- a real, spec-defined lookup (the exact
 * mechanism a real EFI-aware OS uses), rather than scanning physical
 * memory for a signature within some fixed window and hoping it's wide
 * enough (this project's original approach, in boot.S's own
 * scan_for_rsdp32, now removed entirely -- that window was tuned
 * against wherever OVMF happened to place the RSDP with this project's
 * own default test RAM size, and genuinely broke the moment RAM size
 * changed: exactly the class of fixed-layout assumption this session
 * has been removing elsewhere).
 * Every pointer read out of EFI structures here goes through
 * xnu_efi_translate_virt first -- see that function's own comment for
 * why that step is necessary at all, found the hard way when this
 * function's first version hung the kernel dereferencing
 * efi_system_table directly. No plain-memory-scan fallback: this
 * project only supports the v2 boot_args shape, which always carries
 * a real efi_system_table on this project's EFI-only boot path (see
 * kernel_boot's own call site for why that's a safe assumption here,
 * not just an unhandled gap). Prefers the ACPI 2.0 GUID over 1.0 if
 * both are present, matching every other real ACPI-consuming OS's own
 * convention. Returns 0 if boot_args is absent/not v2, efi_system_table
 * is 0 or untranslatable, or neither
 * GUID is present in the configuration table. */
static uint32_t xnu_boot_args_find_rsdp(void)
{
    if (g_xnu_boot_args_addr == 0) {
        return 0;
    }
    const struct xnu_boot_args_v2 *args =
        (const struct xnu_boot_args_v2 *)(uintptr_t)g_xnu_boot_args_addr;
    if (args->vermajor != 2 || args->efi_system_table == 0) {
        return 0;
    }

    uint32_t sys_phys = xnu_efi_translate_virt(args, args->efi_system_table);
    if (sys_phys == 0) {
        return 0;
    }
    const struct efi_system_table_64 *sys =
        (const struct efi_system_table_64 *)(uintptr_t)sys_phys;
    /* EFI_SYSTEM_TABLE_SIGNATURE ("IBI SYST" packed little-endian) --
     * validates sys_phys actually landed on a real system table before
     * trusting number_of_table_entries/configuration_table at all.
     * Necessary because xnu_efi_translate_virt's own "not found, return
     * unchanged" fallback (see that function's own comment) is a
     * reasonable guess, not a proof -- this is the validation that
     * guess still needs, matching this project's consistent pattern of
     * checking data rather than trusting a source unconditionally
     * (the RSDP found below still gets its own real checksum check in
     * coreinit's acpi.c, same reasoning, one layer further out). */
    if (sys->hdr.signature != 0x5453595320494249ull) {
        return 0;
    }
    if (sys->configuration_table == 0) {
        return 0;
    }

    /* configuration_table is the real, 64-bit field value (see
     * efi_system_table_64's own comment) -- truncated to uint32_t here
     * because xnu_efi_translate_virt, and every address downstream of
     * it, is 32-bit throughout this boot path; every real address
     * observed fits comfortably. */
    uint32_t tables_phys =
        xnu_efi_translate_virt(args, (uint32_t)sys->configuration_table);
    if (tables_phys == 0) {
        /* Not found anywhere in the map -- this address genuinely
         * isn't verified safe to dereference (translate_virt already
         * returns the correct identity result, unchanged, for any
         * address covered by a descriptor with virtual_start ==
         * physical_start, which is the common case for the
         * configuration table array -- see that function's own
         * comment). Failing closed here, not guessing, is the point:
         * the whole reason this function exists is an earlier version
         * hanging the kernel on exactly this kind of unverified
         * pointer. */
        return 0;
    }
    const struct efi_configuration_table *tables =
        (const struct efi_configuration_table *)(uintptr_t)tables_phys;

    uint32_t acpi1_addr = 0;
    for (uint64_t i = 0; i < sys->number_of_table_entries; i++) {
        const struct efi_guid *g = &tables[i].vendor_guid;
        /* ACPI 2.0: 8868e871-e4f1-11d3-bc22-0080c73c8881 */
        if (efi_guid_eq(g, 0x8868e871, 0xe4f1, 0x11d3, 0xbc, 0x22, 0x00,
                        0x80, 0xc7, 0x3c, 0x88, 0x81)) {
            return xnu_efi_translate_virt(args,
                                          (uint32_t)tables[i].vendor_table);
        }
        /* ACPI 1.0: eb9d2d30-2d88-11d3-9a16-0090273fc14d -- kept as a
         * fallback candidate, only used if 2.0 is never found across
         * the whole table (a real firmware may list both). */
        if (efi_guid_eq(g, 0xeb9d2d30, 0x2d88, 0x11d3, 0x9a, 0x16, 0x00,
                        0x90, 0x27, 0x3f, 0xc1, 0x4d)) {
            acpi1_addr = (uint32_t)tables[i].vendor_table;
        }
    }
    return acpi1_addr ? xnu_efi_translate_virt(args, acpi1_addr) : 0;
}

void kernel_boot_ap(uint64_t region_base);

/* Set once, by the BSP's own kernel_boot, and never touched again by
 * this specific running copy -- but since kernel_boot_ap runs in a
 * GENUINELY SEPARATE copy of this whole kernel image (code and data
 * together, copied by init's own SMP bring-up code), the value copied
 * into THAT instance's own copy of this same global is exactly the
 * BSP's original value. This is how an AP's kernel_boot_ap knows where
 * init's original, unparsed file bytes are without needing any
 * extra trampoline parameter: it falls straight out of copying the
 * whole image rather than just code. */
static uint64_t g_init_image_addr = 0;

/* This kernel's entry point -- see boot.S for the assembly-level
 * handoff. Despite the command's name, GRUB's xnu_kernel64 does NOT
 * hand off in 64-bit long mode -- confirmed empirically via GDB
 * (EFER.LME clear, a 32-bit CS at entry) -- so boot.S performs its own
 * 32-to-64-bit transition first (identity map, PAE, EFER.LME, paging,
 * a 64-bit GDT, a far jump) before this function is ever reached.
 *
 * There is no multiboot info here at all -- there is no magic, no
 * mbi, no module list. The userspace init image instead travels WITH
 * this kernel, embedded directly into its own binary by boot.S's own
 * .incbin (g_embedded_init_start/_end, below) -- macho_load itself
 * doesn't know or care whether the bytes it's reading came from a
 * multiboot module or this kernel's own .rodata. Which specific init
 * this is -- this project's own reference implementation lives in
 * coreinit/ -- is a build-time choice (see build.sh's own INIT_MACHO
 * path) this kernel has no visibility into and no opinion about; see
 * include/init_abi.h's own note on why.
 *
 * kernel_image_addr/kernel_image_len point at this kernel's own
 * currently-loaded image (see macho_image_size), for init's own SMP
 * bring-up (smp.c, in this project's reference coreinit/ implementation)
 * to copy verbatim to each AP's own region -- no separate, embedded
 * "second module" needed the way multiboot's own two-module convention
 * required: this kernel is already fully loaded in memory at a known,
 * fixed address (KERNEL_LOAD_BASE), so its own span is all init needs
 * to know to copy it elsewhere. */
extern uint8_t g_embedded_init_start[];
extern uint8_t g_embedded_init_end[];

requires_thread_token(mode64_token)
void kernel_boot(void)
{
    serial_init();
    kprintf("[kernel] abstraction layer initialized\n");

    uint64_t init_len =
        (uint64_t)(g_embedded_init_end - g_embedded_init_start);
    kprintf("[kernel] embedded init image: %u bytes\n",
            (unsigned)init_len);

    /* target_base=0, not BSP_COREINIT_LOAD_BASE -- deliberately: the
     * kernel does not get to decide where init lives. 0 here means
     * "wherever init's own vmaddr already says", not "no
     * relocation" -- init's own build declares that address itself
     * (see init's own build-time -pagezero_size, currently 24 MiB),
     * so macho_load just honors what's already encoded in the image
     * it was handed, the same way it already honors that image's own
     * entry point rather than the kernel choosing one. The kernel's
     * only remaining job here is copying bytes to the address the
     * image itself requests -- see kernel/macho.h's own comment on
     * why target_base exists at all (it's for an AP's own, genuinely
     * different copy, relocated by a region_base this kernel receives
     * from its actual caller -- see kernel_boot_ap below -- not one it
     * invents). */
    uint64_t entry = macho_load((const void *)g_embedded_init_start, 0);
    if (entry == 0) {
        kprintf("[kernel] PANIC: failed to Mach-O-load init\n");
        halt_forever();
    }
    kprintf("[kernel] init entry point: 0x%X\n", entry);
    g_init_image_addr = (uint32_t)(uintptr_t)g_embedded_init_start;

    static struct init_boot_info boot_info;
    boot_info.caps = kernel_cap_bootstrap();
    boot_info.kernel_invoke = kernel_invoke;
    boot_info.kernel_load_base = KERNEL_LOAD_BASE;
    boot_info.kernel_boot_ap_addr = (uint64_t)(uintptr_t)kernel_boot_ap;
    boot_info.pml4_phys = (uint64_t)(uintptr_t)boot_pml4;
    boot_info.init_image_addr = (uint64_t)(uintptr_t)g_embedded_init_start;
    boot_info.init_image_len = init_len;
    /* This kernel's own currently-loaded image, for SMP bring-up
     * (smp.c) to copy verbatim to an AP's region -- see
     * macho_image_size's own comment for why this is a raw memory
     * span, not a re-parsed/re-loaded image: this kernel is already
     * correctly laid out in memory (GRUB put it there), so there's
     * nothing to load, only a size to know. KERNEL_LOAD_BASE is where
     * that image starts; its own Mach-O header is still there,
     * unmodified, letting macho_image_size compute how far it extends
     * without this kernel needing to separately track or embed its
     * own size anywhere. */
    boot_info.kernel_image_addr = KERNEL_LOAD_BASE;
    boot_info.kernel_image_len =
        macho_image_size((const void *)(uintptr_t)KERNEL_LOAD_BASE);
    if (boot_info.kernel_image_len == 0) {
        kprintf("[kernel] WARNING: couldn't determine this kernel's own "
               "image size -- SMP bring-up will not be possible this "
               "boot\n");
    } else {
        kprintf("[kernel] this kernel's own image: 0x%X bytes, for SMP "
               "bring-up\n",
               (unsigned)boot_info.kernel_image_len);
    }
    boot_info.bsp_kernel_invoke_addr = 0;
    boot_info.mutex_manager_handle = 0;
    boot_info.memory_manager_handle = 0;
    boot_info.hardware_mutex_id = 0;
    boot_info.lapic_calibration_count = 0;
    boot_info.cluster_scheduler_handle = 0;
    boot_info.own_mailbox_base = BSP_MAILBOX_REGION_BASE + CORE_MAILBOX_OFFSET;
    boot_info.ram_size = xnu_boot_args_ram_size();
    kprintf("[kernel] xnu boot_args: %s (addr 0x%X), ram_size %s (%u MiB)\n",
           g_xnu_boot_args_addr ? "captured" : "not captured",
           (unsigned)g_xnu_boot_args_addr,
           boot_info.ram_size ? "known" : "unknown",
           (unsigned)(boot_info.ram_size / (1024 * 1024)));
    /* Real EFI configuration-table lookup -- see
     * xnu_boot_args_find_rsdp's own comment for the mechanism. No
     * plain-memory-scan fallback: this project only supports the v2
     * boot_args shape (see g_xnu_version_string in boot.S), which
     * always carries a real efi_system_table on this project's
     * EFI-only boot path (confirmed against GRUB's own source: even
     * legacy BIOS gets a v2-shaped efi_system_table via GRUB's own
     * efiemu emulation layer -- the v1/v2 split is purely about
     * whether GRUB recognized the Darwin Kernel Version string, never
     * about EFI presence). 0 here means acpi_discover falls back to
     * its own legacy-BIOS scan (see that function's own comment). */
    boot_info.early_rsdp_phys = (uint64_t)xnu_boot_args_find_rsdp();
    kprintf("[kernel] ACPI RSDP via EFI config table: %s (0x%X)\n",
           boot_info.early_rsdp_phys ? "found" : "not found",
           (unsigned)boot_info.early_rsdp_phys);
    {
        uint64_t free_base = 0, free_size = 0;
        if (xnu_boot_args_find_largest_free(KERNEL_LOAD_BASE, &free_base,
                                            &free_size)) {
            kprintf("[kernel] largest free region (>= 0x%X, from GRUB's "
                   "real EFI memory map): base 0x%X, size 0x%X (%u MiB)\n",
                   (unsigned)KERNEL_LOAD_BASE, (unsigned)free_base,
                   (unsigned)free_size, (unsigned)(free_size / (1024 * 1024)));
        } else {
            kprintf("[kernel] no free region discoverable from boot_args -- "
                   "falling back to this project's own fixed layout\n");
        }
    }

    kprintf("[kernel] handing off to init -- kernel does not run again after this\n");

    init_entry_t init_entry = (init_entry_t)(uintptr_t)entry;
    init_entry(&boot_info);

    /* init_main must never return; if it somehow does, stop rather
     * than execute past the end of known code. */
    kprintf("[kernel] PANIC: init returned\n");
    halt_forever();
}

/* The AP entry point: reached via the real-mode trampoline (see
 * coreinit/ap_trampoline.S) -- an AP has no boot module list, nothing.
 * `region_base` is this core's own assigned region (see
 * include/smp_layout.h), passed in RDI by the trampoline's final jump,
 * per ordinary SysV calling convention.
 *
 * This function, and everything it calls, exists in a genuinely
 * separate physical copy of this entire kernel image -- init copied
 * the whole thing (code and data together) into region_base before ever
 * sending SIPI, specifically so this instance's own static state
 * (the factory object, everything kernel_cap_bootstrap builds) is
 * this core's own, not shared with the BSP's. See kernel_boot's own
 * -fpie note in build.sh for why copying code+data together, rather
 * than just data, is what makes this correct. */
#define PTE_PRESENT 0x1ull
#define PTE_WRITABLE 0x2ull
#define PTE_HUGE 0x80ull

/* Builds a fresh, independent identity map -- GB0 (0-0x40000000) and
 * GB3 (0xC0000000-0xFFFFFFFF, for LAPIC/IOAPIC MMIO) -- at four fixed,
 * 4KB-aligned offsets within this AP's own per-core region. Narrower
 * than boot.S's own setup_identity_map32, which covers the full 4GB
 * (GB0-GB3 contiguously) -- that breadth is specifically because GRUB's
 * xnu relocator chooses this kernel's own load address, which this
 * function doesn't need to worry about (an AP's own region is placed
 * by THIS project's own SMP bring-up code, at a known, fixed address --
 * see CORE_REGION_BASE), so GB0+GB3 stays sufficient here. Worth
 * revisiting together with the rest of AP bring-up if that ever stops
 * being true. Returns the new PML4's physical address. */
static uint64_t build_ap_page_tables(uint64_t region_base)
{
    uint64_t pml4_phys = region_base + CORE_PGTABLE_OFFSET + CORE_PGTABLE_PML4_OFF;
    uint64_t pdpt_phys = region_base + CORE_PGTABLE_OFFSET + CORE_PGTABLE_PDPT_OFF;
    uint64_t pd_phys = region_base + CORE_PGTABLE_OFFSET + CORE_PGTABLE_PD_OFF;
    uint64_t pd_hi_phys = region_base + CORE_PGTABLE_OFFSET + CORE_PGTABLE_PD_HI_OFF;

    uint64_t *pml4 = (uint64_t *)(uintptr_t)pml4_phys;
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_phys;
    uint64_t *pd = (uint64_t *)(uintptr_t)pd_phys;
    uint64_t *pd_hi = (uint64_t *)(uintptr_t)pd_hi_phys;

    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
    }

    pml4[0] = pdpt_phys | PTE_PRESENT | PTE_WRITABLE;
    pdpt[0] = pd_phys | PTE_PRESENT | PTE_WRITABLE; /* GB0 */
    pdpt[3] = pd_hi_phys | PTE_PRESENT | PTE_WRITABLE; /* GB3 */

    for (int i = 0; i < 512; i++) {
        pd[i] = ((uint64_t)i * 0x200000ull) | PTE_PRESENT | PTE_WRITABLE |
               PTE_HUGE;
        pd_hi[i] = (0xC0000000ull + (uint64_t)i * 0x200000ull) |
                  PTE_PRESENT | PTE_WRITABLE | PTE_HUGE;
    }

    return pml4_phys;
}

requires_thread_token(mode64_token)
void kernel_boot_ap(uint64_t region_base)
{
    serial_init();
    kprintf("[kernel] AP core booting, region base 0x%X\n",
            (unsigned)region_base);

    /* Independent page tables BEFORE anything else runs on this core --
     * deliberately as early as possible: if the switch itself were
     * ever wrong, this is where it should fail loudly, not somewhere
     * downstream after init is already mid-boot. Built under the
     * OLD (still-shared) tables, which already cover region_base
     * identically, so the writes above are safe; the switch below
     * changes only WHICH physical table structure is active. */
    uint64_t ap_pml4_phys = build_ap_page_tables(region_base);
    __asm__ volatile("mov %0, %%cr3" : : "r"(ap_pml4_phys) : "memory");
    kprintf("[kernel] AP switched to its own independent page tables "
            "(pml4 at 0x%X)\n",
            (unsigned)ap_pml4_phys);

    const void *init_image =
        (const void *)(uintptr_t)g_init_image_addr;
    uint64_t entry = macho_load(init_image,
                                region_base + CORE_INIT_OFFSET);
    if (entry == 0) {
        kprintf("[kernel] AP PANIC: failed to Mach-O-load init copy\n");
        halt_forever();
    }

    static struct init_boot_info boot_info;
    boot_info.caps = kernel_cap_bootstrap();
    boot_info.kernel_invoke = kernel_invoke;
    boot_info.kernel_load_base = KERNEL_LOAD_BASE;
    boot_info.kernel_boot_ap_addr = (uint64_t)(uintptr_t)kernel_boot_ap;
    boot_info.pml4_phys = ap_pml4_phys;
    /* An AP-booted init doesn't get the original module addresses --
     * only the BSP orchestrates further bring-up in this design, an
     * explicit scope limit rather than an oversight. */
    boot_info.init_image_addr = 0;
    boot_info.init_image_len = 0;
    boot_info.kernel_image_addr = 0;
    boot_info.kernel_image_len = 0;

    /* Read the discovery record init's own smp.c wrote into this exact
     * region before ever sending SIPI -- this is how objects the BSP
     * created before the trampoline become reachable from here, without
     * needing any capability handle to mean the same thing in two
     * genuinely separate capability tables. */
    const uint64_t *discovery =
        (const uint64_t *)(uintptr_t)(region_base + CORE_DISCOVERY_OFFSET);
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
    boot_info.own_mailbox_base = region_base + CORE_MAILBOX_OFFSET;

    kprintf("[kernel] AP handing off to its own init copy\n");
    init_entry_t init_entry = (init_entry_t)(uintptr_t)entry;
    init_entry(&boot_info);

    kprintf("[kernel] AP PANIC: init returned\n");
    halt_forever();
}
