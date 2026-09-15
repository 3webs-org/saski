#include "fdt.h"

/* All multi-byte fields in an FDT blob are big-endian, per spec,
 * regardless of the host CPU's own endianness -- x86-64 and ARM64 are
 * both little-endian in normal operation, so every field read here
 * needs an explicit byte-swap; never a direct dereference. */

#define FDT_MAGIC 0xd00dfeedu

#define FDT_BEGIN_NODE 0x00000001u
#define FDT_END_NODE   0x00000002u
#define FDT_PROP       0x00000003u
#define FDT_NOP        0x00000004u
#define FDT_END        0x00000009u

typedef struct {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
} fdt_header_t;

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd_be64(const uint8_t *p)
{
    return ((uint64_t)rd_be32(p) << 32) | (uint64_t)rd_be32(p + 4);
}

/* Reads a `cells`-sized (1 or 2 -- this project never expects more,
 * matching every #address-cells/#size-cells value actually observed
 * in practice) big-endian value out of a "reg"-style property's raw
 * bytes at `p`. Advances nothing; the caller tracks its own offset,
 * since a single "reg" entry is (address-cells + size-cells) cells
 * wide and this is called once per half. */
static uint64_t rd_cells(const uint8_t *p, uint32_t cells)
{
    if (cells == 1) {
        return (uint64_t)rd_be32(p);
    }
    /* cells == 2 (or anything else this project doesn't expect to
     * see -- treated the same as 2 rather than silently misreading
     * fewer bytes than the property actually contains). */
    return rd_be64(p);
}

int fdt_valid(const void *blob, uint64_t blob_size)
{
    if (!blob) {
        return 0;
    }
    const fdt_header_t *hdr = (const fdt_header_t *)blob;
    if (rd_be32((const uint8_t *)&hdr->magic) != FDT_MAGIC) {
        return 0;
    }
    if (blob_size != 0) {
        uint32_t totalsize = rd_be32((const uint8_t *)&hdr->totalsize);
        if ((uint64_t)totalsize > blob_size) {
            return 0;
        }
    }
    return 1;
}

size_t fdt_reserved_ranges(const void *blob, fdt_range_t *out, size_t max)
{
    const fdt_header_t *hdr = (const fdt_header_t *)blob;
    uint32_t off = rd_be32((const uint8_t *)&hdr->off_mem_rsvmap);
    const uint8_t *base = (const uint8_t *)blob;

    size_t count = 0;
    while (count < max) {
        uint64_t addr = rd_be64(base + off);
        uint64_t size = rd_be64(base + off + 8);
        if (addr == 0 && size == 0) {
            break; /* terminator, per spec */
        }
        out[count].base = addr;
        out[count].size = size;
        count++;
        off += 16;
    }
    return count;
}

/* Walks the structure block once, tracking #address-cells/#size-cells
 * (inherited from the nearest ancestor that sets them, defaulting to
 * 2/2 -- the common 64-bit convention, and what this project's own
 * targets need -- if the tree never sets them at all) and node depth,
 * collecting every "reg" range from any depth-1 node ("/memory" or
 * "/memory@...", per spec always a direct child of the root) into
 * `out`. Single pass, no recursion, no dynamic allocation -- just a
 * small fixed stack of inherited cell counts, one slot per depth
 * level this project would ever plausibly see (8 is generous; a
 * pathologically deep tree just stops updating cell counts past that
 * depth rather than overflowing anything, which only matters for
 * nodes this function doesn't care about anyway). */
size_t fdt_memory_ranges(const void *blob, fdt_range_t *out, size_t max)
{
    const fdt_header_t *hdr = (const fdt_header_t *)blob;
    const uint8_t *base = (const uint8_t *)blob;
    uint32_t struct_off = rd_be32((const uint8_t *)&hdr->off_dt_struct);
    uint32_t struct_size = rd_be32((const uint8_t *)&hdr->size_dt_struct);
    uint32_t strings_off = rd_be32((const uint8_t *)&hdr->off_dt_strings);
    const uint8_t *strings = base + strings_off;

    const uint8_t *p = base + struct_off;
    const uint8_t *end = p + struct_size;

    size_t count = 0;
    int depth = 0;
    enum { MAX_DEPTH = 8 };
    uint32_t addr_cells[MAX_DEPTH];
    uint32_t size_cells[MAX_DEPTH];
    addr_cells[0] = 2;
    size_cells[0] = 2;
    int is_memory_node[MAX_DEPTH];
    is_memory_node[0] = 0;

    while (p + 4 <= end) {
        uint32_t token = rd_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t namelen = 0;
            while (p[namelen] != '\0') {
                namelen++;
            }
            p += namelen + 1;
            /* Align to 4 bytes. */
            p = base + (((p - base) + 3) & ~(size_t)3);

            int new_depth = depth + 1;
            if (new_depth < MAX_DEPTH) {
                addr_cells[new_depth] = addr_cells[depth];
                size_cells[new_depth] = size_cells[depth];
                /* "/memory" or "/memory@ADDRESS" -- per spec, the
                 * device_type == "memory" property is the fully
                 * correct test, but every real-world DTB (including
                 * QEMU's own -machine virt output) also names the
                 * node itself "memory" or "memory@...", which is
                 * simpler to check without a second pass for the
                 * device_type property specifically. Depth 2 only:
                 * the root node's own BEGIN_NODE (empty name) is
                 * itself depth 1 in this counting, so an actual
                 * direct child of root -- which is where /memory
                 * always lives, per spec -- lands at depth 2, not 1
                 * (confirmed the hard way against a real dtc-compiled
                 * blob: the naive "depth 1" check silently matched
                 * nothing at all). */
                is_memory_node[new_depth] =
                    (new_depth == 2) &&
                    (name[0] == 'm' && name[1] == 'e' && name[2] == 'm' &&
                    name[3] == 'o' && name[4] == 'r' && name[5] == 'y' &&
                    (name[6] == '\0' || name[6] == '@'));
            }
            depth = new_depth;
        } else if (token == FDT_END_NODE) {
            depth--;
            if (depth < 0) {
                break; /* malformed; stop rather than read garbage */
            }
        } else if (token == FDT_PROP) {
            uint32_t len = rd_be32(p);
            uint32_t nameoff = rd_be32(p + 4);
            const uint8_t *propdata = p + 8;
            p = propdata + len;
            p = base + (((p - base) + 3) & ~(size_t)3);

            const char *propname = (const char *)(strings + nameoff);
            int at_valid_depth = (depth >= 0 && depth < MAX_DEPTH);

            if (at_valid_depth && propname[0] == '#' &&
               propname[1] == 'a' /* "#address-cells" */) {
                addr_cells[depth] = rd_be32(propdata);
            } else if (at_valid_depth && propname[0] == '#' &&
                      propname[1] == 's' /* "#size-cells" */) {
                size_cells[depth] = rd_be32(propdata);
            } else if (at_valid_depth && depth == 2 && is_memory_node[2] &&
                      propname[0] == 'r' && propname[1] == 'e' &&
                      propname[2] == 'g' && propname[3] == '\0') {
                uint32_t ac = addr_cells[1]; /* reg cells are sized by
                                              * the PARENT's own
                                              * #address-cells/
                                              * #size-cells, per spec
                                              * -- root's (depth 1 in
                                              * this counting -- see
                                              * the BEGIN_NODE handler
                                              * above), for a
                                              * depth-2/direct-child-
                                              * of-root node. */
                uint32_t sc = size_cells[1];
                uint32_t entry_bytes = (ac + sc) * 4;
                if (entry_bytes > 0) {
                    uint32_t n = len / entry_bytes;
                    for (uint32_t i = 0; i < n && count < max; i++) {
                        const uint8_t *entry = propdata + i * entry_bytes;
                        out[count].base = rd_cells(entry, ac);
                        out[count].size = rd_cells(entry + ac * 4, sc);
                        count++;
                    }
                }
            }
        } else if (token == FDT_NOP) {
            /* nothing to skip -- NOP has no payload */
        } else if (token == FDT_END) {
            break;
        } else {
            break; /* unrecognized token -- malformed or truncated;
                    * stop rather than misinterpret arbitrary bytes as
                    * further tokens */
        }
    }

    return count;
}
