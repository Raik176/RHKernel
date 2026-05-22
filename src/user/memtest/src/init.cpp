#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

extern "C" void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
extern "C" int munmap(void *addr, size_t length);
extern "C" int brk(void *addr);
extern "C" void *sbrk(ptrdiff_t increment);

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)

static int g_failures = 0;
static int g_checks = 0;

static void pass(const char *name) {
    g_checks++;
    printf("[PASS] %s\n", name);
    fflush(stdout);
}

static void fail(const char *name, const char *why) {
    g_checks++;
    g_failures++;
    printf("[FAIL] %s: %s\n", name, why);
    fflush(stdout);
}

static uint8_t pattern_a(size_t i) {
    return (uint8_t)((i * 131u + 17u) ^ (i >> 3));
}

static uint8_t pattern_b(size_t i) {
    return (uint8_t)(0xA5u ^ (i * 29u) ^ (i >> 7));
}

static int verify_pattern(uint8_t *p, size_t n, uint8_t (*pattern)(size_t)) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] != pattern(i)) {
            printf("        mismatch at offset %lu: got 0x%02x expected 0x%02x\n",
                   (unsigned long)i, (unsigned)p[i], (unsigned)pattern(i));
            return 0;
        }
    }
    return 1;
}

static void fill_pattern(uint8_t *p, size_t n, uint8_t (*pattern)(size_t)) {
    for (size_t i = 0; i < n; i++) p[i] = pattern(i);
}

static void test_sbrk_basic() {
    const char *name = "sbrk grow/write/read/shrink";
    void *start = sbrk(0);
    if (start == MAP_FAILED) {
        fail(name, "sbrk(0) failed");
        return;
    }

    const size_t len = 5 * 4096 + 123;
    uint8_t *p = (uint8_t *)sbrk((ptrdiff_t)len);
    if (p == MAP_FAILED) {
        fail(name, "sbrk positive increment failed");
        return;
    }
    if (p != start) {
        fail(name, "sbrk did not return old break");
        return;
    }

    fill_pattern(p, len, pattern_a);
    if (!verify_pattern(p, len, pattern_a)) {
        fail(name, "data changed after sbrk write");
        return;
    }

    void *after = sbrk(0);
    if ((uintptr_t)after < (uintptr_t)start + len) {
        fail(name, "program break did not advance enough");
        return;
    }

    if (sbrk(-4096) == MAP_FAILED) {
        fail(name, "sbrk negative increment failed");
        return;
    }

    pass(name);
}

static void test_brk_exact() {
    const char *name = "brk exact set and query";
    void *old_break = sbrk(0);
    if (old_break == MAP_FAILED) {
        fail(name, "sbrk(0) failed");
        return;
    }

    uintptr_t target = ((uintptr_t)old_break + 8192 + 4095) & ~((uintptr_t)4095);
    if (brk((void *)target) != 0) {
        fail(name, "brk(target) failed");
        return;
    }
    if (sbrk(0) != (void *)target) {
        fail(name, "sbrk(0) did not report target break");
        return;
    }

    uint8_t *last = (uint8_t *)(target - 1);
    *last = 0x5A;
    if (*last != 0x5A) {
        fail(name, "last byte before break was not writable");
        return;
    }

    pass(name);
}

static void test_malloc_various_sizes() {
    const char *name = "malloc/free varied sizes";
    const size_t sizes[] = {1, 2, 3, 7, 16, 31, 64, 127, 256, 1024, 4096, 8193, 65536};
    const size_t count = sizeof(sizes) / sizeof(sizes[0]);
    uint8_t *ptrs[count];

    for (size_t i = 0; i < count; i++) {
        ptrs[i] = (uint8_t *)malloc(sizes[i]);
        if (!ptrs[i]) {
            fail(name, "malloc returned NULL");
            for (size_t j = 0; j < i; j++) free(ptrs[j]);
            return;
        }
        fill_pattern(ptrs[i], sizes[i], pattern_a);
    }

    for (size_t i = 0; i < count; i++) {
        if (!verify_pattern(ptrs[i], sizes[i], pattern_a)) {
            fail(name, "allocation contents corrupted");
            for (size_t j = 0; j < count; j++) free(ptrs[j]);
            return;
        }
    }

    for (size_t i = 0; i < count; i++) free(ptrs[i]);
    pass(name);
}

static void test_calloc_zero() {
    const char *name = "calloc zero fill";
    const size_t n = 32768;
    uint8_t *p = (uint8_t *)calloc(n, 1);
    if (!p) {
        fail(name, "calloc returned NULL");
        return;
    }

    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0) {
            free(p);
            fail(name, "calloc memory was not zeroed");
            return;
        }
    }

    memset(p, 0xCC, n);
    free(p);
    pass(name);
}

static void test_realloc_grow_shrink() {
    const char *name = "realloc grow/shrink preserves data";
    const size_t small = 1024;
    const size_t large = 64 * 1024;
    uint8_t *p = (uint8_t *)malloc(small);
    if (!p) {
        fail(name, "initial malloc failed");
        return;
    }

    fill_pattern(p, small, pattern_a);
    uint8_t *q = (uint8_t *)realloc(p, large);
    if (!q) {
        free(p);
        fail(name, "realloc grow failed");
        return;
    }
    if (!verify_pattern(q, small, pattern_a)) {
        free(q);
        fail(name, "prefix corrupted after grow");
        return;
    }

    fill_pattern(q, large, pattern_b);
    uint8_t *r = (uint8_t *)realloc(q, 2048);
    if (!r) {
        free(q);
        fail(name, "realloc shrink failed");
        return;
    }
    if (!verify_pattern(r, 2048, pattern_b)) {
        free(r);
        fail(name, "prefix corrupted after shrink");
        return;
    }

    free(r);
    pass(name);
}

static void test_malloc_fragmentation() {
    const char *name = "malloc fragmentation stress";
    const size_t count = 96;
    void *ptrs[count];
    size_t sizes[count];

    for (size_t i = 0; i < count; i++) {
        ptrs[i] = NULL;
        sizes[i] = 17 + ((i * 977) % 8192);
    }

    for (size_t i = 0; i < count; i++) {
        ptrs[i] = malloc(sizes[i]);
        if (!ptrs[i]) {
            fail(name, "initial allocation failed");
            for (size_t j = 0; j < count; j++) free(ptrs[j]);
            return;
        }
        memset(ptrs[i], (int)(i ^ 0x5A), sizes[i]);
    }

    for (size_t i = 1; i < count; i += 2) {
        free(ptrs[i]);
        ptrs[i] = NULL;
    }

    for (size_t i = 1; i < count; i += 2) {
        sizes[i] = 33 + ((i * 1231) % 16384);
        ptrs[i] = malloc(sizes[i]);
        if (!ptrs[i]) {
            fail(name, "re-allocation into holes failed");
            for (size_t j = 0; j < count; j++) free(ptrs[j]);
            return;
        }
        memset(ptrs[i], (int)(i ^ 0xA5), sizes[i]);
    }

    for (size_t i = 0; i < count; i++) {
        uint8_t expected = (uint8_t)((i & 1) ? (i ^ 0xA5) : (i ^ 0x5A));
        uint8_t *p = (uint8_t *)ptrs[i];
        for (size_t j = 0; j < sizes[i]; j++) {
            if (p[j] != expected) {
                fail(name, "block contents corrupted");
                for (size_t k = 0; k < count; k++) free(ptrs[k]);
                return;
            }
        }
    }

    for (size_t i = 0; i < count; i++) free(ptrs[i]);
    pass(name);
}

static void test_mmap_anonymous() {
    const char *name = "mmap anonymous zero/write/read/munmap";
    const size_t len = 128 * 1024 + 333;
    uint8_t *p = (uint8_t *)mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fail(name, "mmap returned MAP_FAILED");
        return;
    }

    for (size_t i = 0; i < len; i++) {
        if (p[i] != 0) {
            munmap(p, len);
            fail(name, "anonymous mmap was not zero-filled");
            return;
        }
    }

    fill_pattern(p, len, pattern_b);
    if (!verify_pattern(p, len, pattern_b)) {
        munmap(p, len);
        fail(name, "mmap contents corrupted");
        return;
    }

    if (munmap(p, len) != 0) {
        fail(name, "munmap failed");
        return;
    }

    pass(name);
}

static void test_mmap_page_boundaries() {
    const char *name = "mmap page-boundary writes";
    const size_t len = 3 * 4096;
    uint8_t *p = (uint8_t *)mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fail(name, "mmap failed");
        return;
    }

    const size_t offsets[] = {0, 1, 4095, 4096, 4097, 8191, 8192, 12287};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        p[offsets[i]] = (uint8_t)(0x10 + i);
    }
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        if (p[offsets[i]] != (uint8_t)(0x10 + i)) {
            munmap(p, len);
            fail(name, "boundary byte changed");
            return;
        }
    }

    munmap(p, len);
    pass(name);
}

static void test_mmap_fixed() {
    const char *name = "mmap MAP_FIXED high address";
    void *want = (void *)0x500000000000ULL;
    const size_t len = 2 * 4096;
    uint8_t *p = (uint8_t *)mmap(want, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        fail(name, "mmap fixed returned MAP_FAILED");
        return;
    }
    if (p != want) {
        munmap(p, len);
        fail(name, "mmap fixed returned wrong address");
        return;
    }

    p[0] = 0x12;
    p[4095] = 0x34;
    p[4096] = 0x56;
    p[8191] = 0x78;
    if (p[0] != 0x12 || p[4095] != 0x34 || p[4096] != 0x56 || p[8191] != 0x78) {
        munmap(p, len);
        fail(name, "fixed mapping contents incorrect");
        return;
    }

    if (munmap(p, len) != 0) {
        fail(name, "munmap fixed failed");
        return;
    }

    pass(name);
}

static void test_mmap_invalid() {
    const char *name = "mmap invalid zero length rejected";
    void *p = mmap(NULL, 0, PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        fail(name, "zero-length mmap unexpectedly succeeded");
        return;
    }
    pass(name);
}


static void test_stack_growth() {
    const char *name = "user stack grows on demand near 8 MiB limit";
    uintptr_t sp = 0;
    asm volatile("mov %%rsp, %0" : "=r"(sp));

    // The kernel initially commits a small stack and reserves an 8 MiB window.
    // Touch well below the initial commitment to force multiple grow faults,
    // but stay comfortably above the guard at the bottom of the reserved stack.
    const size_t depth = 7 * 1024 * 1024;
    volatile uint8_t *base = (volatile uint8_t *)(sp - depth);

    for (size_t off = 0; off < depth; off += 4096) {
        base[off] = (uint8_t)(off >> 12);
    }
    for (size_t off = 0; off < depth; off += 4096) {
        if (base[off] != (uint8_t)(off >> 12)) {
            fail(name, "stack-grown page contents changed");
            return;
        }
    }

    pass(name);
}

static void test_mmap_rejects_stack_window() {
    const char *name = "mmap rejects reserved stack window";
    void *want = (void *)0x00007FFFFFFFE000ULL;
    void *p = mmap(want, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != MAP_FAILED) {
        munmap(p, 4096);
        fail(name, "mmap unexpectedly mapped over the stack window");
        return;
    }
    pass(name);
}

static void test_large_heap_pressure() {
    const char *name = "large heap pressure 1 MiB";
    const size_t len = 1024 * 1024;
    uint8_t *p = (uint8_t *)malloc(len);
    if (!p) {
        fail(name, "malloc 1 MiB failed");
        return;
    }

    for (size_t i = 0; i < len; i += 4096) {
        p[i] = (uint8_t)(i >> 12);
        p[i + 4095] = (uint8_t)(~(i >> 12));
    }
    for (size_t i = 0; i < len; i += 4096) {
        if (p[i] != (uint8_t)(i >> 12) || p[i + 4095] != (uint8_t)(~(i >> 12))) {
            free(p);
            fail(name, "page edge contents corrupted");
            return;
        }
    }

    free(p);
    pass(name);
}

int main() {
    printf("memtest: starting\n");
    fflush(stdout);

    test_sbrk_basic();
    test_brk_exact();
    test_malloc_various_sizes();
    test_calloc_zero();
    test_realloc_grow_shrink();
    test_malloc_fragmentation();
    test_mmap_anonymous();
    test_mmap_page_boundaries();
    test_mmap_fixed();
    test_mmap_invalid();
    test_stack_growth();
    test_mmap_rejects_stack_window();
    test_large_heap_pressure();

    printf("memtest: %d/%d checks passed\n", g_checks - g_failures, g_checks);
    if (g_failures == 0) {
        printf("memtest: PASS\n");
        return 0;
    }

    printf("memtest: FAIL (%d failure%s)\n", g_failures, g_failures == 1 ? "" : "s");
    return 1;
}
