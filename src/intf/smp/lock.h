#pragma once
#include <stdint.h>

namespace lock {
    struct spinlock {
        volatile int locked;

        spinlock() : locked(0) {}

        void acquire(uint64_t& flags) {
            asm volatile("pushf\ncli\npop %0" : "=r"(flags)::"memory");

            while (__atomic_test_and_set(&locked, __ATOMIC_ACQUIRE)) { asm volatile("pause"); }
        }

        void release(uint64_t flags) {
            __atomic_clear(&locked, __ATOMIC_RELEASE);
            asm volatile("push %0\npopf" ::"r"(flags) : "memory");
        }

        bool try_acquire(uint64_t& flags) {
            uint64_t f;
            asm volatile("pushf\ncli\npop %0" : "=r"(f)::"memory");

            if (!__atomic_test_and_set(&locked, __ATOMIC_ACQUIRE)) {
                flags = f;
                return true;
            }

            asm volatile("push %0\npopf" ::"r"(f) : "memory");
            return false;
        }
    };
}  // namespace lock