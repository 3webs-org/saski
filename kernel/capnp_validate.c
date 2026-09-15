#include "capnp_validate.h"
#include "capnp.h"
#include <stdint.h>

#define CAPNP_MAX_DEPTH 16

typedef struct {
    const uint8_t *seg;
    uint64_t seg_words;
} capnp_msg_t;

static int in_bounds(const capnp_msg_t *m, int64_t word_offset, uint64_t word_count)
{
    if (word_offset < 0) {
        return 0;
    }
    if ((uint64_t)word_offset > m->seg_words) {
        return 0;
    }
    if (word_count > m->seg_words - (uint64_t)word_offset) {
        return 0;
    }
    return 1;
}

static uint64_t read_word(const capnp_msg_t *m, int64_t word_offset)
{
    const uint8_t *p = m->seg + word_offset * 8;
    uint64_t w = 0;
    for (int i = 0; i < 8; i++) {
        w |= ((uint64_t)p[i]) << (8 * i);
    }
    return w;
}

static int validate_struct_at(const capnp_msg_t *m, int64_t struct_word_offset,
                              uint16_t data_words, uint16_t ptr_words,
                              const struct_schema_t *schema, int depth);

static int validate_pointer_slot(const capnp_msg_t *m, int64_t ptr_word_offset,
                                 const pointer_schema_t *pschema, int depth)
{
    if (depth > CAPNP_MAX_DEPTH) {
        return 0;
    }
    if (!in_bounds(m, ptr_word_offset, 1)) {
        return 0;
    }

    uint64_t word = read_word(m, ptr_word_offset);
    capnp_ptr_t ptr = capnp_decode_ptr(word);

    if (ptr.tag == CAPNP_PTR_NULL) {
        /* Cap'n Proto's own evolution rules: an absent pointer is
         * always a valid default value for any declared type -- never
         * a validation failure. */
        return 1;
    }

    if (pschema->kind == PTR_SCHEMA_ANY) {
        return ptr.tag != CAPNP_PTR_FAR;
    }
    if (ptr.tag == CAPNP_PTR_FAR) {
        return 0; /* not resolved -- see module scope note */
    }

    /* struct/list offsets are relative to the word immediately AFTER
     * the pointer word itself. */
    int64_t target_offset = ptr_word_offset + 1 + ptr.offset_words;

    switch (pschema->kind) {
    case PTR_SCHEMA_CAPABILITY:
        return ptr.tag == CAPNP_PTR_CAPABILITY;

    case PTR_SCHEMA_STRUCT: {
        if (ptr.tag != CAPNP_PTR_STRUCT) {
            return 0;
        }
        uint64_t total = (uint64_t)ptr.data_words + ptr.ptr_words;
        if (!in_bounds(m, target_offset, total)) {
            return 0;
        }
        return validate_struct_at(m, target_offset, ptr.data_words,
                                  ptr.ptr_words, pschema->element_struct,
                                  depth + 1);
    }

    case PTR_SCHEMA_TEXT:
    case PTR_SCHEMA_DATA: {
        if (ptr.tag != CAPNP_PTR_LIST || ptr.element_size != 2) {
            return 0;
        }
        uint64_t byte_count = ptr.element_count;
        uint64_t word_count = (byte_count + 7) / 8;
        return in_bounds(m, target_offset, word_count);
    }

    case PTR_SCHEMA_LIST: {
        if (ptr.tag != CAPNP_PTR_LIST) {
            return 0;
        }
        if (pschema->list_element_size != 0xFFu &&
            ptr.element_size != pschema->list_element_size) {
            return 0;
        }

        if (ptr.element_size == 7) {
            /* Composite list: the count field is repurposed as the
             * total WORD COUNT of (tag word + all elements). The list
             * body begins with a tag word, itself shaped like a struct
             * pointer, whose "offset" field is repurposed as the true
             * element count and whose data/ptr word counts describe
             * every element uniformly. */
            uint64_t total_words = ptr.element_count;
            if (!in_bounds(m, target_offset, total_words + 1)) {
                return 0;
            }
            uint64_t tag_word = read_word(m, target_offset);
            capnp_ptr_t tag = capnp_decode_ptr(tag_word);
            uint32_t elem_count = (uint32_t)tag.offset_words;
            uint64_t elem_words = (uint64_t)tag.data_words + tag.ptr_words;

            if (elem_words != 0 &&
                (uint64_t)elem_count > total_words / elem_words) {
                return 0; /* elem_count * elem_words would overflow the
                          * declared total -- reject rather than trust it */
            }
            if (elem_words * (uint64_t)elem_count > total_words) {
                return 0;
            }

            for (uint32_t i = 0; i < elem_count; i++) {
                int64_t elem_offset =
                    target_offset + 1 + (int64_t)(i * elem_words);
                if (!validate_struct_at(m, elem_offset, tag.data_words,
                                        tag.ptr_words, pschema->element_struct,
                                        depth + 1)) {
                    return 0;
                }
            }
            return 1;
        }

        /* Non-composite list: bounds-check the byte extent only.
         * Per-element structural validation for non-struct element
         * kinds is out of scope for this milestone. */
        {
            static const uint8_t bits_per_elem[8] = {0, 1, 8, 16, 32, 64, 64, 0};
            uint64_t total_bits =
                (uint64_t)bits_per_elem[ptr.element_size] * ptr.element_count;
            uint64_t total_words = (total_bits + 63) / 64;
            return in_bounds(m, target_offset, total_words);
        }
    }

    default:
        return 0;
    }
}

static int validate_struct_at(const capnp_msg_t *m, int64_t struct_word_offset,
                              uint16_t data_words, uint16_t ptr_words,
                              const struct_schema_t *schema, int depth)
{
    if (!schema) {
        return 1;
    }
    if (data_words < schema->data_words) {
        return 0; /* fewer data words present than the schema requires:
                   * reject rather than read past what's actually there */
    }

    uint16_t slots_to_check = schema->pointer_count;
    if (slots_to_check > ptr_words) {
        /* Fewer pointer slots present than the schema declares: the
         * missing ones are equivalent to null per Cap'n Proto's own
         * evolution model, which validate_pointer_slot's null handling
         * already accepts -- simply not checking them has the same
         * effect as checking and finding null. */
        slots_to_check = ptr_words;
    }

    int64_t ptr_section_start = struct_word_offset + data_words;
    for (uint16_t i = 0; i < slots_to_check; i++) {
        if (!validate_pointer_slot(m, ptr_section_start + i,
                                   &schema->pointers[i], depth)) {
            return 0;
        }
    }
    return 1;
}

int capnp_validate_message(const void *msg, size_t len,
                           const struct_schema_t *schema)
{
    if (!schema) {
        return 1;
    }
    if (!msg || len < 8) {
        return 0;
    }

    const uint8_t *bytes = (const uint8_t *)msg;

    uint32_t segment_count_minus_1 =
        (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
        ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
    uint32_t segment_count = segment_count_minus_1 + 1;

    if (segment_count != 1) {
        return 0; /* multi-segment: out of scope, see header comment */
    }

    uint32_t seg0_words =
        (uint32_t)bytes[4] | ((uint32_t)bytes[5] << 8) |
        ((uint32_t)bytes[6] << 16) | ((uint32_t)bytes[7] << 24);

    size_t header_bytes = 4 + 4 * (size_t)segment_count;
    if (header_bytes % 8 != 0) {
        header_bytes += 4;
    }

    size_t seg0_bytes = (size_t)seg0_words * 8;
    if (len < header_bytes || len - header_bytes < seg0_bytes) {
        return 0;
    }

    capnp_msg_t m;
    m.seg = bytes + header_bytes;
    m.seg_words = seg0_words;

    if (m.seg_words < 1) {
        return 0;
    }

    uint64_t root_word = read_word(&m, 0);
    capnp_ptr_t root = capnp_decode_ptr(root_word);

    if (root.tag == CAPNP_PTR_NULL) {
        /* A null root is a validly-encoded "all fields default"
         * message; whether zero data/pointer words satisfies the
         * schema is exactly what validate_struct_at already checks. */
        return validate_struct_at(&m, 0, 0, 0, schema, 0);
    }
    if (root.tag != CAPNP_PTR_STRUCT) {
        return 0; /* the root of a message must be a struct, per spec */
    }

    int64_t target_offset = 1 + root.offset_words;
    uint64_t total = (uint64_t)root.data_words + root.ptr_words;
    if (!in_bounds(&m, target_offset, total)) {
        return 0;
    }
    return validate_struct_at(&m, target_offset, root.data_words,
                              root.ptr_words, schema, 0);
}
