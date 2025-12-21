/**
 * @file util.h
 * @brief Utility functions and macros.
 *
 * This header provides basic kernel utilities.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

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
static inline void* p2v(uint64_t phys) { return (void*)(phys + PHYS_MAP_BASE); }

/**
 * @brief Convert a virtual address to a physical address
 * @param virt The virtual address
 * @return The corresponding physical address
 */
static inline uint64_t v2p(void* virt) { return (uint64_t)virt - PHYS_MAP_BASE; }

/**
 * @brief Convert a physical address to a kernel virtual address
 * @param phys The physical address
 * @return The corresponding kernel virtual address
 */
static inline void* kp2v(uint64_t phys) { return (void*)(phys + KERNEL_VIRT_OFFSET); }

/**
 * @brief Convert a kernel virtual address to a physical address
 * @param virt The kernel virtual address
 * @return The corresponding physical address
 */
static inline uint64_t kv2p(void* virt) { return (uint64_t)virt - KERNEL_VIRT_OFFSET; }

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

/**
 * @brief Write a byte to an I/O port
 * @param port The I/O port
 * @param val The byte value to write
 */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * @brief Read a byte from an I/O port
 * @param port The I/O port
 * @return The byte value read from the port
 */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * @brief Write a word (16-bit) to an I/O port
 * @param port The I/O port
 * @param val The word value to write
 *
 */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * @brief Read a word (16-bit) from an I/O port
 * @param port The I/O port
 * @return The word value read from the port
 */
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}