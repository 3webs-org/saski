#include <stdint.h>

/* aarch64 serial transport: QEMU virt's PL011 UART, memory-mapped.
 *
 * There is no port I/O on this architecture, so the x86 transport's outb/inb
 * do not apply; the UART is reached through normal memory accesses to its MMIO
 * window. The base address is the one QEMU's -machine virt uses (0x09000000),
 * which arch/qemu-virt.dts also declares -- the two are the same fact written
 * twice, and the device tree is the one that should win once discovery exists.
 *
 * PL011 needs no init for QEMU's default: firmware leaves it configured, and
 * the flags register's transmit-empty bit is all a polled transmitter needs. */

#define PL011_BASE 0x09000000ull
#define PL011_DR   (PL011_BASE + 0x000)  /* data register */
#define PL011_FR   (PL011_BASE + 0x018)  /* flag register */
#define PL011_FR_TXFF (1u << 5)         /* transmit FIFO full */

static volatile uint8_t *reg(uint64_t off)
{
    return (volatile uint8_t *)(uintptr_t)off;
}

void serial_init(void)
{
    /* Nothing to do for QEMU's PL011: firmware has already enabled the UART.
     * Kept as a symbol so the transport interface is identical across arches. */
}

void serial_putc(char c)
{
    while (*reg(PL011_FR) & PL011_FR_TXFF) {
    }
    *reg(PL011_DR) = (uint8_t)c;
}
