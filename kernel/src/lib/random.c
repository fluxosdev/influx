/*
 * Kernel random numbers: a ChaCha20 keystream generator. The key is mixed
 * from RDSEED/RDRAND output (when the CPU has them), TSC jitter sampled
 * around timer reads, and an entropy pool that interrupt handlers feed with
 * their arrival timestamps. Every request ends by replacing the key with
 * fresh keystream (fast key erasure), so earlier output cannot be recovered
 * from the state; the pool is folded in again every few seconds.
 */
#include "random.h"
#include "arch.h"
#include "io.h"
#include "spinlock.h"
#include "string.h"

static uint32_t key[8], nonce[3];
static uint64_t counter;
static spinlock_t lock = SPINLOCK_INIT;
static volatile uint64_t pool[4];
static volatile uint32_t pool_pos;
static uint64_t last_reseed;
static bool have_rdrand, have_rdseed;

#define ROTL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) a += b; d ^= a; d = ROTL(d, 16); c += d; b ^= c; b = ROTL(b, 12); a += b; d ^= a; d = ROTL(d, 8); c += d; b ^= c; b = ROTL(b, 7);

static void chacha_block(uint32_t out[16]) {
    uint32_t s[16] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                       key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                       (uint32_t)counter, (uint32_t)(counter >> 32) ^ nonce[0], nonce[1], nonce[2] };
    uint32_t x[16];
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13]) QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
        QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12]) QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + s[i];
    counter++;
}

static bool rdrand64(uint64_t *v) { unsigned char ok; for (int i = 0; i < 10; i++) { __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok)); if (ok) return true; } return false; }
static bool rdseed64(uint64_t *v) { unsigned char ok; for (int i = 0; i < 10; i++) { __asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok)); if (ok) return true; pause(); } return false; }

void random_add_entropy(uint64_t v) {
    uint32_t i = __atomic_fetch_add(&pool_pos, 1, __ATOMIC_RELAXED) & 3;
    uint64_t x = pool[i] ^ (v * 0x9E3779B97F4A7C15ull) ^ rdtsc();
    pool[i] = (x << 13) | (x >> 51);
}

/* lock held: mix hardware randomness, jitter and the pool into the key */
static void reseed(void) {
    uint64_t m[8] = { 0 };
    for (int i = 0; i < 8; i++) {
        uint64_t v = 0;
        if (have_rdseed && rdseed64(&v)) m[i] ^= v;
        if (have_rdrand && rdrand64(&v)) m[i] ^= v * 0xD6E8FEB86659FD93ull;
        /* timing jitter: tiny loops whose duration wobbles with caches and interrupts */
        for (int j = 0; j < 64; j++) { uint64_t t0 = rdtsc(); for (volatile int k = 0; k < (j & 7) + 1; k++) {} m[i] = (m[i] << 7 | m[i] >> 57) ^ (rdtsc() - t0) ^ t0; }
        m[i] ^= pool[i & 3];
    }
    for (int i = 0; i < 8; i++) key[i] ^= (uint32_t)(m[i] ^ (m[i] >> 32));
    nonce[0] ^= (uint32_t)rdtsc();
    uint32_t blk[16];
    chacha_block(blk);   /* run the new key once so the inputs are not the key */
    memcpy(key, blk, sizeof key);
    memset(blk, 0, sizeof blk);
    last_reseed = time_ms();
}

void random_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    have_rdrand = c & (1u << 30);
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    have_rdseed = b & (1u << 18);
    struct rtc_time t; rtc_read(&t);
    random_add_entropy(rtc_to_unix(&t));
    spin_lock(&lock);
    reseed();
    spin_unlock(&lock);
    kprintf("random: ChaCha20, seeded from %s%stiming jitter\n", have_rdseed ? "RDSEED, " : "", have_rdrand ? "RDRAND, " : "");
}

void random_bytes(void *buf, size_t n) {
    uint8_t *o = buf;
    uint32_t blk[16];
    spin_lock(&lock);
    if (time_ms() - last_reseed > 5000) reseed();
    while (n) {
        chacha_block(blk);
        size_t k = MIN(n, sizeof blk);
        memcpy(o, blk, k);
        o += k; n -= k;
        /* keep the lock bounded for big requests */
        if (n && (counter & 63) == 0) { spin_unlock(&lock); spin_lock(&lock); }
    }
    chacha_block(blk);
    memcpy(key, blk, sizeof key);   /* fast key erasure */
    memset(blk, 0, sizeof blk);
    spin_unlock(&lock);
}

uint64_t random_u64(void) { uint64_t v; random_bytes(&v, sizeof v); return v; }
