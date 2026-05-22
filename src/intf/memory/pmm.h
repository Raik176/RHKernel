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

    static_assert((pmm::PAGE_SIZE & (pmm::PAGE_SIZE - 1)) == 0, "PAGE_SIZE must be a power of two");
    static_assert(pmm::PAGE_SIZE >= alignof(void *), "PAGE_SIZE must be pointer-aligned");
    static_assert(pmm::PAGE_SIZE == 4096, "PMM assumes 4 KiB hardware pages");

    /** @brief Maximum order for buddy allocator (2^MAX_ORDER pages per block) */
    constexpr size_t MAX_ORDER = 14;  ///< 2^14 pages = 64 MiB blocks
    static_assert(pmm::MAX_ORDER > 0, "MAX_ORDER must be > 0");
    static_assert(pmm::MAX_ORDER < 63, "MAX_ORDER too large for 64-bit math");

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
     * @brief Release usable RAM that was above the temporary boot direct-map.
     *
     * PMM initialization runs before the final VMM page tables exist, so it can
     * only write free-list nodes into pages covered by the small boot-time
     * direct map.  After vmm::init() installs the full direct map, this folds
     * the deferred high-memory spans into the buddy allocator.
     */
    void release_deferred_memory();

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

    /**
     * @brief Highest physical address limit that must be direct-mapped.
     *
     * This is not the same as total RAM. Machines with a PCI/MMIO hole below
     * 4 GiB can have usable RAM above 4 GiB, so the direct map must cover up
     * to the maximum address reported by the memory map, not just the sum of
     * usable/reserved byte counts.
     */
    size_t get_physical_limit_bytes();

    void ref_page(uint64_t phys);
    void unref_page(uint64_t phys);
    uint32_t get_ref(uint64_t phys);

}  // namespace pmm