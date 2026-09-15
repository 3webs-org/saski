#include "macho.h"
#include "serial.h"
#include <stdint.h>

#define MH_MAGIC_64     0xfeedfacfu
#define FAT_MAGIC       0xcafebabeu
#define CPU_TYPE_X86_64 0x01000007u
#define LC_SEGMENT_64   0x19u
#define LC_MAIN         0x80000028u /* 0x28 | LC_REQ_DYLD */

typedef struct {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags,
        reserved;
} mach_header_64_t;

typedef struct {
    uint32_t cmd, cmdsize;
} load_command_t;

typedef struct {
    uint32_t cmd, cmdsize;
    char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
} segment_command_64_t;

typedef struct {
    uint32_t cmd, cmdsize;
    uint64_t entryoff, stacksize;
} entry_point_command_t;

/* The fat header and every fat_arch entry are always big-endian
 * regardless of the architectures they describe (a historical PowerPC
 * convention Apple never revisited) -- confirmed against our own
 * ld64.lld/llvm-lipo-produced universal binaries earlier: the magic
 * bytes on disk are CA FE BA BE, which reads as 0xBEBAFECA on a
 * little-endian load, not 0xCAFEBABE, unless explicitly byte-swapped. */
static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Returns the byte offset of the x86_64 slice within `image`, or 0 if
 * `image` is already a plain (non-fat) x86_64 Mach-O, or UINT32_MAX on
 * failure (neither a fat image nor a directly-recognizable one, or no
 * x86_64 slice present in a fat one). */
static uint32_t resolve_slice_offset(const uint8_t *image)
{
    uint32_t magic_native = *(const uint32_t *)image;
    if (magic_native == MH_MAGIC_64) {
        return 0;
    }

    if (rd_be32(image) != FAT_MAGIC) {
        return 0xFFFFFFFFu;
    }

    uint32_t nfat_arch = rd_be32(image + 4);
    const uint8_t *arch = image + 8;
    for (uint32_t i = 0; i < nfat_arch; i++) {
        uint32_t cputype = rd_be32(arch + 0);
        uint32_t offset = rd_be32(arch + 8);
        if (cputype == CPU_TYPE_X86_64) {
            return offset;
        }
        arch += 20; /* sizeof(struct fat_arch) */
    }
    return 0xFFFFFFFFu;
}

uint64_t macho_load(const void *image_raw, uint64_t target_base)
{
    const uint8_t *base = (const uint8_t *)image_raw;
    uint32_t slice_off = resolve_slice_offset(base);
    if (slice_off == 0xFFFFFFFFu) {
        kprintf("[kernel] macho_load: no recognizable x86_64 slice\n");
        return 0;
    }

    const uint8_t *slice = base + slice_off;
    const mach_header_64_t *mh = (const mach_header_64_t *)slice;
    if (mh->magic != MH_MAGIC_64) {
        kprintf("[kernel] macho_load: bad slice magic\n");
        return 0;
    }
    if (mh->cputype != CPU_TYPE_X86_64) {
        kprintf("[kernel] macho_load: slice is not x86_64\n");
        return 0;
    }

    const uint8_t *cmd_base = slice + sizeof(mach_header_64_t);
    uint64_t entryoff = 0;
    int have_entry = 0;

    /* First pass: map every LC_SEGMENT_64 to target_base + its declared
     * vmaddr. */
    const uint8_t *p = cmd_base;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const load_command_t *lc = (const load_command_t *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const segment_command_64_t *seg = (const segment_command_64_t *)p;
            /* Skip segments with no real, file-backed content at all
             * -- e.g. a PIE binary's own __PAGEZERO, which is
             * deliberately left unmapped as a null-pointer trap, not
             * something to copy or zero-fill. Checking filesize, not
             * just vmsize: __PAGEZERO has vmsize > 0 (that's the whole
             * point -- it reserves address space) but filesize == 0
             * (no real content backs it) -- init itself now has a
             * genuine, nonzero-sized one (see init's own build's
             * -pagezero_size, no longer 0x0 -- kernel_boot no longer
             * decides init's own load address, init's own build
             * declares it), so checking vmsize alone (as this used to)
             * caught nothing here and tried to zero-fill __PAGEZERO's
             * entire span starting at target_base + 0 -- for the BSP's
             * own init, target_base is 0 too, so that span landed
             * squarely on top of this kernel's own currently-running
             * code, overwriting it out from under itself. Found the
             * hard way: silent, total hang, no further output at all. */
            if (seg->vmsize == 0 || seg->filesize == 0) {
                p += lc->cmdsize;
                continue;
            }
            uint64_t dest_vaddr = target_base + seg->vmaddr;
            kprintf("[kernel] macho_load: segment '%s' -> vaddr 0x%X, "
                    "filesize %u, memsz %u\n",
                    seg->segname, (unsigned)dest_vaddr,
                    (unsigned)seg->filesize, (unsigned)seg->vmsize);
            uint8_t *dst = (uint8_t *)(uintptr_t)dest_vaddr;
            const uint8_t *src = slice + seg->fileoff;
            for (uint64_t b = 0; b < seg->filesize; b++) {
                dst[b] = src[b];
            }
            for (uint64_t b = seg->filesize; b < seg->vmsize; b++) {
                dst[b] = 0;
            }
        } else if (lc->cmd == LC_MAIN) {
            const entry_point_command_t *ep = (const entry_point_command_t *)p;
            entryoff = ep->entryoff;
            have_entry = 1;
        }
        p += lc->cmdsize;
    }

    if (!have_entry) {
        kprintf("[kernel] macho_load: no LC_MAIN found\n");
        return 0;
    }

    /* entryoff is a file offset within the slice, not a runtime address
     * -- translate it by finding which segment's file range contains it
     * and applying that segment's own file->vaddr delta, then
     * target_base on top. A second pass, since LC_MAIN can appear before
     * or after the segment containing its target in load-command
     * order. */
    p = cmd_base;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const load_command_t *lc = (const load_command_t *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const segment_command_64_t *seg = (const segment_command_64_t *)p;
            if (entryoff >= seg->fileoff &&
                entryoff < seg->fileoff + seg->filesize) {
                return target_base + seg->vmaddr + (entryoff - seg->fileoff);
            }
        }
        p += lc->cmdsize;
    }

    kprintf("[kernel] macho_load: entryoff not within any mapped segment\n");
    return 0;
}

uint64_t macho_image_size(const void *image_raw)
{
    const uint8_t *base = (const uint8_t *)image_raw;
    uint32_t slice_off = resolve_slice_offset(base);
    if (slice_off == 0xFFFFFFFFu) {
        return 0;
    }

    const uint8_t *slice = base + slice_off;
    const mach_header_64_t *mh = (const mach_header_64_t *)slice;
    if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_X86_64) {
        return 0;
    }

    /* Span from the lowest to the highest vmaddr among segments with
     * REAL content (filesize > 0), not from vmaddr 0 -- a segment with
     * filesize == 0 is a true, unbacked reservation (a Darwin PIE
     * binary's own __PAGEZERO, most notably: this project's own
     * kernel links with a real, 16MB one -- see build.sh's
     * -pagezero_size -- specifically so its own vmaddr-0 no longer
     * collides with firmware-owned low memory). Counting it here would
     * report this kernel's own image as ~16MB larger than it actually
     * is: the caller (kernel_boot) wants the span its own currently-
     * loaded, ACTUAL image occupies starting from kernel_image_addr
     * (KERNEL_LOAD_BASE, __TEXT's own vmaddr), not from address 0. */
    uint64_t lowest = 0xFFFFFFFFFFFFFFFFull;
    uint64_t highest = 0;
    int found_any = 0;
    const uint8_t *p = slice + sizeof(mach_header_64_t);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        const load_command_t *lc = (const load_command_t *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const segment_command_64_t *seg = (const segment_command_64_t *)p;
            if (seg->filesize == 0) {
                p += lc->cmdsize;
                continue; /* an unbacked reservation, e.g. __PAGEZERO --
                          * not part of the real, copyable image */
            }
            uint64_t end = seg->vmaddr + seg->vmsize;
            if (end > highest) {
                highest = end;
            }
            if (seg->vmaddr < lowest) {
                lowest = seg->vmaddr;
            }
            found_any = 1;
        }
        p += lc->cmdsize;
    }

    return found_any ? (highest - lowest) : 0;
}
