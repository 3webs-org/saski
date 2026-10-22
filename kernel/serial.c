#include "serial.h"
#include <stdarg.h>
#include <stdint.h>

/* The formatter half of the kernel console.
 *
 * This file is deliberately architecture-neutral: it never touches a port, an
 * MMIO register, or an inline asm instruction. It calls serial_putc, which the
 * per-architecture transport file provides. The x86_64 transport is the ISA
 * COM1 UART through port I/O; the aarch64 transport is QEMU virt's PL011
 * through MMIO. Both expose the same two functions, so this file compiles
 * unchanged for either.
 *
 * Minimal freestanding formatter: %s %d %u %x %X %c %% only. Enough for
 * kernel/coreinit bring-up logging; not a general libc printf. */

static void puts_raw(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}

static void put_uint(unsigned long v, unsigned base, int upper)
{
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (v == 0) {
        serial_putc('0');
        return;
    }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i > 0) {
        serial_putc(buf[--i]);
    }
}

static void put_int(long v)
{
    if (v < 0) {
        serial_putc('-');
        put_uint((unsigned long)(-v), 10, 0);
    } else {
        put_uint((unsigned long)v, 10, 0);
    }
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (*p == '\n') {
                serial_putc('\r');
            }
            serial_putc(*p);
            continue;
        }
        p++;
        switch (*p) {
        case 's':
            puts_raw(va_arg(ap, const char *));
            break;
        case 'd':
            put_int(va_arg(ap, int));
            break;
        case 'u':
            put_uint(va_arg(ap, unsigned int), 10, 0);
            break;
        case 'x':
            put_uint(va_arg(ap, unsigned int), 16, 0);
            break;
        case 'X':
            put_uint(va_arg(ap, unsigned int), 16, 1);
            break;
        case 'c':
            serial_putc((char)va_arg(ap, int));
            break;
        case '%':
            serial_putc('%');
            break;
        default:
            serial_putc('%');
            serial_putc(*p);
            break;
        }
    }

    va_end(ap);
}
