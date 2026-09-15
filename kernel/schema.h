#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PTR_SCHEMA_ANY,        /* any structurally-sound pointer accepted, unchecked further */
    PTR_SCHEMA_STRUCT,
    PTR_SCHEMA_LIST,
    PTR_SCHEMA_TEXT,       /* List(UInt8), element_size forced to byte */
    PTR_SCHEMA_DATA,       /* List(UInt8), same wire shape as TEXT */
    PTR_SCHEMA_CAPABILITY,
} ptr_schema_kind_t;

typedef struct struct_schema struct_schema_t;

typedef struct {
    ptr_schema_kind_t kind;
    /* For STRUCT, or LIST whose elements are structs (list_element_size
     * == 7 / composite): the shape required of the pointed-to struct(s).
     * NULL means "no further requirement on that struct's own shape". */
    const struct_schema_t *element_struct;
    /* For LIST only: required Cap'n Proto element-size tag (0=void,
     * 1=bit, 2=byte, 3=2-byte, 4=4-byte, 5=8-byte-nonptr, 6=8-byte-ptr,
     * 7=composite). 0xFF means "any element size accepted". Ignored for
     * non-LIST kinds. */
    uint8_t list_element_size;
} pointer_schema_t;

struct struct_schema {
    uint16_t data_words;    /* minimum required data-section size, in words */
    uint16_t pointer_count; /* number of pointer slots this schema checks */
    const pointer_schema_t *pointers; /* array of length pointer_count */
};

typedef struct {
    uint32_t method_id;
    /* NULL param_schema/result_schema means "no validation performed
     * for that side" -- an object can decline the kernel's validation
     * for a given method entirely, at the cost of having to do its own
     * bounds-checking on whatever it reads out of an unvalidated
     * buffer. This is an explicit, visible opt-out, not a silent gap. */
    const struct_schema_t *param_schema;
    const struct_schema_t *result_schema;
} method_schema_t;

typedef struct {
    const method_schema_t *methods;
    size_t method_count;
} object_schema_t;
