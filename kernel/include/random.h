#pragma once
#include "kernel.h"
/* Kernel CSPRNG (ChaCha20), seeded from RDSEED/RDRAND when the CPU has them and from timing
 * jitter always; interrupts keep stirring in more. getrandom(2), /dev/urandom, AT_RANDOM, TLS. */
void random_init(void);
void random_bytes(void *buf, size_t n);
void random_add_entropy(uint64_t v);   /* cheap; called from interrupt handlers */
uint64_t random_u64(void);
