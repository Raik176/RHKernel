#pragma once
#include <stdint.h>

#include "util.h"

namespace vmm {

    /**
     * @brief Page table entry flags for x86-64 paging.
     *
     * These flags map directly to hardware-defined bits in x86-64
     * page table entries (PML4E/PDPTE/PDE/PTE).
     */
    enum class PageFlags : uint64_t {
        None = 0,

        /** Page is present in memory */
        Present = 1ULL << 0,

        /** Page is writable */
        Write = 1ULL << 1,

        /** Page is accessible from user mode (ring 3) */
        User = 1ULL << 2,

        /** Write-through caching */
        WriteThrough = 1ULL << 3,

        /** Disable caching (useful for MMIO) */
        NoCache = 1ULL << 4,

        /** Page has been accessed (set by CPU) */
        Accessed = 1ULL << 5,

        /** Page has been written to (set by CPU) */
        Dirty = 1ULL << 6,

        /** Page is a large page (2 MiB or 1 GiB) */
        Huge = 1ULL << 7,

        /** Global page (not flushed from TLB on CR3 reload) */
        Global = 1ULL << 8,

        /** Disable instruction fetch (NX bit) */
        NX = 1ULL << 63
    };

    enum class PageSize { Size4K, Size2M, Size1G };

    /**
     * @brief Combine page flags using bitwise OR.
     */
    constexpr PageFlags operator|(PageFlags a, PageFlags b) {
        return static_cast<PageFlags>(static_cast<uint64_t>(a) | static_cast<uint64_t>(b));
    }

    /**
     * @brief Mask page flags using bitwise AND.
     */
    constexpr PageFlags operator&(PageFlags a, PageFlags b) {
        return static_cast<PageFlags>(static_cast<uint64_t>(a) & static_cast<uint64_t>(b));
    }

    /**
     * @brief Initialize the virtual memory manager.
     *
     * Sets up paging structures and switches page tables
     */
    void init();

    /**
     * @brief Map a single 4 KiB virtual page to a physical frame.
     *
     * @param virt  Virtual address (must be page-aligned)
     * @param phys  Physical address (must be page-aligned)
     * @param flags PageFlags controlling access and caching behavior
     */
    void map_page(uint64_t virt, uint64_t phys, PageFlags flags, PageSize size);

    void map_range(uint64_t virt, uint64_t phys, uint64_t size, PageFlags flags);

    uint64_t get_kernel_pagemap();  // physical location!

    uint64_t create_user_address_space();

    uint64_t get_phys_addr_mask();
}  // namespace vmm