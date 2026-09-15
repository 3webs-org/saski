#pragma once

#include <stddef.h>
#include "schema.h"

/* Validates that `msg`/`len` is a well-formed, single-segment Cap'n
 * Proto message whose root struct structurally conforms to `schema`:
 * every pointer slot the schema declares resolves to data of the
 * declared kind, entirely within the message's own bounds. This is
 * SHAPE validation only -- it never inspects field values for
 * application-level sense, only that reading the message as the
 * declared type can never read out of bounds. Returns 1 if valid, 0 if
 * not. A NULL schema means "no validation requested" and always
 * returns 1 -- an object can opt out explicitly (see schema.h), but the
 * kernel never opts out silently on its own.
 *
 * SCOPE, stated plainly, matching this project's own established
 * practice of not silently claiming more than is implemented:
 *   - Single-segment messages only. A message header declaring more
 *     than one segment is rejected outright, not partially validated.
 *   - Far pointers are never resolved (there is nowhere else to resolve
 *     them TO, with only one segment) -- a far pointer anywhere in a
 *     validated region is a rejection.
 *   - Capability pointers are checked for correct tag only; the pointed-
 *     to capability index is not resolved against any message-level
 *     capability table. Message-embedded capability transfer is real
 *     OCapN/CapTP territory (three-party handoff and friends) that
 *     hasn't been built yet.
 *   - Recursion through nested structs/lists is depth-limited rather
 *     than cycle-detected precisely. This is a sound, if conservative,
 *     mitigation against a malicious message trying to force unbounded
 *     or cyclic validation work -- a legitimate deeply-nested message
 *     past the depth limit is also rejected, which is the correct
 *     trade for a kernel-level check that must never run unboundedly.
 */
int capnp_validate_message(const void *msg, size_t len,
                           const struct_schema_t *schema);
