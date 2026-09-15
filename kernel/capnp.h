#pragma once

#include <stdint.h>

/* Cap'n Proto pointer encoding (capnproto.org/encoding.html), decoded
 * from a raw 64-bit little-endian pointer word. The bottom 2 bits are a
 * tag: 0 = struct, 1 = list, 2 = far pointer, 3 = capability. Layout of
 * the remaining bits differs per tag -- see each decode function below.
 * This module only decodes; it does not interpret what the pointed-to
 * bytes *mean*, that's capnp_validate.c's job, working from a
 * kernel-declared schema. */

typedef enum {
    CAPNP_PTR_NULL = -1, /* all-zero word: the well-known null pointer */
    CAPNP_PTR_STRUCT = 0,
    CAPNP_PTR_LIST = 1,
    CAPNP_PTR_FAR = 2,
    CAPNP_PTR_CAPABILITY = 3,
} capnp_ptr_tag_t;

typedef struct {
    capnp_ptr_tag_t tag;
    int32_t offset_words; /* signed; struct/list only */
    /* struct */
    uint16_t data_words;
    uint16_t ptr_words;
    /* list (ignored for struct) */
    uint8_t element_size; /* 0=void 1=bit 2=byte 3=2byte 4=4byte 5=8byte
                           * 6=8byte-pointer 7=composite */
    uint32_t element_count; /* or, if element_size==7, total word count
                             * of the composite list body (tag word +
                             * elements) -- see capnp_validate.c for how
                             * that's actually split into element count
                             * vs per-element size via the tag word */
    /* far pointer */
    uint8_t far_double;
    uint32_t far_segment_id;
    /* capability */
    uint32_t cap_index;
} capnp_ptr_t;

static inline capnp_ptr_t capnp_decode_ptr(uint64_t word)
{
    capnp_ptr_t p;
    p.offset_words = 0;
    p.data_words = 0;
    p.ptr_words = 0;
    p.element_size = 0;
    p.element_count = 0;
    p.far_double = 0;
    p.far_segment_id = 0;
    p.cap_index = 0;

    if (word == 0) {
        p.tag = CAPNP_PTR_NULL;
        return p;
    }

    uint32_t tag = (uint32_t)(word & 0x3u);
    switch (tag) {
    case 0: /* struct pointer */
        p.tag = CAPNP_PTR_STRUCT;
        p.offset_words = (int32_t)(word >> 2) & 0x3FFFFFFF;
        /* sign-extend 30-bit field */
        if (p.offset_words & 0x20000000) {
            p.offset_words |= (int32_t)0xC0000000u;
        }
        p.data_words = (uint16_t)((word >> 32) & 0xFFFFu);
        p.ptr_words = (uint16_t)((word >> 48) & 0xFFFFu);
        break;
    case 1: /* list pointer */
        p.tag = CAPNP_PTR_LIST;
        p.offset_words = (int32_t)(word >> 2) & 0x3FFFFFFF;
        if (p.offset_words & 0x20000000) {
            p.offset_words |= (int32_t)0xC0000000u;
        }
        p.element_size = (uint8_t)((word >> 32) & 0x7u);
        p.element_count = (uint32_t)((word >> 35) & 0x1FFFFFFFu);
        break;
    case 2: /* far pointer */
        p.tag = CAPNP_PTR_FAR;
        p.far_double = (uint8_t)((word >> 2) & 0x1u);
        p.offset_words = (int32_t)((word >> 3) & 0x1FFFFFFFu);
        p.far_segment_id = (uint32_t)((word >> 32) & 0xFFFFFFFFu);
        break;
    default: /* 3: capability pointer */
        p.tag = CAPNP_PTR_CAPABILITY;
        p.cap_index = (uint32_t)((word >> 32) & 0xFFFFFFFFu);
        break;
    }
    return p;
}
