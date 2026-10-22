#include "arch.h"
#include "serial.h"
#include "fdt.h"
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

void kernel_boot_aarch64(void)
{
    serial_init();

    kprintf("[kernel] aarch64 boot\n");



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
