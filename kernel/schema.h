#pragma once

#include <stddef.h>
#include <stdint.h>

/* Object schema arrays are COMPILE-TIME CONSTANT DATA.
 *
 * Relationships between entries are indices rather than pointers, so the
 * arrays need no relocation when another core runs a byte-for-byte copy of
 * this kernel. A schema_table_t still holds four array pointers: a copied
 * instance must rebind that small descriptor to its OWN const arrays at
 * bootstrap (see cap.c), not retain the BSP's absolute pointer values.
 * There is no runtime schema parser or per-object schema allocation.
 *
 * Everything is resolved against a schema_table_t, which owns the four flat
 * arrays. A resolved schema is therefore always a (table, index) pair -- an
 * index alone means nothing. */

typedef enum {
    PTR_SCHEMA_ANY,        /* any structurally-sound pointer accepted, unchecked further */
    PTR_SCHEMA_STRUCT,
    PTR_SCHEMA_LIST,
    PTR_SCHEMA_TEXT,       /* List(UInt8), element_size forced to byte */
    PTR_SCHEMA_DATA,       /* List(UInt8), same wire shape as TEXT */
    PTR_SCHEMA_CAPABILITY,
} ptr_schema_kind_t;

/* "no such entry". Deliberately not 0, which is a real first entry. */
#define SCHEMA_IDX_NONE 0xFFFFu

typedef uint16_t schema_idx_t;

typedef struct {
    ptr_schema_kind_t kind;
    /* For STRUCT, or a LIST whose elements are structs (list_element_size
     * == 7 / composite): the index of the shape required of the pointed-to
     * struct(s), or SCHEMA_IDX_NONE for no further requirement. */
    schema_idx_t element_struct;
    /* For LIST only: required Cap'n Proto element-size tag (0=void, 1=bit,
     * 2=byte, 3=2-byte, 4=4-byte, 5=8-byte-nonptr, 6=8-byte-ptr, 7=composite).
     * 0xFF means any element size accepted. Ignored for non-LIST kinds. */
    uint8_t list_element_size;
} pointer_schema_t;

typedef struct {
    uint16_t data_words;    /* minimum required data-section size, in words */
    uint16_t pointer_count; /* number of pointer slots this schema checks */
    /* Index of the first of this struct's pointer_count entries in the table's
     * pointer array, or SCHEMA_IDX_NONE when pointer_count is 0. */
    schema_idx_t first_pointer;
} struct_schema_t;

typedef struct {
    uint32_t method_id;
    /* SCHEMA_IDX_NONE means "no validation performed for that side" -- an
     * object can decline the kernel's validation for a given method entirely,
     * at the cost of doing its own bounds-checking on whatever it reads out of
     * an unvalidated buffer. A visible opt-out, not a silent gap. */
    schema_idx_t param_schema;
    schema_idx_t result_schema;
} method_schema_t;

typedef struct {
    schema_idx_t first_method; /* index into the table's method array */
    uint16_t method_count;
} object_schema_t;

typedef struct {
    const struct_schema_t *structs;
    uint16_t struct_count;
    const pointer_schema_t *pointers;
    uint16_t pointer_count;
    const method_schema_t *methods;
    uint16_t method_count;
    const object_schema_t *objects;
    uint16_t object_count;
} schema_table_t;

/* Bounds-checked accessors. A malformed index is a rejection, never a read past
 * the end of a table: a caller can supply its own schema table, so the indices
 * inside it are as untrusted as the message that carries them. */
static inline const struct_schema_t *schema_struct(const schema_table_t *t,
                                                   schema_idx_t i)
{
    if (!t || i == SCHEMA_IDX_NONE || i >= t->struct_count) {
        return NULL;
    }
    return &t->structs[i];
}

static inline const pointer_schema_t *schema_pointer(const schema_table_t *t,
                                                     schema_idx_t i)
{
    if (!t || i == SCHEMA_IDX_NONE || i >= t->pointer_count) {
        return NULL;
    }
    return &t->pointers[i];
}

static inline const object_schema_t *schema_object(const schema_table_t *t,
                                                   schema_idx_t i)
{
    if (!t || i == SCHEMA_IDX_NONE || i >= t->object_count) {
        return NULL;
    }
    return &t->objects[i];
}

static inline const method_schema_t *schema_method(const schema_table_t *t,
                                                   schema_idx_t i)
{
    if (!t || i == SCHEMA_IDX_NONE || i >= t->method_count) {
        return NULL;
    }
    return &t->methods[i];
}
