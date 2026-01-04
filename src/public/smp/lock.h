#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spinlock {
    volatile int locked;

#ifdef __cplusplus
    inline spinlock();

    inline void acquire(uint64_t &flags);
    inline void release(uint64_t flags);
    inline bool try_acquire(uint64_t &flags);
#endif
} spinlock_t;

static inline void spinlock_init(struct spinlock *lock) { lock->locked = 0; }

static inline void spinlock_acquire(struct spinlock *lock, uint64_t *flags) {
    asm volatile("pushf\ncli\npop %0" : "=r"(*flags) : : "memory");

    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) { asm volatile("pause"); }
}

static inline void spinlock_release(struct spinlock *lock, uint64_t flags) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
    asm volatile("push %0\npopf" : : "r"(flags) : "memory");
}

static inline bool spinlock_try_acquire(struct spinlock *lock, uint64_t *flags) {
    uint64_t f;
    asm volatile("pushf\ncli\npop %0" : "=r"(f) : : "memory");

    if (!__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE)) {
        *flags = f;
        return true;
    }

    asm volatile("push %0\npopf" : : "r"(f) : "memory");
    return false;
}

#ifdef __cplusplus
}  // end extern "C"

spinlock::spinlock() { spinlock_init(this); }
inline void spinlock::acquire(uint64_t &flags) { spinlock_acquire(this, &flags); }
inline void spinlock::release(uint64_t flags) { spinlock_release(this, flags); }
inline bool spinlock::try_acquire(uint64_t &flags) { return spinlock_try_acquire(this, &flags); }
#endif