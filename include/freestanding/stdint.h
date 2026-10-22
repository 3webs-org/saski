/* Minimal freestanding stdint.h for this kernel's own use.
 *
 * The macOS SDK's stdint.h rejects non-Apple architectures outright
 * ("Unsupported architecture"), and clang's resource directory on this host
 * ships no headers at all. A freestanding kernel needs exactly the fixed-width
 * integer types and their limits, which are fully defined by the C standard
 * and by the target's own ABI -- aarch64: long is 64-bit, int is 32-bit, short
 * is 16-bit, char is 8-bit, all little-endian. Nothing here is invented; it is
 * the standard's own minimum. */

#ifndef _FREESTANDING_STDINT_H
#define _FREESTANDING_STDINT_H

typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long int64_t;

typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;

typedef long intptr_t;
typedef unsigned long uintptr_t;

#define INT8_MIN (-128)
#define INT16_MIN (-32768)
#define INT32_MIN (-2147483647 - 1)
#define INT64_MIN (-9223372036854775807L - 1)

#define UINT8_MAX 255
#define UINT16_MAX 65535
#define UINT32_MAX 4294967295u
#define UINT64_MAX 18446744073709551615ul

#define INT8_MAX 127
#define INT16_MAX 32767
#define INT32_MAX 2147483647
#define INT64_MAX 9223372036854775807L

#endif
