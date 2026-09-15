#include "mode.h"

requires_thread_token(mode64_token)
void do_64bit_only_thing(void);

void do_64bit_only_thing(void)
{
}

/* Safe: transitions first, then calls the gated function. */
void correct_caller(void)
{
    enter_long_mode();
}

/* Unsafe: calls the 64-bit-only function without ever transitioning.
 * This is exactly the shape of bug that bit us in boot.S, expressed at
 * the C level where the checker can actually see it. */
void broken_caller(void)
{
    do_64bit_only_thing();
}

requires_thread_token_absent(mode64_token)
void portable_only_thing(void);

void portable_only_thing(void)
{
}

/* Unsafe: calls a portable-only function while mode64_token is held. */
void broken_portable_caller(void)
{
    enter_long_mode();
    portable_only_thing();
}
