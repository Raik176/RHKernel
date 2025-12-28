/**
 * @file pmm.cpp
 * @brief Implementation of the Physical Memory Manager (PMM)
 *
 * Uses a buddy allocator to manage physical memory.
 * Provides functions for allocating/freeing pages, initializing memory,
 * and querying memory statistics.
 */

#include "memory/pmm.h"

#include "console.h"
#include "multiboot2.h"
#include "smp/lock.h"
#include "string.h"
#include "util.h"

extern "C" {
extern uint8_t _kernel_phys_start[];
extern uint8_t _kernel_phys_end[];
extern uint8_t pml4_table[];
extern uint8_t pml4_table_end[];
extern uint8_t pdp_table[];
extern uint8_t pdp_table_end[];
extern uint8_t page_directory[];
extern uint8_t page_directory_end[];
extern uint8_t phys_map_pdp_table[];
extern uint8_t phys_map_pdp_table_end[];
extern uint8_t phys_map_pd_table[];
extern uint8_t phys_map_pd_table_end[];
extern uint8_t high_pdp_table[];
extern uint8_t high_pdp_table_end[];
extern uint8_t high_pd_table[];
extern uint8_t high_pd_table_end[];
}

namespace {
    static lock::spinlock pmm_lock;

    size_t managed_bytes = 0;  ///< Total memory managed by PMM
    size_t free_bytes = 0;     ///< Currently free memory
    size_t system_bytes = 0;   ///< Total system RAM detected
}  // namespace

namespace pmm {

    /**
     * @internal Linked list node representing a free memory block
     */
    struct FreeBlock {
        FreeBlock* next;
    };

    static_assert(sizeof(FreeBlock) <= pmm::PAGE_SIZE, "FreeBlock must fit within a single page");
    static_assert(alignof(FreeBlock) <= pmm::PAGE_SIZE, "FreeBlock alignment exceeds page size");
    static_assert((1ULL << pmm::MAX_ORDER) * pmm::PAGE_SIZE > 0, "block_size computation overflows");

    static FreeBlock* free_lists[MAX_ORDER + 1];  ///< Free block lists per buddy order
    static_assert(sizeof(free_lists) / sizeof(free_lists[0]) == pmm::MAX_ORDER + 1, "free_lists must have MAX_ORDER + 1 entries");

    /**
     * @brief Convert a size in bytes to buddy allocator order
     *
     * @param size Size in bytes
     * @return Minimum order that can hold `size`
     */
    size_t size_to_order(size_t size) {
        size = align_up(size, PAGE_SIZE);
        size_t pages = size / PAGE_SIZE;
        size_t order = 0;
        while ((1ULL << order) < pages) order++;
        return order;
    }

    /**
     * @internal Return block size for a given order
     */
    static uint64_t block_size(size_t order) { return (1ULL << order) * PAGE_SIZE; }

    /**
     * @internal Compute the buddy address of a block
     */
    static uint64_t buddy_of(uint64_t addr, size_t order) { return addr ^ block_size(order); }

    /**
     * @internal Push a free block into its free list
     */
    static void push_block(uint64_t addr, size_t order) {
        FreeBlock* block = (FreeBlock*)p2v(addr);
        block->next = free_lists[order];
        free_lists[order] = block;

        free_bytes += block_size(order);
    }

    /**
     * @internal Pop a free block from the free list
     */
    static uint64_t pop_block(size_t order) {
        FreeBlock* block = free_lists[order];
        if (!block) return 0;
        free_lists[order] = block->next;

        free_bytes -= block_size(order);
        return v2p(block);
    }

    /**
     * @internal Check if two memory regions overlap
     */
    static inline bool overlaps(uint64_t start, uint64_t len, uint64_t r_start, uint64_t r_size) {
        return (start < r_start + r_size) && (r_start < start + len);
    }

    /**
     * @internal Remove a specific block from the free list
     *
     * @param phys_addr Physical address of block
     * @param order Order of the block
     * @return True if block was found and removed
     */
    static bool remove_block(uint64_t phys_addr, size_t order) {
        FreeBlock** cur = &free_lists[order];
        FreeBlock* target_virt = (FreeBlock*)p2v(phys_addr);
        while (*cur) {
            if (*cur == target_virt) {
                *cur = (*cur)->next;
                free_bytes -= block_size(order);
                return true;
            }
            cur = &(*cur)->next;
        }
        return false;
    }

    /**
     * @internal Add a contiguous memory span to free lists
     */
    void add_span(uint64_t start, uint64_t end) {
        uint64_t curr = align_up(start, PAGE_SIZE);
        uint64_t last = align_down(end, PAGE_SIZE);

        while (curr < last) {
            uint64_t remaining = last - curr;

            size_t align_order = (curr == 0) ? MAX_ORDER : (size_t)__builtin_ctzll(curr >> 12);

            size_t size_order = 63 - __builtin_clzll(remaining >> 12);

            size_t order = align_order;
            if (order > size_order) order = size_order;
            if (order > MAX_ORDER) order = MAX_ORDER;

            uint64_t sz = block_size(order);

            managed_bytes += sz;

            push_block(curr, order);
            curr += sz;
        }
    }

    /**
     * @internal Free a memory region while respecting reserved areas
     */
    void free_with_reservation(uint64_t start, uint64_t end, const uint64_t* reserved,
                               size_t res_count) {
        if (start >= end) return;

        for (size_t i = 0; i < res_count; i++) {
            uint64_t res_start = reserved[i * 2];
            uint64_t res_end = reserved[i * 2 + 1];

            if (start < res_end && res_start < end) {
                if (start < res_start) {
                    free_with_reservation(start, res_start, reserved + (i + 1) * 2,
                                          res_count - (i + 1));
                }
                if (end > res_end) {
                    free_with_reservation(res_end, end, reserved + (i + 1) * 2,
                                          res_count - (i + 1));
                }
                return;
            }
        }
        add_span(start, end);
    }

    /**
     * @brief Initialize the PMM using the Multiboot memory map
     *
     * Sets up free lists and reserves memory used by the kernel and critical structures.
     *
     * @param mb_phys_addr Physical address of the multiboot memory map
     */
    void init(uint64_t mb_phys_addr) {
        managed_bytes = 0;
        free_bytes = 0;
        system_bytes = 0;

        for (size_t i = 0; i <= MAX_ORDER; i++) free_lists[i] = nullptr;

        uint8_t* mb_ptr = (uint8_t*)p2v(mb_phys_addr);
        uint32_t mb_size = *(uint32_t*)mb_ptr;

        multiboot_tag_mmap* mmap = nullptr;
        for (multiboot_tag* tag = (multiboot_tag*)(mb_ptr + 8); tag->type != MULTIBOOT_TAG_TYPE_END;
             tag = (multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7))) {
            if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
                mmap = (multiboot_tag_mmap*)tag;
                break;
            }
        }
        if (!mmap) return;

        uint64_t initramfs_start = 0;
        uint64_t initramfs_end = 0;

        for (multiboot_tag* tag = (multiboot_tag*)(mb_ptr + 8); tag->type != MULTIBOOT_TAG_TYPE_END;
             tag = (multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7))) {
            if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
                multiboot_tag_module* mod = (multiboot_tag_module*)tag;

                if (strcmp(mod->cmdline, "initramfs") == 0) {
                    initramfs_start = mod->mod_start;
                    initramfs_end = mod->mod_end;
                    break;
                }
            }
        }

        if (initramfs_start == 0 && initramfs_end == 0) return;

        uint64_t reserved[] = {
            0,
            0x100000,
            (uint64_t)&_kernel_phys_start,
            (uint64_t)&_kernel_phys_end,

            (uint64_t)&pml4_table,
            (uint64_t)&pml4_table + (uint64_t)&pml4_table_end - (uint64_t)&pml4_table,
            (uint64_t)&pdp_table,
            (uint64_t)&pdp_table + (uint64_t)&pdp_table_end - (uint64_t)&pdp_table,
            (uint64_t)&page_directory,
            (uint64_t)&page_directory + (uint64_t)&page_directory_end - (uint64_t)&page_directory,
            (uint64_t)&phys_map_pdp_table,
            (uint64_t)&phys_map_pdp_table + (uint64_t)&phys_map_pdp_table_end -
                (uint64_t)&phys_map_pdp_table,
            (uint64_t)&phys_map_pd_table,
            (uint64_t)&phys_map_pd_table + (uint64_t)&phys_map_pd_table_end -
                (uint64_t)&phys_map_pd_table,
            (uint64_t)&high_pdp_table,
            (uint64_t)&high_pdp_table + (uint64_t)&high_pdp_table_end - (uint64_t)&high_pdp_table,
            (uint64_t)&high_pd_table,
            (uint64_t)&high_pd_table + (uint64_t)&high_pd_table_end - (uint64_t)&high_pd_table,

            mb_phys_addr,
            mb_phys_addr + mb_size,
            initramfs_start,
            initramfs_end};
        size_t res_count = sizeof(reserved) / (sizeof(uint64_t) * 2);

        for (auto* e = mmap->entries; (uint8_t*)e < (uint8_t*)mmap + mmap->size;
             e = (multiboot_mmap_entry*)((uint8_t*)e + mmap->entry_size)) {
            system_bytes += e->len;

            if (e->type != MULTIBOOT_MEMORY_AVAILABLE) continue;

            free_with_reservation(e->addr, e->addr + e->len, reserved, res_count);
        }
    }

    /**
     * @brief Free a previously allocated physical memory block
     *
     * Attempts to coalesce with its buddy before returning to the free list.
     *
     * @param phys Physical address of the block
     * @param size Size in bytes
     */
    void free(uint64_t phys, size_t size) {
        if (!phys || size == 0) return;

        uint64_t flags;
        pmm_lock.acquire(flags);

        size_t order = size_to_order(size);
        uint64_t addr = phys;
        while (order < MAX_ORDER) {
            uint64_t buddy = buddy_of(addr, order);
            if (!remove_block(buddy, order)) break;
            addr = (addr < buddy) ? addr : buddy;
            order++;
        }
        push_block(addr, order);

        pmm_lock.release(flags);
    }

    /**
     * @brief Allocate a physical memory block
     *
     * Finds the smallest suitable block and splits larger blocks if necessary.
     *
     * @param size Size in bytes
     * @return Physical address of the allocated block or 0 if unavailable
     */
    uint64_t alloc(size_t size) {
        if (size == 0) return 0;

        uint64_t flags;
        pmm_lock.acquire(flags);

        size_t order = size_to_order(size);
        for (size_t i = order; i <= MAX_ORDER; i++) {
            uint64_t block = pop_block(i);
            if (!block) continue;

            while (i > order) {
                i--;
                uint64_t buddy = block + block_size(i);
                push_block(buddy, i);
            }

            pmm_lock.release(flags);
            return block;
        }

        pmm_lock.release(flags);
        return 0;
    }

    /**
     * @brief Get total memory managed in bytes
     */
    size_t get_total_bytes() { return managed_bytes; }

    /**
     * @brief Get free memory in bytes
     */
    size_t get_free_bytes() { return free_bytes; }

    /**
     * @brief Get total system RAM in bytes
     */
    size_t get_system_bytes() { return system_bytes; }

}  // namespace pmm