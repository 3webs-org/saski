#pragma once

#include "mode.h"
#include <stdint.h>

/* Every raw inline-assembly primitive the kernel uses lives here, and
 * nowhere else -- serial.c, kernel.c, and cap.c call these instead of
 * embedding __asm__ directly. This is the minimum-surface consolidation
 * requested: one place to audit for amd64-specific machine instructions,
 * with each one explicitly requiring mode64_token.
 *
 * Be precise about what that annotation does and doesn't buy: it does
 * NOT catch a caller running before the 32-bit-to-64-bit transition --
 * every C function in this kernel already runs strictly after that
 * transition by construction (nothing in C executes before
 * enter_long_mode's far jump; there is no C call edge into that period
 * for a checker to reason about at all). What it DOES do is mark, in a
 * form the analyzer can read back later if this checker suite ever grows
 * a generic (non-errno-specific) reader for grants_thread_token /
 * requires_thread_token, exactly which functions are amd64-only --
 * directly serving the "temporary platform split" boot.S already
 * documents. If a genuinely portable (non-amd64) build path is ever
 * added, this is the file whose contents would need an i386
 * counterpart, and the annotation is what would let a checker flag any
 * accidental call into it from portable code. */

requires_thread_token(mode64_token)
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

requires_thread_token(mode64_token)
static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

requires_thread_token(mode64_token)
static inline void halt_forever(void)
{
    for (;;) {
        __asm__ volatile("hlt");
    }
}
