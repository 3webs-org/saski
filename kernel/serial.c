#include "arch_x86_64.h"
#include "serial.h"
#include <stdarg.h>
#include <stdint.h>

/* QEMU's default ISA serial port (COM1) I/O base address. */
#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* disable interrupts */
    outb(COM1 + 3, 0x80); /* enable DLAB */
    outb(COM1 + 0, 0x03); /* divisor low byte: 38400 baud */
    outb(COM1 + 1, 0x00); /* divisor high byte */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7); /* enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
}

static int transmit_empty(void)
{
    return inb(COM1 + 5) & 0x20;
}

static void putc(char c)
{
    while (!transmit_empty()) {
    }
    outb(COM1, (uint8_t)c);
}

static void puts_raw(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            putc('\r');
        }
        putc(*s++);
    }
}

static void put_uint(unsigned long v, unsigned base, int upper)
{
    char buf[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (v == 0) {
        putc('0');
        return;
    }
    while (v > 0 && i < (int)sizeof(buf)) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i > 0) {
        putc(buf[--i]);
    }
}

static void put_int(long v)
{
    if (v < 0) {
        putc('-');
        put_uint((unsigned long)(-v), 10, 0);
    } else {
        put_uint((unsigned long)v, 10, 0);
    }
}

/* Minimal freestanding formatter: %s %d %u %x %X %c %% only. Enough for
 * kernel/coreinit bring-up logging; not a general libc printf. */
void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (*p == '\n') {
                putc('\r');
            }
            putc(*p);
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
            putc((char)va_arg(ap, int));
            break;
        case '%':
            putc('%');
            break;
        default:
            putc('%');
            putc(*p);
            break;
        }
    }

    va_end(ap);
}
