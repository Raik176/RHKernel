#include "security/stack_protector.h"

#include "console.h"
#include "util.h"

extern "C" {
__attribute__((used, externally_visible)) uintptr_t __stack_chk_guard = 0x2d2d6b737461636bULL;
}

namespace {
    static inline uint64_t rdtsc() {
        uint32_t lo, hi;
        asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }

    static uint64_t mix(uint64_t x) {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }
}

namespace stack_protector {
    __attribute__((no_stack_protector)) void init(uint64_t boot_entropy) {
        uint64_t rsp;
        asm volatile("mov %%rsp, %0" : "=r"(rsp));

        uint64_t guard = mix(boot_entropy ^ rdtsc() ^ rsp ^ (uint64_t)&guard ^
                             (uint64_t)&__stack_chk_guard);
        guard &= ~0xffULL;
        if (guard == 0) guard = 0x00d0fefe5afe0000ULL;
        __stack_chk_guard = guard;
    }
}

extern "C" __attribute__((noreturn, no_stack_protector, used, externally_visible)) void __stack_chk_fail() {
    kpanic("stack canary corrupted");
}
