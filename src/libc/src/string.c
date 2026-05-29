#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef size_t libc_word_t __attribute__((__may_alias__));

#define WORD_BYTES ((size_t)sizeof(libc_word_t))
#define WORD_MASK (WORD_BYTES - 1)
#define SSE_BYTES ((size_t)16)
#define AVX_BYTES ((size_t)32)
#define SSE_COPY_THRESHOLD ((size_t)128)
#define AVX_COPY_THRESHOLD ((size_t)512)
#define SSE_SCAN_THRESHOLD ((size_t)64)
#define AVX_SCAN_THRESHOLD ((size_t)96)

static inline libc_word_t repeat_byte(unsigned char c) {
    return ((libc_word_t)~(libc_word_t)0 / (libc_word_t)0xff) * (libc_word_t)c;
}

static inline int has_zero_byte(libc_word_t x) {
    const libc_word_t ones = (libc_word_t)~(libc_word_t)0 / (libc_word_t)0xff;
    const libc_word_t highs = ones << 7;
    return ((x - ones) & ~x & highs) != 0;
}

static inline int same_alignment(const void *a, const void *b) {
    return ((((uintptr_t)a ^ (uintptr_t)b) & WORD_MASK) == 0);
}

#if defined(__x86_64__) && defined(__GNUC__)
#define LIBC_X86_SIMD 1
#define CPU_SSE2 1
#define CPU_AVX 2
#define CPU_AVX2 4

typedef char libc_v16qi __attribute__((__vector_size__(16)));
typedef char libc_v32qi __attribute__((__vector_size__(32)));
typedef unsigned char libc_sse_u8x16 __attribute__((__vector_size__(16), __aligned__(1), __may_alias__));
typedef unsigned char libc_avx_u8x32 __attribute__((__vector_size__(32), __aligned__(1), __may_alias__));
typedef uint64_t libc_sse_u64x2 __attribute__((__vector_size__(16), __aligned__(1), __may_alias__));
typedef uint64_t libc_avx_u64x4 __attribute__((__vector_size__(32), __aligned__(1), __may_alias__));

static inline void cpuid_leaf(uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(subleaf));
    *a = eax;
    *b = ebx;
    *c = ecx;
    *d = edx;
}

static inline uint64_t xgetbv0(void) {
    uint32_t eax;
    uint32_t edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((uint64_t)edx << 32) | eax;
}

static int cpu_features(void) {
    static int cached;
    int state = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);
    if (state) return state > 0 ? state : 0;

    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    int features = 0;

    cpuid_leaf(0, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;
    if (max_leaf >= 1) {
        cpuid_leaf(1, 0, &eax, &ebx, &ecx, &edx);
        if (edx & (1u << 26)) features |= CPU_SSE2;

        const uint32_t avx_bits = (1u << 26) | (1u << 27) | (1u << 28);
        if ((ecx & avx_bits) == avx_bits && (xgetbv0() & 6) == 6) {
            features |= CPU_AVX;
            if (max_leaf >= 7) {
                cpuid_leaf(7, 0, &eax, &ebx, &ecx, &edx);
                if (ebx & (1u << 5)) features |= CPU_AVX2;
            }
        }
    }

    __atomic_store_n(&cached, features ? features : -1, __ATOMIC_RELEASE);
    return features;
}

static inline int simd_has(int feature) {
    return (cpu_features() & feature) != 0;
}

__attribute__((__target__("sse2"), __noinline__))
static void *memcpy_forward_sse2(void *restrict dst, const void *restrict src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n >= SSE_BYTES * 8) {
        ((libc_sse_u8x16 *)d)[0] = ((const libc_sse_u8x16 *)s)[0];
        ((libc_sse_u8x16 *)d)[1] = ((const libc_sse_u8x16 *)s)[1];
        ((libc_sse_u8x16 *)d)[2] = ((const libc_sse_u8x16 *)s)[2];
        ((libc_sse_u8x16 *)d)[3] = ((const libc_sse_u8x16 *)s)[3];
        ((libc_sse_u8x16 *)d)[4] = ((const libc_sse_u8x16 *)s)[4];
        ((libc_sse_u8x16 *)d)[5] = ((const libc_sse_u8x16 *)s)[5];
        ((libc_sse_u8x16 *)d)[6] = ((const libc_sse_u8x16 *)s)[6];
        ((libc_sse_u8x16 *)d)[7] = ((const libc_sse_u8x16 *)s)[7];
        d += SSE_BYTES * 8;
        s += SSE_BYTES * 8;
        n -= SSE_BYTES * 8;
    }
    while (n >= SSE_BYTES) {
        *(libc_sse_u8x16 *)d = *(const libc_sse_u8x16 *)s;
        d += SSE_BYTES;
        s += SSE_BYTES;
        n -= SSE_BYTES;
    }
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((__target__("sse2"), __noinline__))
static void memmove_backward_sse2(unsigned char *d, const unsigned char *s, size_t n) {
    d += n;
    s += n;
    while (n >= SSE_BYTES * 8) {
        d -= SSE_BYTES * 8;
        s -= SSE_BYTES * 8;
        ((libc_sse_u8x16 *)d)[7] = ((const libc_sse_u8x16 *)s)[7];
        ((libc_sse_u8x16 *)d)[6] = ((const libc_sse_u8x16 *)s)[6];
        ((libc_sse_u8x16 *)d)[5] = ((const libc_sse_u8x16 *)s)[5];
        ((libc_sse_u8x16 *)d)[4] = ((const libc_sse_u8x16 *)s)[4];
        ((libc_sse_u8x16 *)d)[3] = ((const libc_sse_u8x16 *)s)[3];
        ((libc_sse_u8x16 *)d)[2] = ((const libc_sse_u8x16 *)s)[2];
        ((libc_sse_u8x16 *)d)[1] = ((const libc_sse_u8x16 *)s)[1];
        ((libc_sse_u8x16 *)d)[0] = ((const libc_sse_u8x16 *)s)[0];
        n -= SSE_BYTES * 8;
    }
    while (n >= SSE_BYTES) {
        d -= SSE_BYTES;
        s -= SSE_BYTES;
        *(libc_sse_u8x16 *)d = *(const libc_sse_u8x16 *)s;
        n -= SSE_BYTES;
    }
    while (n--) *--d = *--s;
}

__attribute__((__target__("sse2"), __noinline__))
static void *memset_sse2(void *dst, int value, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char c = (unsigned char)value;
    uint64_t w = ((uint64_t)~(uint64_t)0 / (uint64_t)0xff) * (uint64_t)c;
    libc_sse_u64x2 v = {w, w};

    while (n >= SSE_BYTES * 8) {
        ((libc_sse_u64x2 *)d)[0] = v;
        ((libc_sse_u64x2 *)d)[1] = v;
        ((libc_sse_u64x2 *)d)[2] = v;
        ((libc_sse_u64x2 *)d)[3] = v;
        ((libc_sse_u64x2 *)d)[4] = v;
        ((libc_sse_u64x2 *)d)[5] = v;
        ((libc_sse_u64x2 *)d)[6] = v;
        ((libc_sse_u64x2 *)d)[7] = v;
        d += SSE_BYTES * 8;
        n -= SSE_BYTES * 8;
    }
    while (n >= SSE_BYTES) {
        *(libc_sse_u64x2 *)d = v;
        d += SSE_BYTES;
        n -= SSE_BYTES;
    }
    while (n--) *d++ = c;
    return dst;
}

__attribute__((__target__("avx"), __noinline__))
static void *memcpy_forward_avx(void *restrict dst, const void *restrict src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while (n >= AVX_BYTES * 8) {
        ((libc_avx_u8x32 *)d)[0] = ((const libc_avx_u8x32 *)s)[0];
        ((libc_avx_u8x32 *)d)[1] = ((const libc_avx_u8x32 *)s)[1];
        ((libc_avx_u8x32 *)d)[2] = ((const libc_avx_u8x32 *)s)[2];
        ((libc_avx_u8x32 *)d)[3] = ((const libc_avx_u8x32 *)s)[3];
        ((libc_avx_u8x32 *)d)[4] = ((const libc_avx_u8x32 *)s)[4];
        ((libc_avx_u8x32 *)d)[5] = ((const libc_avx_u8x32 *)s)[5];
        ((libc_avx_u8x32 *)d)[6] = ((const libc_avx_u8x32 *)s)[6];
        ((libc_avx_u8x32 *)d)[7] = ((const libc_avx_u8x32 *)s)[7];
        d += AVX_BYTES * 8;
        s += AVX_BYTES * 8;
        n -= AVX_BYTES * 8;
    }
    while (n >= AVX_BYTES) {
        *(libc_avx_u8x32 *)d = *(const libc_avx_u8x32 *)s;
        d += AVX_BYTES;
        s += AVX_BYTES;
        n -= AVX_BYTES;
    }
    __builtin_ia32_vzeroupper();
    while (n--) *d++ = *s++;
    return dst;
}

__attribute__((__target__("avx"), __noinline__))
static void memmove_backward_avx(unsigned char *d, const unsigned char *s, size_t n) {
    d += n;
    s += n;
    while (n >= AVX_BYTES * 8) {
        d -= AVX_BYTES * 8;
        s -= AVX_BYTES * 8;
        ((libc_avx_u8x32 *)d)[7] = ((const libc_avx_u8x32 *)s)[7];
        ((libc_avx_u8x32 *)d)[6] = ((const libc_avx_u8x32 *)s)[6];
        ((libc_avx_u8x32 *)d)[5] = ((const libc_avx_u8x32 *)s)[5];
        ((libc_avx_u8x32 *)d)[4] = ((const libc_avx_u8x32 *)s)[4];
        ((libc_avx_u8x32 *)d)[3] = ((const libc_avx_u8x32 *)s)[3];
        ((libc_avx_u8x32 *)d)[2] = ((const libc_avx_u8x32 *)s)[2];
        ((libc_avx_u8x32 *)d)[1] = ((const libc_avx_u8x32 *)s)[1];
        ((libc_avx_u8x32 *)d)[0] = ((const libc_avx_u8x32 *)s)[0];
        n -= AVX_BYTES * 8;
    }
    while (n >= AVX_BYTES) {
        d -= AVX_BYTES;
        s -= AVX_BYTES;
        *(libc_avx_u8x32 *)d = *(const libc_avx_u8x32 *)s;
        n -= AVX_BYTES;
    }
    __builtin_ia32_vzeroupper();
    while (n--) *--d = *--s;
}

__attribute__((__target__("avx"), __noinline__))
static void *memset_avx(void *dst, int value, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char c = (unsigned char)value;
    uint64_t w = ((uint64_t)~(uint64_t)0 / (uint64_t)0xff) * (uint64_t)c;
    libc_avx_u64x4 v = {w, w, w, w};

    while (n >= AVX_BYTES * 8) {
        ((libc_avx_u64x4 *)d)[0] = v;
        ((libc_avx_u64x4 *)d)[1] = v;
        ((libc_avx_u64x4 *)d)[2] = v;
        ((libc_avx_u64x4 *)d)[3] = v;
        ((libc_avx_u64x4 *)d)[4] = v;
        ((libc_avx_u64x4 *)d)[5] = v;
        ((libc_avx_u64x4 *)d)[6] = v;
        ((libc_avx_u64x4 *)d)[7] = v;
        d += AVX_BYTES * 8;
        n -= AVX_BYTES * 8;
    }
    while (n >= AVX_BYTES) {
        *(libc_avx_u64x4 *)d = v;
        d += AVX_BYTES;
        n -= AVX_BYTES;
    }
    __builtin_ia32_vzeroupper();
    while (n--) *d++ = c;
    return dst;
}

__attribute__((__target__("sse2"), __noinline__))
static void *memchr_sse2(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;
    libc_sse_u8x16 v = {ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch};

    while (n >= SSE_BYTES * 4) {
        int m0 = __builtin_ia32_pmovmskb128((libc_v16qi)(((const libc_sse_u8x16 *)p)[0] == v));
        int m1 = __builtin_ia32_pmovmskb128((libc_v16qi)(((const libc_sse_u8x16 *)p)[1] == v));
        int m2 = __builtin_ia32_pmovmskb128((libc_v16qi)(((const libc_sse_u8x16 *)p)[2] == v));
        int m3 = __builtin_ia32_pmovmskb128((libc_v16qi)(((const libc_sse_u8x16 *)p)[3] == v));
        if (m0) return (void *)(p + __builtin_ctz((unsigned)m0));
        if (m1) return (void *)(p + SSE_BYTES + __builtin_ctz((unsigned)m1));
        if (m2) return (void *)(p + SSE_BYTES * 2 + __builtin_ctz((unsigned)m2));
        if (m3) return (void *)(p + SSE_BYTES * 3 + __builtin_ctz((unsigned)m3));
        p += SSE_BYTES * 4;
        n -= SSE_BYTES * 4;
    }
    while (n >= SSE_BYTES) {
        int m = __builtin_ia32_pmovmskb128((libc_v16qi)(*(const libc_sse_u8x16 *)p == v));
        if (m) return (void *)(p + __builtin_ctz((unsigned)m));
        p += SSE_BYTES;
        n -= SSE_BYTES;
    }
    while (n--) {
        if (*p == ch) return (void *)p;
        p++;
    }
    return 0;
}

__attribute__((__target__("avx2"), __noinline__))
static void *memchr_avx2(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;
    libc_avx_u8x32 v = {ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch,
                        ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch};

    while (n >= AVX_BYTES * 4) {
        int m0 = __builtin_ia32_pmovmskb256((libc_v32qi)(((const libc_avx_u8x32 *)p)[0] == v));
        int m1 = __builtin_ia32_pmovmskb256((libc_v32qi)(((const libc_avx_u8x32 *)p)[1] == v));
        int m2 = __builtin_ia32_pmovmskb256((libc_v32qi)(((const libc_avx_u8x32 *)p)[2] == v));
        int m3 = __builtin_ia32_pmovmskb256((libc_v32qi)(((const libc_avx_u8x32 *)p)[3] == v));
        if (m0) { __builtin_ia32_vzeroupper(); return (void *)(p + __builtin_ctz((unsigned)m0)); }
        if (m1) { __builtin_ia32_vzeroupper(); return (void *)(p + AVX_BYTES + __builtin_ctz((unsigned)m1)); }
        if (m2) { __builtin_ia32_vzeroupper(); return (void *)(p + AVX_BYTES * 2 + __builtin_ctz((unsigned)m2)); }
        if (m3) { __builtin_ia32_vzeroupper(); return (void *)(p + AVX_BYTES * 3 + __builtin_ctz((unsigned)m3)); }
        p += AVX_BYTES * 4;
        n -= AVX_BYTES * 4;
    }
    while (n >= AVX_BYTES) {
        int m = __builtin_ia32_pmovmskb256((libc_v32qi)(*(const libc_avx_u8x32 *)p == v));
        if (m) { __builtin_ia32_vzeroupper(); return (void *)(p + __builtin_ctz((unsigned)m)); }
        p += AVX_BYTES;
        n -= AVX_BYTES;
    }
    __builtin_ia32_vzeroupper();
    return memchr_sse2(p, c, n);
}

__attribute__((__target__("sse2"), __noinline__))
static size_t strnlen_sse2(const char *s, size_t maxlen) {
    const unsigned char *p = (const unsigned char *)s;
    size_t n = maxlen;
    libc_sse_u8x16 z = {0};

    while (n && ((uintptr_t)p & (SSE_BYTES - 1))) {
        if (*p == 0) return (size_t)((const char *)p - s);
        p++;
        n--;
    }
    while (n >= SSE_BYTES) {
        int m = __builtin_ia32_pmovmskb128((libc_v16qi)(*(const libc_sse_u8x16 *)p == z));
        if (m) return (size_t)((const char *)p - s) + (size_t)__builtin_ctz((unsigned)m);
        p += SSE_BYTES;
        n -= SSE_BYTES;
    }
    while (n && *p) {
        p++;
        n--;
    }
    return (size_t)((const char *)p - s);
}

__attribute__((__target__("avx2"), __noinline__))
static size_t strnlen_avx2(const char *s, size_t maxlen) {
    const unsigned char *p = (const unsigned char *)s;
    size_t n = maxlen;
    libc_avx_u8x32 z = {0};

    while (n && ((uintptr_t)p & (AVX_BYTES - 1))) {
        if (*p == 0) {
            __builtin_ia32_vzeroupper();
            return (size_t)((const char *)p - s);
        }
        p++;
        n--;
    }
    while (n >= AVX_BYTES) {
        int m = __builtin_ia32_pmovmskb256((libc_v32qi)(*(const libc_avx_u8x32 *)p == z));
        if (m) {
            __builtin_ia32_vzeroupper();
            return (size_t)((const char *)p - s) + (size_t)__builtin_ctz((unsigned)m);
        }
        p += AVX_BYTES;
        n -= AVX_BYTES;
    }
    __builtin_ia32_vzeroupper();
    return (size_t)((const char *)p - s) + strnlen_sse2((const char *)p, n);
}

__attribute__((__target__("sse2"), __noinline__))
static char *strchr_sse2(const char *s, int c) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;
    libc_sse_u8x16 v = {ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch};
    libc_sse_u8x16 z = {0};

    while ((uintptr_t)p & (SSE_BYTES - 1)) {
        if (*p == ch) return (char *)p;
        if (*p == 0) return 0;
        p++;
    }

    for (;;) {
        libc_sse_u8x16 x = *(const libc_sse_u8x16 *)p;
        int hit = __builtin_ia32_pmovmskb128((libc_v16qi)(x == v));
        int end = __builtin_ia32_pmovmskb128((libc_v16qi)(x == z));
        int mask = hit | end;
        if (mask) {
            unsigned off = (unsigned)__builtin_ctz((unsigned)mask);
            if (hit & (1u << off)) return (char *)(p + off);
            return 0;
        }
        p += SSE_BYTES;
    }
}

__attribute__((__target__("avx2"), __noinline__))
static char *strchr_avx2(const char *s, int c) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;
    libc_avx_u8x32 v = {ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch,
                        ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch, ch};
    libc_avx_u8x32 z = {0};

    while ((uintptr_t)p & (AVX_BYTES - 1)) {
        if (*p == ch) { __builtin_ia32_vzeroupper(); return (char *)p; }
        if (*p == 0) { __builtin_ia32_vzeroupper(); return 0; }
        p++;
    }

    for (;;) {
        libc_avx_u8x32 x = *(const libc_avx_u8x32 *)p;
        int hit = __builtin_ia32_pmovmskb256((libc_v32qi)(x == v));
        int end = __builtin_ia32_pmovmskb256((libc_v32qi)(x == z));
        int mask = hit | end;
        if (mask) {
            unsigned off = (unsigned)__builtin_ctz((unsigned)mask);
            __builtin_ia32_vzeroupper();
            if (hit & (1u << off)) return (char *)(p + off);
            return 0;
        }
        p += AVX_BYTES;
    }
}
#else
#define LIBC_X86_SIMD 0
static inline int simd_has(int feature) {
    (void)feature;
    return 0;
}
#endif

static void *memcpy_forward(void *restrict dst, const void *restrict src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

#if LIBC_X86_SIMD
    if (n >= AVX_COPY_THRESHOLD && simd_has(CPU_AVX)) return memcpy_forward_avx(dst, src, n);
    if (n >= SSE_COPY_THRESHOLD && simd_has(CPU_SSE2)) return memcpy_forward_sse2(dst, src, n);
#endif

    if (n >= WORD_BYTES && same_alignment(d, s)) {
        while (n && ((uintptr_t)d & WORD_MASK)) {
            *d++ = *s++;
            n--;
        }
        while (n >= WORD_BYTES * 4) {
            ((libc_word_t *)d)[0] = ((const libc_word_t *)s)[0];
            ((libc_word_t *)d)[1] = ((const libc_word_t *)s)[1];
            ((libc_word_t *)d)[2] = ((const libc_word_t *)s)[2];
            ((libc_word_t *)d)[3] = ((const libc_word_t *)s)[3];
            d += WORD_BYTES * 4;
            s += WORD_BYTES * 4;
            n -= WORD_BYTES * 4;
        }
        while (n >= WORD_BYTES) {
            *(libc_word_t *)d = *(const libc_word_t *)s;
            d += WORD_BYTES;
            s += WORD_BYTES;
            n -= WORD_BYTES;
        }
    }

    while (n--) *d++ = *s++;
    return dst;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n) {
    return memcpy_forward(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (d == s || n == 0) return dst;

    uintptr_t da = (uintptr_t)d;
    uintptr_t sa = (uintptr_t)s;
    if (da - sa >= n) return memcpy_forward(dst, src, n);

#if LIBC_X86_SIMD
    if (n >= AVX_COPY_THRESHOLD && simd_has(CPU_AVX)) {
        memmove_backward_avx(d, s, n);
        return dst;
    }
    if (n >= SSE_COPY_THRESHOLD && simd_has(CPU_SSE2)) {
        memmove_backward_sse2(d, s, n);
        return dst;
    }
#endif

    d += n;
    s += n;
    if (n >= WORD_BYTES && same_alignment(d, s)) {
        while (n && ((uintptr_t)d & WORD_MASK)) {
            *--d = *--s;
            n--;
        }
        while (n >= WORD_BYTES * 4) {
            d -= WORD_BYTES * 4;
            s -= WORD_BYTES * 4;
            ((libc_word_t *)d)[3] = ((const libc_word_t *)s)[3];
            ((libc_word_t *)d)[2] = ((const libc_word_t *)s)[2];
            ((libc_word_t *)d)[1] = ((const libc_word_t *)s)[1];
            ((libc_word_t *)d)[0] = ((const libc_word_t *)s)[0];
            n -= WORD_BYTES * 4;
        }
        while (n >= WORD_BYTES) {
            d -= WORD_BYTES;
            s -= WORD_BYTES;
            *(libc_word_t *)d = *(const libc_word_t *)s;
            n -= WORD_BYTES;
        }
    }

    while (n--) *--d = *--s;
    return dst;
}

void *memset(void *dst, int value, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char c = (unsigned char)value;

#if LIBC_X86_SIMD
    if (n >= AVX_COPY_THRESHOLD && simd_has(CPU_AVX)) return memset_avx(dst, value, n);
    if (n >= SSE_COPY_THRESHOLD && simd_has(CPU_SSE2)) return memset_sse2(dst, value, n);
#endif

    if (n >= WORD_BYTES) {
        libc_word_t w = repeat_byte(c);
        while (n && ((uintptr_t)d & WORD_MASK)) {
            *d++ = c;
            n--;
        }
        while (n >= WORD_BYTES * 4) {
            ((libc_word_t *)d)[0] = w;
            ((libc_word_t *)d)[1] = w;
            ((libc_word_t *)d)[2] = w;
            ((libc_word_t *)d)[3] = w;
            d += WORD_BYTES * 4;
            n -= WORD_BYTES * 4;
        }
        while (n >= WORD_BYTES) {
            *(libc_word_t *)d = w;
            d += WORD_BYTES;
            n -= WORD_BYTES;
        }
    }

    while (n--) *d++ = c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;

    if (n >= WORD_BYTES && same_alignment(x, y)) {
        while (n && ((uintptr_t)x & WORD_MASK)) {
            if (*x != *y) return (int)*x - (int)*y;
            x++;
            y++;
            n--;
        }
        while (n >= WORD_BYTES) {
            libc_word_t wx = *(const libc_word_t *)x;
            libc_word_t wy = *(const libc_word_t *)y;
            if (wx != wy) break;
            x += WORD_BYTES;
            y += WORD_BYTES;
            n -= WORD_BYTES;
        }
    }

    while (n--) {
        if (*x != *y) return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
#if LIBC_X86_SIMD
    if (n >= AVX_SCAN_THRESHOLD && simd_has(CPU_AVX2)) return memchr_avx2(s, c, n);
    if (n >= SSE_SCAN_THRESHOLD && simd_has(CPU_SSE2)) return memchr_sse2(s, c, n);
#endif

    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;

    while (n && ((uintptr_t)p & WORD_MASK)) {
        if (*p == ch) return (void *)p;
        p++;
        n--;
    }

    if (n >= WORD_BYTES) {
        libc_word_t mask = repeat_byte(ch);
        while (n >= WORD_BYTES) {
            libc_word_t w = *(const libc_word_t *)p;
            if (has_zero_byte(w ^ mask)) break;
            p += WORD_BYTES;
            n -= WORD_BYTES;
        }
    }

    while (n--) {
        if (*p == ch) return (void *)p;
        p++;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;

#if LIBC_X86_SIMD
    if (simd_has(CPU_AVX2)) return strnlen_avx2(s, (size_t)-1);
    if (simd_has(CPU_SSE2)) return strnlen_sse2(s, (size_t)-1);
#endif

    while ((uintptr_t)p & WORD_MASK) {
        if (!*p) return (size_t)(p - s);
        p++;
    }
    for (;;) {
        libc_word_t w = *(const libc_word_t *)p;
        if (has_zero_byte(w)) break;
        p += WORD_BYTES;
    }
    while (*p) p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t maxlen) {
#if LIBC_X86_SIMD
    if (maxlen >= AVX_SCAN_THRESHOLD && simd_has(CPU_AVX2)) return strnlen_avx2(s, maxlen);
    if (maxlen >= SSE_SCAN_THRESHOLD && simd_has(CPU_SSE2)) return strnlen_sse2(s, maxlen);
#endif

    const char *p = s;
    size_t n = maxlen;

    while (n && ((uintptr_t)p & WORD_MASK)) {
        if (!*p) return (size_t)(p - s);
        p++;
        n--;
    }

    while (n >= WORD_BYTES) {
        libc_word_t w = *(const libc_word_t *)p;
        if (has_zero_byte(w)) break;
        p += WORD_BYTES;
        n -= WORD_BYTES;
    }

    while (n && *p) {
        p++;
        n--;
    }
    return (size_t)(p - s);
}

char *strcpy(char *restrict dst, const char *restrict src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *restrict dst, const char *restrict src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    if (i < n) memset(dst + i, 0, n - i);
    return dst;
}

char *strcat(char *restrict dst, const char *restrict src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb || ca == 0) return (int)ca - (int)cb;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    unsigned char ch = (unsigned char)c;
    const unsigned char *p = (const unsigned char *)s;

    if (ch == 0) return (char *)s + strlen(s);

#if LIBC_X86_SIMD
    if (simd_has(CPU_AVX2)) return strchr_avx2(s, c);
    if (simd_has(CPU_SSE2)) return strchr_sse2(s, c);
#endif

    while ((uintptr_t)p & WORD_MASK) {
        if (*p == ch) return (char *)p;
        if (*p == 0) return 0;
        p++;
    }

    libc_word_t mask = repeat_byte(ch);
    for (;;) {
        libc_word_t w = *(const libc_word_t *)p;
        if (has_zero_byte(w) || has_zero_byte(w ^ mask)) break;
        p += WORD_BYTES;
    }
    while (*p) {
        if (*p == ch) return (char *)p;
        p++;
    }
    return 0;
}

char *strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *last = 0;
    do {
        if (*s == ch) last = s;
    } while (*s++);
    return (char *)last;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (!copy) return 0;
    memcpy(copy, s, len);
    return copy;
}

char *strncat(char *restrict dst, const char *restrict src, size_t n) {
    char *d = dst + strlen(dst);
    size_t i = 0;
    for (; i < n && src[i]; i++) d[i] = src[i];
    d[i] = 0;
    return dst;
}

static void byte_set(const char *s, unsigned char set[32]) {
    memset(set, 0, 32);
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        set[c >> 3] |= (unsigned char)(1u << (c & 7));
    }
}

static int byte_set_has(const unsigned char set[32], unsigned char c) {
    return (set[c >> 3] & (unsigned char)(1u << (c & 7))) != 0;
}

size_t strspn(const char *s, const char *accept) {
    unsigned char set[32];
    size_t n = 0;
    byte_set(accept, set);
    while (s[n] && byte_set_has(set, (unsigned char)s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    unsigned char set[32];
    size_t n = 0;
    byte_set(reject, set);
    while (s[n] && !byte_set_has(set, (unsigned char)s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    unsigned char set[32];
    byte_set(accept, set);
    while (*s) {
        if (byte_set_has(set, (unsigned char)*s)) return (char *)s;
        s++;
    }
    return 0;
}

char *strstr(const char *haystack, const char *needle) {
    unsigned char first = (unsigned char)needle[0];
    if (!first) return (char *)haystack;

    size_t nlen = strlen(needle);
    if (nlen == 1) return strchr(haystack, first);

    size_t hlen = strlen(haystack);
    while (hlen >= nlen) {
        const char *p = (const char *)memchr(haystack, first, hlen - nlen + 1);
        if (!p) return 0;
        hlen -= (size_t)(p - haystack);
        haystack = p;
        if (memcmp(haystack + 1, needle + 1, nlen - 1) == 0) return (char *)haystack;
        haystack++;
        hlen--;
    }
    return 0;
}

char *strtok_r(char *restrict str, const char *restrict delim, char **restrict saveptr) {
    char *s = str ? str : *saveptr;
    if (!s) return 0;
    s += strspn(s, delim);
    if (!*s) {
        *saveptr = 0;
        return 0;
    }
    char *end = s + strcspn(s, delim);
    if (*end) {
        *end++ = 0;
        *saveptr = end;
    } else {
        *saveptr = 0;
    }
    return s;
}

char *strtok(char *restrict str, const char *restrict delim) {
    static char *save;
    return strtok_r(str, delim, &save);
}
