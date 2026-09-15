#pragma once

#include "ownership.h"

/* Whether the CPU is currently executing in 64-bit long mode is a fact
 * about the current path, not about any particular value -- exactly the
 * thread-scoped case ownership.h's grants_thread_token/requires_thread_token
 * (and, as of the calgebra-lints patch this project upstreamed, their
 * drops_thread_token/requires_thread_token_absent counterparts) exist
 * for. Enforced generically (not errno-specific) by ntlibc.ErrnoDiscipline's
 * checkPreCall/checkPostCall extension -- verified end to end against
 * this exact header, see kernel/mode_token_test.c and mode_token_test2.c. */
tokdef mode64_token;

/* Transitions the CPU from 32-bit protected mode into 64-bit long mode.
 * Implemented in boot.S; never returns via a normal C return (the far
 * jump lands directly in 64-bit code), which is why this is _Noreturn --
 * and why, correctly, the analyzer treats anything "after" a call to
 * this in the same C function as unreachable. No C call edge actually
 * connects "before" this transition to "after" it in this kernel's
 * current structure: nothing in C runs before it at all. */
grants_thread_token(mode64_token)
_Noreturn void enter_long_mode(void);

/* The reverse transition: drops back to 32-bit protected mode. Requires
 * holding mode64_token, and drops it on completion. Not currently
 * called anywhere -- bring-up is strictly one-way today. */
requires_thread_token(mode64_token)
drops_thread_token(mode64_token)
void enter_protected_mode(void);

/* The shape a hypothetical portable (non-amd64) helper would use. No
 * such helper exists yet. */
#define i386_only requires_thread_token_absent(mode64_token)
