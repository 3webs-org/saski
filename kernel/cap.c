#include "arch_x86_64.h"
#include "cap.h"
#include "capnp_validate.h"
#include "serial.h"

/* No kernel-owned capability table anymore -- see cap.h's own note on
 * CREATE's new caller-supplied-storage parameter for why. A handle IS
 * the address of its own cap_record_t -- including the factory
 * object's own handle (g_factory_record below), which is no longer a
 * reserved magic value; it's a genuine address like any other, the
 * kernel just happens to own the storage it points at, since it's the
 * one record that bootstraps before any caller exists to supply
 * storage for it. kernel_invoke does not special-case it at all --
 * see that function's own dispatch below.
 *
 * magic exists purely as an integrity check, not a security boundary
 * (none exists yet -- see cap.h's own note): it catches an honest
 * mistake (a stale, zeroed, or otherwise-wrong handle) with a clear
 * diagnostic instead of the kernel silently trusting whatever bytes
 * happen to sit at an arbitrary address. */
#define CAP_RECORD_MAGIC 0xCAB1E000CAB1E000ull

typedef struct {
    uint64_t magic;
    int valid;
    object_handler_fn handler;
    void *data;
    const object_schema_t *schema;
} cap_record_t;

_Static_assert(sizeof(cap_record_t) <= CAP_RECORD_SIZE,
              "cap_record_t grew past what cap.h promises callers");

static cap_record_t g_factory_record;

/* Protects a record's own fields from a concurrent read racing a
 * concurrent destroy -- genuinely needed the moment more than one core
 * is up, since two cores' kernel_invoke calls can target the very same
 * handle simultaneously. Global rather than per-record: simple, still
 * fully correct (a record is only ever touched while holding it), just
 * coarser than the finest grain possible -- the same tradeoff this
 * project has made elsewhere (the PIC/PIT hardware mutex, for one)
 * rather than a deliberate performance decision. Never held across a
 * handler call, for the same reason as always: a handler may itself
 * call kernel_invoke, and holding this across that would self-deadlock. */
static volatile int g_cap_lock = 0;

static void cap_lock(void)
{
    while (__sync_lock_test_and_set(&g_cap_lock, 1)) {
        while (g_cap_lock) {
            /* spin without retrying the atomic op until it looks free --
             * cheaper than hammering a shared cache line with locked
             * instructions from multiple cores */
        }
    }
}

static void cap_unlock(void)
{
    __sync_lock_release(&g_cap_lock);
}

/* ---- Object 0's own wire protocol ----
 *
 * CREATE (method 0): {handlerFn: UInt64, handlerData: UInt64,
 * schemaPtr: UInt64} -> {handle: UInt64}. Embedding raw native pointers
 * as plain data-section integers is a deliberate, flagged simplification:
 * it's only safe because nothing in this system is isolated from
 * anything else yet (ring-0 monolithic, one flat address space) -- a
 * genuinely untrusted caller being able to hand the kernel an arbitrary
 * "function pointer" to jump to would be catastrophic the moment
 * isolation exists. The correct fix at that point is restricting who can
 * reach the factory object's create method at all, or routing object construction
 * through a trusted loader that verifies code rather than accepting a
 * bare pointer -- tracked as follow-up work, not silently assumed safe
 * forever.
 *
 * DESTROY (method 1): {target: UInt64} -> (no response). Frees target's
 * table slot outright; there is no per-object authorization hook for
 * "who may destroy me" here anymore -- an object that wants that
 * protection implements its own method that does the check and then
 * invokes the factory object's destroy itself, exactly the same "wrapping is just
 * another object" pattern as everything else. */

#define FACTORY_METHOD_CREATE 0
#define FACTORY_METHOD_DESTROY 1

static const struct_schema_t g_create_request_schema = {
    .data_words = 4, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t g_create_response_schema = {
    .data_words = 1, .pointer_count = 0, .pointers = NULL};
static const struct_schema_t g_destroy_request_schema = {
    .data_words = 1, .pointer_count = 0, .pointers = NULL};

/* These three leaf schemas have no pointer fields of their own, so they
 * stay ordinary compile-time const data -- genuinely position-independent
 * already, nothing to fix up regardless of load address.
 *
 * g_factory_methods/g_factory_schema are different: initializing
 * them with &g_create_request_schema (or an array-decay pointer, for
 * .methods) at compile time would bake in an ABSOLUTE address computed
 * for a load base of exactly 1M (this kernel's current fixed link
 * address) -- correct only by coincidence of always loading there. The
 * moment a second kernel instance (a second core, in a multikernel
 * design) needs to load at a different physical address, that baked
 * address would be silently wrong, and Mach-O/ELF relocation-table
 * processing to fix it up doesn't exist in either of this project's own
 * from-scratch loaders. Populating these at runtime instead sidesteps
 * the whole problem: the assignment below is an ordinary RIP-relative
 * store, correct no matter where this kernel image actually ended up
 * running from -- no relocation table, no loader changes, anywhere. */
static method_schema_t g_factory_methods[2];
static object_schema_t g_factory_schema;

static void init_factory_schema(void)
{
    g_factory_methods[0].method_id = FACTORY_METHOD_CREATE;
    g_factory_methods[0].param_schema = &g_create_request_schema;
    g_factory_methods[0].result_schema = &g_create_response_schema;

    g_factory_methods[1].method_id = FACTORY_METHOD_DESTROY;
    g_factory_methods[1].param_schema = &g_destroy_request_schema;
    g_factory_methods[1].result_schema = NULL;

    g_factory_schema.methods = g_factory_methods;
    g_factory_schema.method_count = 2;
}

static void factory_handler(void *data, uint32_t method_id,
                                const void *msg, size_t len, void *response,
                                size_t response_cap, size_t *out_response_len)
{
    (void)data;
    (void)len;
    *out_response_len = 0;

    if (method_id == FACTORY_METHOD_CREATE) {
        /* Data words start 16 bytes in: 8-byte segment header + 8-byte
         * root struct pointer come first, per the flat encoding every
         * caller here uses (see coreinit's capnp_build_flat, which this
         * mirrors on the read side). Reading from byte 0 instead would
         * pick up the message's own framing bytes as if they were the
         * handler/data/schema pointers -- exactly the bug this comment
         * exists to make impossible to reintroduce silently. */
        const uint64_t *words = (const uint64_t *)((const uint8_t *)msg + 16);
        object_handler_fn handler = (object_handler_fn)(uintptr_t)words[0];
        void *handler_data = (void *)(uintptr_t)words[1];
        const object_schema_t *schema =
            (const object_schema_t *)(uintptr_t)words[2];
        void *record_storage = (void *)(uintptr_t)words[3];

        if (!record_storage) {
            kprintf("[kernel] factory: create failed, no record storage "
                    "supplied\n");
            return;
        }

        cap_record_t *r = (cap_record_t *)record_storage;
        cap_lock();
        r->magic = CAP_RECORD_MAGIC;
        r->valid = 1;
        r->handler = handler;
        r->data = handler_data;
        r->schema = schema;
        cap_unlock();

        uint64_t handle = (uint64_t)(uintptr_t)record_storage;
        if (response_cap >= 24) {
            uint8_t *out = (uint8_t *)response;
            /* segment count - 1 = 0 */
            out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
            /* segment 0 word count = 2 (root ptr + 1 data word) */
            out[4] = 2; out[5] = 0; out[6] = 0; out[7] = 0;
            /* root pointer: struct, offset 0, dataWords=1, ptrWords=0 */
            uint64_t *out_words = (uint64_t *)(out + 8);
            out_words[0] = ((uint64_t)1) << 32;
            out_words[1] = handle;
            *out_response_len = 24;
        }
        kprintf("[kernel] factory: created handle 0x%X\n", (unsigned)handle);
        return;
    }

    if (method_id == FACTORY_METHOD_DESTROY) {
        const uint64_t *words = (const uint64_t *)((const uint8_t *)msg + 16);
        uint64_t target = words[0];
        if (target == (uint64_t)(uintptr_t)&g_factory_record) {
            /* The factory object itself is permanently undestroyable --
             * same refusal as before this became an ordinary-looking
             * handle, just compared against its real address now
             * rather than the old reserved value 0 (which no longer
             * means anything special at all -- see cap.h's own note). */
            kprintf("[kernel] factory: destroy refused for the factory "
                    "object's own handle\n");
            return;
        }
        cap_record_t *r = (cap_record_t *)(uintptr_t)target;

        cap_lock();
        if (r->magic != CAP_RECORD_MAGIC || !r->valid) {
            /* Double-destroy guard, and catches a handle that was never
             * a real record in the first place -- without the magic
             * check specifically, destroying the same handle twice
             * would silently succeed twice, which is harmless here only
             * because the caller (not the kernel) owns the underlying
             * memory; the check still catches a clear caller mistake
             * early with a real diagnostic instead of not at all. */
            cap_unlock();
            kprintf("[kernel] factory: destroy refused for handle 0x%X, "
                    "not a live capability\n",
                    (unsigned)target);
            return;
        }
        kprintf("[kernel] factory: destroying handle 0x%X\n",
                (unsigned)target);
        r->valid = 0;
        r->magic = 0;
        r->handler = NULL;
        r->data = NULL;
        r->schema = NULL;
        cap_unlock();
        return;
    }

    kprintf("[kernel] factory: unknown method %u\n", (unsigned)method_id);
}

initial_capabilities_t kernel_cap_bootstrap(void)
{
    init_factory_schema();

    g_factory_record.magic = CAP_RECORD_MAGIC;
    g_factory_record.valid = 1;
    g_factory_record.handler = factory_handler;
    g_factory_record.data = NULL;
    g_factory_record.schema = &g_factory_schema;

    initial_capabilities_t caps;
    caps.count = 1; /* the factory object itself */
    caps.factory.handle = (uint64_t)(uintptr_t)&g_factory_record;
    return caps;
}

static const method_schema_t *find_method(const object_schema_t *schema,
                                          uint32_t method_id)
{
    for (size_t i = 0; i < schema->method_count; i++) {
        if (schema->methods[i].method_id == method_id) {
            return &schema->methods[i];
        }
    }
    return NULL;
}

int kernel_invoke(capability_t target, uint32_t method_id, const void *msg,
                  size_t len, void *response, size_t response_cap,
                  size_t *out_response_len)
{
    size_t dummy_len;
    if (!out_response_len) {
        out_response_len = &dummy_len;
    }
    *out_response_len = 0;

    /* No special case for the factory object anymore -- its handle IS
     * &g_factory_record (see kernel_cap_bootstrap), a real address like
     * any other caller's. Dispatch is uniform for every handle. */
    cap_record_t *target_record = (cap_record_t *)(uintptr_t)target.handle;

    /* Copy the record while holding the lock, then release before doing
     * anything else -- validation and the handler call both potentially
     * take a while (a handler may itself call kernel_invoke again), and
     * holding a lock across that would block every other core's
     * completely unrelated invocations, or deadlock a re-entrant one
     * outright. Unlike before, a handle CAN now be reused the instant
     * its underlying storage is destroyed and repurposed by its owning
     * caller for a brand new object -- there is no kernel-side handle
     * generation counter yet to detect that (a real fix needs one on
     * capability_t itself, touching every object in this codebase, and
     * is tracked as separate, larger follow-up work, not silently
     * assumed solved here). magic still catches the simple case of a
     * stale or garbage handle; it does not catch a handle that got
     * legitimately reused for something else between two calls. */
    cap_lock();
    if (target_record->magic != CAP_RECORD_MAGIC || !target_record->valid) {
        cap_unlock();
        kprintf("[kernel] kernel_invoke: invalid capability 0x%X\n",
                (unsigned)target.handle);
        return 0;
    }
    cap_record_t record = *target_record;
    cap_unlock();

    const method_schema_t *method = find_method(record.schema, method_id);
    if (!method) {
        kprintf("[kernel] kernel_invoke: handle 0x%X has no method %u\n",
                (unsigned)target.handle, (unsigned)method_id);
        return 0;
    }

    if (!capnp_validate_message(msg, len, method->param_schema)) {
        kprintf("[kernel] kernel_invoke: request to handle %u method %u "
                "failed structural validation, REJECTED before dispatch\n",
                (unsigned)target.handle, (unsigned)method_id);
        return 0;
    }

    record.handler(record.data, method_id, msg, len, response, response_cap,
                    out_response_len);

    if (method->result_schema &&
        !capnp_validate_message(response, *out_response_len,
                                method->result_schema)) {
        kprintf("[kernel] kernel_invoke: response from handle %u method %u "
                "failed structural validation -- object claimed a shape it "
                "did not deliver, response DISCARDED\n",
                (unsigned)target.handle, (unsigned)method_id);
        for (size_t i = 0; i < response_cap; i++) {
            ((uint8_t *)response)[i] = 0;
        }
        *out_response_len = 0;
        return 0;
    }

    return 1;
}
