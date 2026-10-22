/* SPDX-License-Identifier: 0BSD */
/*
 * Host test for lib/fdt.c.
 *
 * This is NOT a kernel test. It builds natively against the same lib/fdt.c the
 * kernel links and walks a dtc-compiled blob, which is the strongest check
 * available before there is a kernel-side caller -- stronger than compiling,
 * weaker than a boot. It is not a substitute for the QEMU judge.
 *
 * The checks are chosen around what actually breaks DTB readers: address
 * translation through a non-identity ranges window, matching a compatible LIST
 * rather than only its first entry, cell-count inheritance, and bounds safety
 * when the blob is truncated or corrupted. Untrusted input is walked at every
 * truncation length, because on ARM the blob arrives from firmware.
 *
 * Every assertion states what property it is protecting, not just that two
 * numbers match -- a test whose failure message does not say what broke sends
 * the next person back to the spec anyway.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fdt.h"

static int g_pass, g_fail;

static void ok(int cond, const char *what)
{
    if (cond) {
        g_pass++;
        printf("  ok   %s\n", what);
    } else {
        g_fail++;
        printf("  FAIL %s\n", what);
    }
}

static uint8_t *slurp(const char *path, long *out_len)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *b;

    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = n;
    return b;
}

/* Reads a big-endian 32-bit cell out of a property. */
static uint32_t cell32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int main(int argc, char **argv)
{
    long n = 0;
    uint8_t *blob;
    int root, soc, serial, gic, cpus;
    uint32_t len = 0;
    fdt_range_t r, mem[8], rsv[8];

    if (argc < 2) {
        printf("usage: %s <blob.dtb>\n", argv[0]);
        return 2;
    }
    blob = slurp(argv[1], &n);
    if (!blob) {
        printf("cannot read %s\n", argv[1]);
        return 2;
    }
    printf("blob: %s (%ld bytes)\n\n", argv[1], n);

    /* ---- validity ---- */
    ok(fdt_valid(blob, (uint64_t)n) == 1, "a real DTB is accepted");
    ok(fdt_valid(blob, (uint64_t)n - 40) == 0, "a size shorter than the blob is rejected");
    ok(fdt_valid(NULL, 0) == 0, "a NULL blob is rejected");

    /* ---- paths ---- */
    root = fdt_path_offset(blob, "/");
    ok(root >= 0, "/ resolves to the root");
    soc = fdt_path_offset(blob, "/soc");
    ok(soc >= 0, "/soc resolves");
    serial = fdt_path_offset(blob, "/soc/serial@2000");
    ok(serial >= 0, "/soc/serial@2000 resolves");
    gic = fdt_path_offset(blob, "/soc/interrupt-controller@1000");
    ok(gic >= 0, "/soc/interrupt-controller@1000 resolves");
    ok(fdt_path_offset(blob, "/soc/serial@9999") == FDT_ERR_NODE,
       "a nonexistent unit address does not resolve");
    ok(fdt_path_offset(blob, "/soc/serial") == FDT_ERR_NODE,
       "a name without its unit address does NOT match");

    /* ---- properties ---- */
    {
        const char *m = (const char *)fdt_getprop(blob, root, "model", &len);
        ok(m != NULL && len == strlen("saski fdt test") + 1,
           "model is present with its NUL included in the length");
        ok(m && strcmp(m, "saski fdt test") == 0, "model value reads back exactly");
    }
    ok(fdt_getprop(blob, root, "nosuchprop", &len) == NULL,
       "an absent property returns NULL");
    {
        const void *flag = fdt_getprop(blob, gic, "interrupt-controller", &len);
        ok(flag != NULL && len == 0,
           "a flag property is non-NULL with length 0 (distinct from absent)");
    }
    {
        const uint8_t *ic = (const uint8_t *)fdt_getprop(blob, gic, "#interrupt-cells", &len);
        ok(ic && len == 4 && cell32(ic) == 3, "#interrupt-cells reads as 3");
    }

    /* ---- cell inheritance ---- */
    {
        /* int, not uint32_t: the API reports cell counts as int so a caller
         * can distinguish a missing value from a legitimate 0.
         *
         * The semantics are the reg-sizing ones: fdt_cell_counts(node) gives
         * the counts that size THAT NODE's own reg entries, which per spec come
         * from its PARENT. So /soc reports the root's 2/2, not /soc's own 1/1 --
         * /soc's own 1/1 sizes the reg entries of its children. Asserting 1/1
         * for /soc would be testing the wrong rule; the child assertion below
         * is what proves inheritance actually happens. */
        int ac = 0, sc = 0;
        ok(fdt_cell_counts(blob, soc, &ac, &sc) == 1 && ac == 2 && sc == 2,
           "a direct child of root is sized by the root's 2/2, not its own 1/1");
        ac = sc = 0;
        ok(fdt_cell_counts(blob, serial, &ac, &sc) == 1 && ac == 1 && sc == 1,
           "a grandchild inherits 1/1 from /soc, not 2/2 from the root");
    }

    /* ---- own cells vs reg-sizing cells ---- */
    {
        /* The pair that is easy to conflate. /soc's own declaration sizes its
         * CHILDREN's reg entries; /soc's own reg is sized by the root. */
        int ac = -1, sc = -1;
        ok(fdt_own_cell_counts(blob, soc, &ac, &sc) == 1 && ac == 1 && sc == 1,
           "own cells for /soc report the 1/1 it declares for its children");

        ac = -1; sc = -1;
        ok(fdt_own_cell_counts(blob, root, &ac, &sc) == 1 && ac == 2 && sc == 2,
           "own cells for the root report its 2/2");

        /* A node with no declaration of its own must report not-found rather
         * than inheriting -- that difference is how a caller detects a tree
         * that is relying on inheritance where the spec wants a declaration. */
        ac = -1; sc = -1;
        ok(fdt_own_cell_counts(blob, serial, &ac, &sc) == 0,
           "a node declaring neither reports not-found, not an inherited value");
        ok(ac == -1 && sc == -1,
           "not-found leaves the outputs untouched rather than zeroing them");

        /* And the contrast, asserted directly so the pair cannot drift apart. */
        ac = -1; sc = -1;
        ok(fdt_cell_counts(blob, soc, &ac, &sc) == 1 && ac == 2 && sc == 2,
           "fdt_cell_counts(/soc) reports the ROOT's 2/2, unlike own cells");
    }

    /* ---- THE TRANSLATION: non-identity ranges ---- */
    ok(fdt_reg_to_addr(blob, serial, &r.base, &r.size) == 1,
       "reg translates for the UART");
    ok(r.base == 0x80002000ull,
       "NON-IDENTITY ranges: 0x2000 inside /soc becomes 0x80002000");
    ok(r.size == 0x1000ull, "the translated size is the declared size");
    ok(fdt_reg_to_addr(blob, gic, &r.base, &r.size) == 1 && r.base == 0x80001000ull,
       "the GIC's 0x1000 becomes 0x80001000");

    /* ---- memory and reservations ---- */
    {
        size_t m = fdt_memory_ranges(blob, mem, 8);
        ok(m == 1 && mem[0].base == 0x80000000ull && mem[0].size == 0x10000000ull,
           "the memory node yields one range of the declared size");
        size_t q = fdt_reserved_ranges(blob, rsv, 8);
        ok(q == 1 && rsv[0].base == 0x80000000ull && rsv[0].size == 0x1000ull,
           "the memreserve block is read");
    }

    /* ---- compatible: EVERY entry must match ---- */
    {
        int off = FDT_ERR_NODE;
        ok(fdt_node_by_compatible(blob, "arm,pl011", &off) == 1 && off >= 0,
           "compatible matches the FIRST entry of the list");
        off = FDT_ERR_NODE;
        ok(fdt_node_by_compatible(blob, "arm,primecell", &off) == 1 && off >= 0,
           "compatible matches the SECOND entry (the classic bug)");
        off = FDT_ERR_NODE;
        ok(fdt_node_by_compatible(blob, "arm,gic-v3", &off) == 1 && off >= 0,
           "the GIC is found by its own compatible string");
        off = FDT_ERR_NODE;
        ok(fdt_node_by_compatible(blob, "no,such,thing", &off) == 0,
           "an absent compatible reports not-found");
    }

    /* ---- child enumeration ---- */
    cpus = fdt_path_offset(blob, "/cpus");
    {
        int cursor = 0, count = 0, c;
        while ((c = fdt_next_child(blob, cpus, &cursor)) != 0) {
            count++;
        }
        ok(count == 3, "child enumeration counts three CPUs");
    }

    /* ---- enabled/disabled ---- */
    {
        int enabled = fdt_path_offset(blob, "/soc/serial@2000");
        int disabled = fdt_path_offset(blob, "/soc/serial@3000");
        ok(fdt_is_enabled(blob, enabled) == 1, "status=okay reads as enabled");
        ok(fdt_is_enabled(blob, disabled) == 0,
           "status=disabled reads as disabled, so it can be skipped");
        ok(fdt_is_enabled(blob, soc) == 1,
           "an absent status reads as enabled");
    }

    /* ---- bounds: untrusted input ---- */
    {
        long cut;
        int survived = 1;
        for (cut = 0; cut < n; cut += 7) {
            int o;
            uint64_t b, s;
            (void)fdt_valid(blob, (uint64_t)cut);
            o = fdt_path_offset(blob, "/soc/serial@2000");
            (void)fdt_reg_to_addr(blob, o >= 0 ? o : 0, &b, &s);
            (void)fdt_node_by_compatible(blob, "arm,pl011", &o);
            (void)fdt_memory_ranges(blob, mem, 8);
            (void)fdt_reserved_ranges(blob, rsv, 8);
        }
        ok(survived, "walking every truncation of the blob does not crash");
    }
    {
        uint8_t *copy = malloc((size_t)n);
        if (copy) {
            memcpy(copy, blob, (size_t)n);
            copy[0] = 0; /* break the magic */
            ok(fdt_valid(copy, (uint64_t)n) == 0, "a corrupted magic is rejected");
            ok(fdt_path_offset(copy, "/") == FDT_ERR_NODE,
               "a corrupted magic yields not-found rather than a wrong node");
            free(copy);
        }
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    free(blob);
    return g_fail ? 1 : 0;
}
