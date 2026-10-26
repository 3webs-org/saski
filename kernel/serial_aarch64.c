#include <stdint.h>
#include "fdt.h"

/* aarch64 serial transport: QEMU virt's PL011 UART, memory-mapped.
 *
 * There is no port I/O on this architecture, so the x86 transport's outb/inb
 * do not apply; the UART is reached through normal memory accesses to its MMIO
 * window. The base address is the one QEMU's -machine virt uses (0x09000000),
 * which tests/qemu-virt.dts also declares. This fixed early-boot address
 * is specific to QEMU virt; a platform's own device tree should win once
 * discovery exists.
 *
 * PL011 needs no init for QEMU's default: firmware leaves it configured, and
 * the flags register's transmit-empty bit is all a polled transmitter needs. */

#define PL011_DEFAULT_BASE 0x09000000ull
#define PL011_DR   0x000
#define PL011_FR   0x018
#define PL011_FR_TXFF (1u << 5)         /* transmit FIFO full */
#define CADENCE_CR 0x000
#define CADENCE_CR_RX_DIS (1u << 3)
#define CADENCE_CR_TX_EN (1u << 4)
#define CADENCE_CR_TX_DIS (1u << 5)
#define CADENCE_SR 0x02c
#define CADENCE_FIFO 0x030
#define CADENCE_SR_TXFULL (1u << 4)

extern unsigned long long g_fdt_blob_phys;
extern unsigned long long g_fdt_blob_size;
static uint64_t g_uart_base = PL011_DEFAULT_BASE;
static int g_uart_is_cadence;

static volatile uint8_t *reg(uint64_t off)
{
    return (volatile uint8_t *)(uintptr_t)off;
}

void serial_init(void)
{
    const void *blob = (const void *)(uintptr_t)g_fdt_blob_phys;
    if (!blob || !fdt_valid(blob, g_fdt_blob_size)) return;
    int node = fdt_node_by_compatible_offset(blob, "cdns,uart-r1p12");
    if (node == FDT_ERR_NODE)
        node = fdt_node_by_compatible_offset(blob, "xlnx,zynqmp-uart");
    fdt_range_t range;
    if (node != FDT_ERR_NODE && fdt_is_enabled(blob, node) &&
        fdt_reg_to_cpu_addr(blob, node, 0, &range)) {
        g_uart_base = range.base;
        g_uart_is_cadence = 1;
        volatile uint32_t *control =
            (volatile uint32_t *)reg(g_uart_base + CADENCE_CR);
        uint32_t value = *control;
        value &= ~(CADENCE_CR_TX_DIS | CADENCE_CR_RX_DIS);
        value |= CADENCE_CR_TX_EN;
        *control = value;
        return;
    }
    node = fdt_node_by_compatible_offset(blob, "arm,pl011");
    if (node != FDT_ERR_NODE && fdt_is_enabled(blob, node) &&
        fdt_reg_to_cpu_addr(blob, node, 0, &range))
        g_uart_base = range.base;
}

void serial_putc(char c)
{
    uint64_t status = g_uart_is_cadence ? CADENCE_SR : PL011_FR;
    uint32_t full = g_uart_is_cadence ? CADENCE_SR_TXFULL : PL011_FR_TXFF;
    while (*(volatile uint32_t *)reg(g_uart_base + status) & full) {}
    *(volatile uint32_t *)reg(g_uart_base +
        (g_uart_is_cadence ? CADENCE_FIFO : PL011_DR)) = (uint8_t)c;
}
