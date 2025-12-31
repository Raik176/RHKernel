/**
 * @file util.h
 * @brief Utility functions and macros.
 *
 * This header provides basic kernel utilities.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "portio.h"
#include "smp/apic.h"

struct cpu_features {
    bool smap;
    bool smep;
};

/**
 * @brief CPU register state structure
 *
 * Represents the state of registers pushed to the stack during
 * an interrupt, exception, or thread switch.
 */
struct regs {
    uint64_t r15;  ///< General-purpose register R15
    uint64_t r14;  ///< General-purpose register R14
    uint64_t r13;  ///< General-purpose register R13
    uint64_t r12;  ///< General-purpose register R12
    uint64_t r11;  ///< General-purpose register R11
    uint64_t r10;  ///< General-purpose register R10
    uint64_t r9;   ///< General-purpose register R9
    uint64_t r8;   ///< General-purpose register R8
    uint64_t rbp;  ///< Base pointer register
    uint64_t rdi;  ///< Destination index register
    uint64_t rsi;  ///< Source index register
    uint64_t rdx;  ///< Data register
    uint64_t rcx;  ///< Counter register
    uint64_t rbx;  ///< Base register
    uint64_t rax;  ///< Accumulator register

    uint64_t int_no;    ///< Interrupt number, only for interrupts
    uint64_t err_code;  ///< Error code, only for interrupts

    uint64_t rip;     ///< Instruction pointer
    uint64_t cs;      ///< Code segment
    uint64_t rflags;  ///< CPU flags register
    uint64_t rsp;     ///< Stack pointer
    uint64_t ss;      ///< Stack segment
} __attribute__((packed));

/**
 * @brief Kernel virtual memory offset
 *
 * All physical addresses are mapped at this offset in the virtual address space
 */
#define KERNEL_VIRT_OFFSET 0xFFFFFFFF80000000ULL
#define PHYS_MAP_BASE 0xFFFF880000000000ULL

/**
 * @brief Convert a physical address to a virtual address
 * @param phys The physical address
 * @return The corresponding virtual address
 */
static inline void *p2v(uint64_t phys) { return (void *)(phys + PHYS_MAP_BASE); }

/**
 * @brief Convert a virtual address to a physical address
 * @param virt The virtual address
 * @return The corresponding physical address
 */
static inline uint64_t v2p(void *virt) { return (uint64_t)virt - PHYS_MAP_BASE; }

/**
 * @brief Convert a physical address to a kernel virtual address
 * @param phys The physical address
 * @return The corresponding kernel virtual address
 */
static inline void *kp2v(uint64_t phys) { return (void *)(phys + KERNEL_VIRT_OFFSET); }

/**
 * @brief Convert a kernel virtual address to a physical address
 * @param virt The kernel virtual address
 * @return The corresponding physical address
 */
static inline uint64_t kv2p(void *virt) { return (uint64_t)virt - KERNEL_VIRT_OFFSET; }

static inline uint64_t align_to(uint64_t val, uint64_t align) {
    if (align == 0) return val;
    return (val + align - 1) & ~(align - 1);
}

/**
 * @brief Align an address upward to the nearest multiple of `align`
 * @param addr The address to align
 * @param align The alignment boundary (must be a power of two)
 * @return The aligned address
 */
static inline uint64_t align_up(uint64_t addr, uint64_t align) {
    return (addr + align - 1) & ~(align - 1);
}

/**
 * @brief Align an address downward to the nearest multiple of `align`
 * @param addr The address to align
 * @param align The alignment boundary (must be a power of two)
 * @return The aligned address
 */
static inline uint64_t align_down(uint64_t addr, uint64_t align) { return addr & ~(align - 1); }

void __attribute__((noreturn)) kpanic(const char *message, struct regs *r = nullptr);
void print_stacktrace();
void busy_sleep(uint64_t ms);
void dump_regs(struct regs *r);