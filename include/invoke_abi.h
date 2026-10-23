#pragma once

#include <stddef.h>
#include <stdint.h>

/* The planned single EL0/ring-3 invocation trap ABI. This header defines a
 * wire contract, NOT a security boundary: the existing kernel_invoke()
 * function-pointer handoff still runs privileged code in a shared address
 * space. Do not expose that pointer or its raw capability_t handles to EL0.
 *
 * Enter with a pointer to ipc_call_t in x0 followed by `svc #0` (AArch64),
 * or in RDI followed by `int $0x80` (x86_64). Return ipc_status_t in x0/RAX.
 * There is no caller-id field: the kernel determines the caller from its
 * current address space. The kernel first copies this fixed-size descriptor,
 * checks caller-owned mappings and bounded lengths, copies the request into
 * private kernel memory, resolves `target` in the caller's capability space,
 * then validates and delivers the Cap'n Proto bytes. Only after a validated
 * response may it copy bytes and response_len back to caller-owned memory.
 * A pointer within this descriptor is always a USER VIRTUAL ADDRESS, never
 * a kernel pointer or a function pointer. The wire layout is little-endian
 * on supported x86_64 and aarch64 targets. */
#define IPC_ABI_VERSION 1u
#define IPC_ARM_SVC_IMM 0u
#define IPC_X86_VECTOR 0x80u
#define IPC_MAX_REQUEST_BYTES 4096u
#define IPC_MAX_RESPONSE_BYTES 4096u

typedef uint64_t ipc_handle_t; /* high 32: generation, low 32: caller-local slot.
                                * Slot zero or generation zero is invalid. */

typedef struct ipc_call {
    uint32_t version;
    uint32_t method_id;
    ipc_handle_t target;
    uint64_t request_ptr;
    uint64_t request_len;
    uint64_t response_ptr;
    uint64_t response_capacity;
    uint64_t response_len; /* kernel output; zero on failure */
} ipc_call_t;

typedef enum ipc_status {
    IPC_STATUS_OK = 0,
    IPC_STATUS_BAD_DESCRIPTOR = -1,
    IPC_STATUS_BAD_HANDLE = -2,
    IPC_STATUS_ACCESS = -3,
    IPC_STATUS_BOUNDS = -4,
    IPC_STATUS_BAD_MESSAGE = -5,
    IPC_STATUS_NOT_READY = -6,
} ipc_status_t;

_Static_assert(sizeof(ipc_call_t) == 56, "invoke descriptor must have a stable size");
_Static_assert(offsetof(ipc_call_t, version) == 0, "invoke version offset");
_Static_assert(offsetof(ipc_call_t, method_id) == 4, "invoke method offset");
_Static_assert(offsetof(ipc_call_t, target) == 8, "invoke target offset");
_Static_assert(offsetof(ipc_call_t, request_ptr) == 16, "invoke request offset");
_Static_assert(offsetof(ipc_call_t, request_len) == 24, "invoke request length offset");
_Static_assert(offsetof(ipc_call_t, response_ptr) == 32, "invoke response offset");
_Static_assert(offsetof(ipc_call_t, response_capacity) == 40, "invoke response capacity offset");
_Static_assert(offsetof(ipc_call_t, response_len) == 48, "invoke response length offset");
