#include "mode.h"

/* A plain (non-_Noreturn) granting function, used purely to isolate
 * testing the requires_thread_token_absent direction from
 * enter_long_mode's real _Noreturn contract, which correctly makes the
 * analyzer treat anything after it as unreachable and would otherwise
 * mask this check entirely. */
grants_thread_token(mode64_token)
void pretend_enter_long_mode(void);

void pretend_enter_long_mode(void)
{
}

requires_thread_token_absent(mode64_token)
void portable_only_thing(void);

void portable_only_thing(void)
{
}

/* Safe: never grants the token, so requiring its absence is satisfied. */
void correct_portable_caller(void)
{
    portable_only_thing();
}

/* Unsafe: grants the token, then calls something requiring its absence. */
void broken_portable_caller(void)
{
    pretend_enter_long_mode();
    portable_only_thing();
}
