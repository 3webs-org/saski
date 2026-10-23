/* SPDX-License-Identifier: 0BSD */
/* Host and cross-compile check for the planned single invocation trap ABI.
 * No trap is installed by this test; it proves the descriptor's wire shape. */
#include "invoke_abi.h"

_Static_assert(IPC_ABI_VERSION == 1, "unexpected invocation ABI version");
_Static_assert(IPC_ARM_SVC_IMM == 0 && IPC_X86_VECTOR == 0x80,
               "trap entry numbers changed");
_Static_assert(IPC_MAX_REQUEST_BYTES == 4096, "request bound changed");
_Static_assert(IPC_MAX_RESPONSE_BYTES == 4096, "response bound changed");

int main(void)
{
    ipc_call_t call = {
        .version = IPC_ABI_VERSION,
        .method_id = 2,
        .target = 0x0102030405060708ull,
        .request_ptr = 0x1122334455667788ull,
        .request_len = 0x123,
        .response_ptr = 0x8877665544332211ull,
        .response_capacity = 0x234,
        .response_len = 0,
    };
    static const unsigned char expected[56] = {
        1, 0, 0, 0, 2, 0, 0, 0,
        8, 7, 6, 5, 4, 3, 2, 1,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x23, 1, 0, 0, 0, 0, 0, 0,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x34, 2, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    const unsigned char *bytes = (const unsigned char *)&call;
    for (unsigned i = 0; i < sizeof(expected); i++) {
        if (bytes[i] != expected[i]) {
            return 1;
        }
    }
    if (IPC_STATUS_OK != 0 || IPC_STATUS_BAD_DESCRIPTOR >= 0 ||
        IPC_STATUS_BAD_HANDLE >= 0 || IPC_STATUS_ACCESS >= 0 ||
        IPC_STATUS_BOUNDS >= 0 || IPC_STATUS_BAD_MESSAGE >= 0 ||
        IPC_STATUS_NOT_READY >= 0) {
        return 2;
    }
    call.response_len = 0x0102030405060708ull;
    return bytes[48] == 8 && bytes[55] == 1 ? 0 : 3;
}
