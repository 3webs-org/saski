#pragma once

#include <stdint.h>

/* Loads a (possibly fat/universal) Mach-O image already sitting in
 * memory at `image` -- embedded directly into this kernel's own binary
 * (see boot.S's .incbin), nothing has parsed it yet. If the image is fat, selects the
 * x86_64 slice; if it's a plain (non-fat) 64-bit Mach-O, treats the
 * whole buffer as that one slice directly. Copies every LC_SEGMENT_64
 * segment to `target_base + segment_vmaddr` (not just segment_vmaddr
 * directly), zeroing its BSS tail, and returns the runtime address of
 * the entry point similarly offset by target_base.
 *
 * target_base=0 for the BSP's own init -- deliberately not "no
 * relocation", but "honor whatever vmaddr this image's own build
 * already declared" (currently 24 MiB in this project's own reference coreinit/, via that build's own
 * -pagezero_size; see kernel_boot's own call site). This is a
 * principle this kernel holds throughout, not specific to this one
 * call: it never decides where something it's loading should live --
 * it only ever acts on a location it was actually given, whether
 * that's the image's own self-declared vmaddr (this case) or a
 * region_base handed down from an actual caller (the AP case below).
 * A nonzero target_base is what lets an additional core's init
 * copy land in that core's own assigned region instead of colliding
 * with the BSP's -- that region_base itself comes from kernel_boot_ap's
 * own caller (ultimately init's own smp_bring_up_all), never
 * invented here. Returns 0 on failure.
 *
 * This is the kernel's only opinion about init's binary format:
 * Mach-O, an x86_64 (amd64-platform-split, see boot.S) slice if fat,
 * nothing else assumed -- it does not care what init's segments
 * actually contain, only where they go. */
uint64_t macho_load(const void *image, uint64_t target_base);

/* Computes the total span (highest segment vmaddr + vmsize, i.e. how
 * many bytes from the image's own vmaddr-0 base its loaded footprint
 * covers) of a plain (non-fat) x86_64 Mach-O already sitting in memory
 * at `image` -- without copying or loading anything, and without
 * needing an entry point at all (unlike macho_load, this never looks
 * for LC_MAIN or LC_UNIXTHREAD), which is exactly why this exists as
 * its own function: this kernel's own Mach-O (see boot.S and
 * tools/macho_to_xnu.py) has neither by the time it's actually
 * running -- LC_MAIN got replaced with LC_UNIXTHREAD during that
 * post-processing step, and macho_load only ever understood LC_MAIN.
 * Used by kernel_boot to tell init's own SMP bring-up (smp.c) how many
 * bytes of this kernel's own currently-loaded image to copy verbatim
 * to an AP's region -- a raw copy, not a re-parse, is deliberate: this
 * kernel is PIE and already correctly laid out in memory (GRUB put it
 * there), and the only code that ever runs from within the copy
 * (kernel_boot_ap and whatever it calls) is itself PIE/RIP-relative,
 * so nothing in the copy needs re-relocating -- see smp_bring_up_all's
 * own comment for the fuller picture, including why boot.S's own
 * absolute-addressed 32-bit setup code never actually runs from a
 * copy at all: an AP reaches kernel_boot_ap directly, via
 * ap_trampoline.S's own separate transition, never through this
 * kernel's own _start. Returns 0 on
 * failure (not a fat image, no x86_64 slice, or no LC_SEGMENT_64 found
 * at all). */
uint64_t macho_image_size(const void *image);
