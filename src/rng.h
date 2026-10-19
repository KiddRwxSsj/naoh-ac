#ifndef RNG_H
#define RNG_H

#include <stdbool.h>
#include <stddef.h>

// hardware rng only. there is deliberately no fallback: a caller that gets
// false back has to give up on whatever needed the randomness
bool rng_bytes(void *out, size_t len);

#endif
