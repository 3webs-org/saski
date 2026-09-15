#pragma once

#include <stddef.h>
#include <stdint.h>
#include "schema.h"

/* The kernel's ENTIRE syscall surface is now one function: kernel_invoke.
 * There is no separate spawn or destroy primitive anymore -- creating and
 * destroying objects are just invocations of object 0's own methods,
 * validated the same way any other invocation is. Wrapping is no longer
 * kernel-tracked data (no more stage chains): an object that wants to
 * wrap another is just an ordinary object whose handler, when invoked,
 * does some work and then invokes the wrapped object itself. Composing
 * wrappers is composing objects, nothing more.
 *
 * The kernel DOES validate every invocation's message structurally
 * against the schema declared when the target object was created (see
 * schema.h, capnp_validate.h) -- rejecting a call before it ever reaches
 * the object's handler if the message can't possibly be read as the
 * declared type without going out of bounds. This is shape validation
 * only: it proves a message CAN be read safely as its declared type, not
 * that its field values make application-level sense. The kernel also
 * validates the object's own response against its declared result shape
 * before handing it back to the caller, protecting the caller from a
 * buggy or malicious object that claims a shape it doesn't deliver.
 *
 * capability_t is still just an opaque, kernel-local handle -- not yet
 * unforgeable in the security sense, for the same reason as always: no
 * runtime isolation boundary exists yet to make that meaningful. */

typedef struct capability {
    uint64_t handle;
} capability_t;

typedef struct initial_capabilities {
    size_t count;
    capability_t factory; /* the object-creation ("factory") object's
                           * own handle -- an ordinary handle like any
                           * other, not a magic reserved value; see
                           * kernel_cap_bootstrap's own comment for why
                           * this is the one thing that has to be
                           * handed over explicitly rather than
                           * discovered, and init_abi.h's own boot_info
                           * for where it actually travels to init. */
} initial_capabilities_t;

/* CREATE (the factory object, method 0) now takes a FOURTH word: a
 * pointer to
 * storage the CALLER allocates and owns, at least CAP_RECORD_SIZE bytes,
 * aligned to CAP_RECORD_ALIGN. The kernel writes its own bookkeeping
 * into that storage and hands back a handle that simply IS that
 * pointer -- there is no kernel-owned capability table, no fixed
 * ceiling on how many objects can ever exist, and nothing for the
 * kernel to grow or run out of. The kernel's job stays purely "validate
 * and dispatch"; sourcing the bytes any given object's bookkeeping
 * lives in is the caller's problem, same as it already owns sourcing
 * the memory for the object's own data and schema. A caller with a real
 * allocator (mem_mgr, once it exists) gets genuinely unbounded object
 * creation for free; a caller bootstrapping before one exists (or
 * creating a small, fixed number of objects once) can use a static
 * array of CAP_RECORD_SIZE-byte slots just as validly -- the kernel
 * doesn't know or care which.
 *
 * This IS the same trust relaxation this file already accepted for
 * handlerFn (a caller has always been able to hand the kernel an
 * arbitrary function pointer to jump to); letting the caller supply the
 * BOOKKEEPING memory too doesn't cross into new territory, it just
 * removes the one thing that was still kernel-owned and therefore
 * kernel-bounded. */
#define CAP_RECORD_SIZE 64
#define CAP_RECORD_ALIGN 16

/* The object-creation object (create/destroy) is an ordinary object,
 * exactly like anything it creates -- its handle is the real address
 * of its own cap_record_t, not a reserved magic value. Its backing
 * storage still has to live somewhere the kernel itself owns (see
 * cap.c's own g_factory_record): it's the one object that bootstraps
 * before any caller exists to supply storage for it, the same
 * necessary exception kernel_cap_bootstrap's own comment already
 * describes. What's different from an earlier version of this design
 * is that nothing about ITS IDENTITY is baked into a shared header
 * anymore -- init doesn't know its handle by convention, it's handed
 * one explicitly, in initial_capabilities_t.factory (see
 * kernel_cap_bootstrap below and include/init_abi.h's own boot_info).
 * This removes the one special case kernel_invoke used to have
 * (dispatch no longer branches on "is this handle 0") and the one
 * thing this project's own generic-init principle (see init_abi.h)
 * hadn't yet caught up to: the kernel no longer expects init to know
 * anything about it ahead of time, including this. */

/* An object's handler: called by the kernel after validating msg/len
 * against the method's declared param_schema, and before the kernel
 * validates whatever this writes into response against result_schema.
 * data is the opaque pointer supplied when the object was created --
 * the kernel never interprets it. The handler reports how many bytes of
 * `response` it actually wrote via *out_response_len (0 if none); the
 * kernel never assumes response_cap bytes were all written. */
typedef void (*object_handler_fn)(void *data, uint32_t method_id,
                                  const void *msg, size_t len, void *response,
                                  size_t response_cap, size_t *out_response_len);

/* Constructs object 0 and the initial capability set. Called exactly
 * once, by the kernel's own boot trampoline, before control passes to
 * init. This is the one deliberate exception to "no ambient authority":
 * something has to receive the initial grant, or nothing could ever
 * bootstrap. Every other capability in the system traces its authority
 * back to invocations of object 0, directly or transitively. */
initial_capabilities_t kernel_cap_bootstrap(void);

/* The one syscall. Looks up target's record, finds method_id within its
 * declared schema, validates msg/len against that method's param_schema
 * (rejecting before dispatch if it doesn't structurally conform), calls
 * the object's handler, then validates whatever the handler wrote into
 * response against the method's result_schema (zeroing the response and
 * reporting failure if the object didn't deliver the shape it declared).
 * Returns 1 on success, 0 on any failure (invalid capability, unknown
 * method, request validation failure, or response validation failure).
 *
 * Whether the callee is in the same privilege context, a different
 * address space, or a different physical node is not visible here --
 * that's the object's own handler's business, not this function's. */
int kernel_invoke(capability_t target, uint32_t method_id, const void *msg,
                  size_t len, void *response, size_t response_cap,
                  size_t *out_response_len);
