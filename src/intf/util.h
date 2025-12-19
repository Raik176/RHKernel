#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stddef.h>

#define KERNEL_VIRT_OFFSET 0xFFFFFFFF80000000
#define PAGE_SIZE          4096

static inline void* p2v(uint64_t phys) {
    return (void*)(phys + KERNEL_VIRT_OFFSET);
}

static inline uint64_t v2p(void* virt) {
    return (uint64_t)virt - KERNEL_VIRT_OFFSET;
}

static inline uint64_t align_up(uint64_t addr, uint64_t align) {
    return (addr + align - 1) & ~(align - 1);
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#endif // UTIL_H