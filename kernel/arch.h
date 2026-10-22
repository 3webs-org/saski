#pragma once

/* Architecture dispatch for the kernel's raw machine primitives.
 *
 * Every translation unit that needs an outb, an inb, or a halt includes THIS
 * file, not an arch-specific one. The arch-specific headers provide the same
 * three names with arch-appropriate implementations, so callers never change
 * when the platform does -- which is the whole point of a seam: the x86_64
 * header's own comment says a portable build path would need a counterpart of
 * that file, and this dispatcher is how the counterpart gets selected.
 *
 * The x86_64 default is preserved exactly: ARCH is only consulted when the
 * compiler does not already tell us. On an aarch64 target __aarch64__ is
 * defined by the compiler, so the aarch64 header is selected with no build
 * flag at all -- the compiler is the authority on what it is compiling for,
 * which is one less thing to get wrong. */

#if defined(__aarch64__)
#include "arch_aarch64.h"
#else
#include "arch_x86_64.h"
#endif
