#pragma once
#include <stdint.h>

#include "smp/lock.h"
#include "util.h"

namespace vmm {
    static constexpr uint64_t MMIO_BASE = 0xFFFFFF0000000000ULL;
    static constexpr uint64_t MMIO_SIZE = 0x2000000000ULL;

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

        CoW = 1ULL << 9,

        /** Use PAT write-combining if available. */
        WriteCombining = 1ULL << 10,

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

    inline PageFlags &operator|=(PageFlags &a, PageFlags b) {
        a = a | b;
        return a;
    }

    /**
     * @brief Initialize the virtual memory manager.
     *
     * Sets up paging structures and switches page tables
     */
    void init();

    uint64_t get_kernel_pagemap();  // physical location!

    /**
     * @brief Map a single 4 KiB virtual page to a physical frame.
     *
     * @param virt  Virtual address (must be page-aligned)
     * @param phys  Physical address (must be page-aligned)
     * @param flags PageFlags controlling access and caching behavior
     */
    void map_page(uint64_t virt, uint64_t phys, PageFlags flags, PageSize size,
                  uint64_t pagemap = get_kernel_pagemap());

    void map_range(uint64_t virt, uint64_t phys, uint64_t size, PageFlags flags,
                   uint64_t pagemap = get_kernel_pagemap());

    void unmap_page(uint64_t virt, uint64_t pagemap = get_kernel_pagemap());
    void unmap_range(uint64_t virt, uint64_t size, uint64_t pagemap = get_kernel_pagemap());

    uint64_t create_user_address_space();
    void destroy_user_address_space(uint64_t pml4_phys);

    uint64_t clone_address_space(uint64_t old_pml4_phys);

    bool handle_fault(uint64_t fault_addr, uint64_t error_code, regs *r = nullptr);

    uint64_t get_mapping(uint64_t virt, uint64_t pagemap = get_kernel_pagemap());
    bool user_range_mapped(uint64_t virt, uint64_t size, bool write,
                           uint64_t pagemap = get_kernel_pagemap());
    void flush_tlb(uint64_t pagemap, uint64_t virt, uint64_t pages);
    uint64_t get_phys_addr_mask();
    bool nx_supported();
    bool pat_supported();
    bool write_combining_supported();
    bool page_1g_supported();
    uint64_t direct_map_bytes();

    class VirtualRangeAllocator {
       public:
        struct Segment {
            uint64_t start;
            uint64_t size;
            bool is_free;
            Segment *next;
            Segment *prev;
        };

        VirtualRangeAllocator(uint64_t base, uint64_t size);
        ~VirtualRangeAllocator();

        // Delete copy constructor and assignment to prevent double-free of segments
        VirtualRangeAllocator(const VirtualRangeAllocator &) = delete;
        VirtualRangeAllocator &operator=(const VirtualRangeAllocator &) = delete;

        /**
         * @brief Reserves a contiguous range of virtual addresses.
         * @return The starting virtual address, or 0 if allocation failed.
         */
        uint64_t allocate(uint64_t size);

        /**
         * @brief Returns a virtual range to the allocator and merges adjacent free blocks.
         */
        void free(uint64_t virt_addr);

       private:
        void coalesce(Segment *seg);

        uint64_t m_base;
        uint64_t m_total_size;
        Segment *m_head;
        spinlock_t m_lock;
    };

    void *mmio_map(uint64_t phys_addr, uint64_t size);
    void *mmio_map_wc(uint64_t phys_addr, uint64_t size);
    void mmio_unmap(void *virt_addr, uint64_t size);
}  // namespace vmm