#pragma once

#include <stdint.h>

/* aarch64 counterparts to arch_x86_64.h's primitives.
 *
 * There is no port I/O on this architecture: MMIO replaces it, and the UART
 * this kernel targets (PL011 on QEMU's virt machine) is memory-mapped at a
 * fixed address from the device tree. So outb/inb have no direct equivalent
 * and are NOT provided -- a caller that reaches for them on aarch64 is
 * expressing an x86 assumption, and a compile error is the correct response,
 * not a silent no-op. serial.c's aarch64 backend will use its own MMIO access
 *ors rather than these.
 *
 * halt_forever maps to wfi (wait for interrupt), the architectural low-power
 * state: on a kernel that has handed off and has nothing left to do, waiting
 * for an interrupt that will never come is the correct way to stop. */

static inline void halt_forever(void)
{
    for (;;) {
        __asm__ volatile("wfi");
    }
}
