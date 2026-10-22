#pragma once

/* The kernel's console: one transport per architecture, one formatter shared
 * by both. serial_init and serial_putc are the transport interface, provided
 * by serial_x86_64.c or serial_aarch64.c; kprintf is the formatter, which is
 * architecture-neutral and lives in serial.c. Splitting them this way means a
 * new architecture adds one file implementing two functions and changes
 * nothing else. */

void serial_init(void);
void serial_putc(char c);
void kprintf(const char *fmt, ...);
