#include "security/random.h"

#include <stddef.h>

namespace random {
    struct state {
        volatile uint32_t lock;
        uint32_t key[8];
        uint64_t counter;
        uint64_t nonce;
        uint64_t generated;
        bool initialized;
        bool have_rdrand;
        bool have_rdseed;
        bool seeded_by_hw;
    };

    static state rng;

    static inline uint64_t irq_save() {
        uint64_t flags;
        asm volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
        return flags;
    }

    static inline void irq_restore(uint64_t flags) {
        asm volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
    }

    static inline void cpu_relax() { asm volatile("pause" ::: "memory"); }

    static void lock(uint64_t *flags) {
        *flags = irq_save();
        while (__atomic_exchange_n(&rng.lock, 1U, __ATOMIC_ACQUIRE)) cpu_relax();
    }

    static void unlock(uint64_t flags) {
        __atomic_store_n(&rng.lock, 0U, __ATOMIC_RELEASE);
        irq_restore(flags);
    }

    static inline uint64_t rdtsc() {
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }

    static inline uint64_t rsp_now() {
        uint64_t sp;
        asm volatile("mov %%rsp, %0" : "=r"(sp));
        return sp;
    }

    static void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b,
                      uint32_t *c, uint32_t *d) {
        uint32_t eax = leaf, ebx, ecx = subleaf, edx;
        asm volatile("pushq %%rbx; cpuid; movl %%ebx, %1; popq %%rbx"
                     : "+a"(eax), "=r"(ebx), "+c"(ecx), "=d"(edx)
                     :
                     : "cc", "memory");
        *a = eax;
        *b = ebx;
        *c = ecx;
        *d = edx;
    }

    static bool rdrand64(uint64_t *out) {
        for (uint32_t i = 0; i < 16; i++) {
            uint64_t v;
            uint8_t ok;
            asm volatile("rdrand %0; setc %1" : "=&r"(v), "=qm"(ok) : : "cc");
            if (ok) {
                *out = v;
                return true;
            }
            cpu_relax();
        }
        return false;
    }

    static bool rdseed64(uint64_t *out) {
        for (uint32_t i = 0; i < 128; i++) {
            uint64_t v;
            uint8_t ok;
            asm volatile("rdseed %0; setc %1" : "=&r"(v), "=qm"(ok) : : "cc");
            if (ok) {
                *out = v;
                return true;
            }
            cpu_relax();
        }
        return false;
    }

    static inline uint32_t rotl32(uint32_t x, uint32_t n) {
        return (x << n) | (x >> (32 - n));
    }

    static uint64_t splitmix64(uint64_t *x) {
        *x += 0x9e3779b97f4a7c15ULL;
        uint64_t z = *x;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    static void quarter(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
        *a += *b; *d ^= *a; *d = rotl32(*d, 16);
        *c += *d; *b ^= *c; *b = rotl32(*b, 12);
        *a += *b; *d ^= *a; *d = rotl32(*d, 8);
        *c += *d; *b ^= *c; *b = rotl32(*b, 7);
    }

    static void store32(uint8_t *p, uint32_t v) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
    }

    static uint32_t load32(const uint8_t *p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
               ((uint32_t)p[3] << 24);
    }

    static void burn(void *ptr, uint64_t len) {
        volatile uint8_t *p = (volatile uint8_t *)ptr;
        while (len--) *p++ = 0;
    }

    static void chacha20_block(uint8_t out[64]) {
        uint32_t x[16];
        uint32_t in[16] = {
            0x61707865U, 0x3320646eU, 0x79622d32U, 0x6b206574U,
            rng.key[0], rng.key[1], rng.key[2], rng.key[3],
            rng.key[4], rng.key[5], rng.key[6], rng.key[7],
            (uint32_t)rng.counter, (uint32_t)(rng.counter >> 32),
            (uint32_t)rng.nonce, (uint32_t)(rng.nonce >> 32),
        };

        for (uint32_t i = 0; i < 16; i++) x[i] = in[i];
        for (uint32_t i = 0; i < 10; i++) {
            quarter(&x[0], &x[4], &x[8],  &x[12]);
            quarter(&x[1], &x[5], &x[9],  &x[13]);
            quarter(&x[2], &x[6], &x[10], &x[14]);
            quarter(&x[3], &x[7], &x[11], &x[15]);
            quarter(&x[0], &x[5], &x[10], &x[15]);
            quarter(&x[1], &x[6], &x[11], &x[12]);
            quarter(&x[2], &x[7], &x[8],  &x[13]);
            quarter(&x[3], &x[4], &x[9],  &x[14]);
        }
        for (uint32_t i = 0; i < 16; i++) store32(out + i * 4, x[i] + in[i]);
        rng.counter++;
        burn(x, sizeof(x));
        burn(in, sizeof(in));
    }

    static void rekey() {
        uint8_t block[64];
        chacha20_block(block);
        for (uint32_t i = 0; i < 8; i++) rng.key[i] = load32(block + i * 4);
        rng.nonce ^= ((uint64_t)load32(block + 32) << 32) | load32(block + 36);
        burn(block, sizeof(block));
    }

    static void absorb64(uint64_t v) {
        uint64_t x = v ^ rdtsc() ^ ((uint64_t)&rng << 7);
        for (uint32_t i = 0; i < 8; i++) {
            uint64_t z = splitmix64(&x);
            rng.key[i] ^= (uint32_t)z;
            rng.key[(i + 5) & 7] ^= (uint32_t)(z >> 32);
        }
        rng.nonce ^= splitmix64(&x);
        rng.counter += (splitmix64(&x) & 0xffff) + 1;
    }

    static void detect_cpu() {
        uint32_t a, b, c, d;
        cpuid(0, 0, &a, &b, &c, &d);
        uint32_t max_basic = a;
        absorb64(((uint64_t)b << 32) ^ d);
        absorb64(((uint64_t)c << 32) ^ max_basic);

        if (max_basic >= 1) {
            cpuid(1, 0, &a, &b, &c, &d);
            rng.have_rdrand = (c & (1U << 30)) != 0;
            absorb64(((uint64_t)a << 32) ^ b ^ ((uint64_t)c << 1) ^ d);
        }
        if (max_basic >= 7) {
            cpuid(7, 0, &a, &b, &c, &d);
            rng.have_rdseed = (b & (1U << 18)) != 0;
            absorb64(((uint64_t)a << 32) ^ b ^ ((uint64_t)c << 1) ^ d);
        }
    }

    static void collect_jitter(uint32_t rounds) {
        uint64_t last = rdtsc();
        uint64_t acc = 0x243f6a8885a308d3ULL ^ rsp_now();
        for (uint32_t i = 0; i < rounds; i++) {
            uint64_t now;
            do {
                cpu_relax();
                now = rdtsc();
            } while (now == last);
            acc ^= (now - last) + (acc << 13) + (acc >> 7) + i;
            last = now;
        }
        absorb64(acc);
    }

    static void collect_hw() {
        uint64_t v;
        if (rng.have_rdseed) {
            for (uint32_t i = 0; i < 8; i++) {
                if (rdseed64(&v)) {
                    absorb64(v);
                    rng.seeded_by_hw = true;
                }
            }
        }
        if (rng.have_rdrand) {
            for (uint32_t i = 0; i < 8; i++) {
                if (rdrand64(&v)) {
                    absorb64(v);
                    rng.seeded_by_hw = true;
                }
            }
        }
    }

    static void init_locked() {
        if (rng.initialized) return;

        uint64_t seed = rdtsc() ^ rsp_now() ^ (uint64_t)&init_locked ^ (uint64_t)&rng;
        for (uint32_t i = 0; i < 8; i++) rng.key[i] = (uint32_t)splitmix64(&seed);
        rng.counter = splitmix64(&seed);
        rng.nonce = splitmix64(&seed);
        rng.generated = 0;
        rng.have_rdrand = false;
        rng.have_rdseed = false;
        rng.seeded_by_hw = false;

        detect_cpu();
        collect_jitter(128);
        collect_hw();
        absorb64(seed);
        rekey();

        rng.initialized = true;
    }

    void init() {
        uint64_t flags;
        lock(&flags);
        init_locked();
        unlock(flags);
    }

    void add_entropy(const void *data, uint64_t len) {
        if (!data || len == 0) return;

        uint64_t flags;
        lock(&flags);
        init_locked();

        const uint8_t *p = (const uint8_t *)data;
        uint64_t acc = len ^ rdtsc() ^ rsp_now();
        for (uint64_t i = 0; i < len; i++) {
            acc ^= (uint64_t)p[i] << ((i & 7) * 8);
            if ((i & 7) == 7) {
                absorb64(acc);
                acc = i ^ 0xa4093822299f31d0ULL;
            }
        }
        absorb64(acc);
        rekey();
        unlock(flags);
    }

    void fill(void *buffer, uint64_t len) {
        if (!buffer || len == 0) return;

        uint64_t flags;
        lock(&flags);
        init_locked();

        if (rng.generated >= (1ULL << 20)) {
            collect_hw();
            collect_jitter(32);
            rekey();
            rng.generated = 0;
        }

        uint8_t *out = (uint8_t *)buffer;
        while (len) {
            uint8_t block[64];
            chacha20_block(block);
            uint64_t n = len < sizeof(block) ? len : sizeof(block);
            for (uint64_t i = 0; i < n; i++) out[i] = block[i];
            burn(block, sizeof(block));
            out += n;
            len -= n;
            rng.generated += n;
        }

        rekey();
        unlock(flags);
    }

    uint64_t next_u64() {
        uint64_t v;
        fill(&v, sizeof(v));
        return v;
    }
}
