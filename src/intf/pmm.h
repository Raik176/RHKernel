/**
 * @file pmm.h
 * @brief Physical Memory Manager (PMM) interface
 *
 * This file defines the interface for managing physical memory in the kernel
 * using a buddy allocator. It provides functions to initialize the allocator,
 * allocate and free physical memory blocks, and query total and free memory.
 */

#pragma once
#include <stddef.h>
#include <stdint.h>

namespace pmm {

    /** @brief Size of a memory page in bytes */
    constexpr size_t PAGE_SIZE = 4096;

    /** @brief Maximum order for buddy allocator (2^MAX_ORDER pages per block) */
    constexpr size_t MAX_ORDER = 11;  ///< 2^11 pages = 8 MiB blocks

    /**
     * @brief Initialize the physical memory manager
     *
     * Sets up the buddy allocator structures and prepares physical memory
     * management using the information from the Multiboot memory map.
     *
     * @param mb_phys_addr Physical address of the Multiboot memory map
     */
    void init(uint64_t mb_phys_addr);

    /**
     * @brief Convert a size in bytes to the appropriate order
     *
     * Determines the minimum power-of-two block order that can satisfy
     * an allocation of the given size.
     *
     * @param size Number of bytes requested
     * @return Corresponding order for the buddy allocator
     */
    size_t size_to_order(size_t size);

    /**
     * @brief Allocate physical memory
     *
     * Allocates a contiguous block of physical memory using the buddy allocator.
     *
     * @param size Number of bytes to allocate
     * @return Physical address of the allocated block, or 0 if allocation failed
     */
    uint64_t alloc(size_t size);

    /**
     * @brief Free previously allocated physical memory
     *
     * Returns a block of physical memory back to the buddy allocator.
     *
     * @param phys Physical address of the block to free
     * @param size Size of the memory block originally allocated
     */
    void free(uint64_t phys, size_t size);

    /**
     * @brief Get total usable memory in kilobytes
     *
     * @return Total physical memory managed by PMM in bytes
     */
    size_t get_total_bytes();

    /**
     * @brief Get free memory in kilobytes
     *
     * @return Amount of currently free physical memory in bytes
     */
    size_t get_free_bytes();

    /**
     * @brief Get total system memory in kilobytes
     *
     * Returns the total amount of RAM in the system, including reserved or
     * unusable regions.
     *
     * @return Total system memory in bytes
     */
    size_t get_system_bytes();

}  // namespace pmm