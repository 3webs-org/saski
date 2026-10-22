/* Minimal freestanding stddef.h: size_t, ptrdiff_t, NULL, offsetof. */

#ifndef _FREESTANDING_STDDEF_H
#define _FREESTANDING_STDDEF_H

typedef unsigned long size_t;
typedef long ptrdiff_t;

#define NULL ((void *)0)
#define offsetof(type, member) __builtin_offsetof(type, member)

#endif
