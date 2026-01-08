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
    static spinlock_t pmm_lock;

    size_t managed_bytes = 0;
    size_t free_bytes = 0;
    size_t system_bytes = 0;

    // CoW Support: Reference counting
    uint32_t *ref_counts = nullptr;
    size_t total_pages = 0;
}  // namespace

namespace pmm {

    struct FreeBlock {
        FreeBlock *next;
    };

    static FreeBlock *free_lists[MAX_ORDER + 1];

    size_t size_to_order(size_t size) {
        size = align_up(size, PAGE_SIZE);
        size_t pages = size / PAGE_SIZE;
        size_t order = 0;
        while ((1ULL << order) < pages) order++;
        return order;
    }

    static uint64_t block_size(size_t order) { return (1ULL << order) * PAGE_SIZE; }
    static uint64_t buddy_of(uint64_t addr, size_t order) { return addr ^ block_size(order); }

    static void push_block(uint64_t addr, size_t order) {
        FreeBlock *block = (FreeBlock *)p2v(addr);
        block->next = free_lists[order];
        free_lists[order] = block;
        free_bytes += block_size(order);
    }

    static uint64_t pop_block(size_t order) {
        FreeBlock *block = free_lists[order];
        if (!block) return 0;
        free_lists[order] = block->next;
        free_bytes -= block_size(order);
        return v2p(block);
    }

    static bool remove_block(uint64_t phys_addr, size_t order) {
        FreeBlock **cur = &free_lists[order];
        FreeBlock *target_virt = (FreeBlock *)p2v(phys_addr);
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

    void free_with_reservation(uint64_t start, uint64_t end, const uint64_t *reserved,
                               size_t res_count) {
        if (start >= end) return;
        for (size_t i = 0; i < res_count; i++) {
            uint64_t res_start = reserved[i * 2];
            uint64_t res_end = reserved[i * 2 + 1];
            if (start < res_end && res_start < end) {
                if (start < res_start)
                    free_with_reservation(start, res_start, reserved + (i + 1) * 2,
                                          res_count - (i + 1));
                if (end > res_end)
                    free_with_reservation(res_end, end, reserved + (i + 1) * 2,
                                          res_count - (i + 1));
                return;
            }
        }
        add_span(start, end);
    }

    void init(uint64_t mb_phys_addr) {
        managed_bytes = 0;
        free_bytes = 0;
        system_bytes = 0;

        for (size_t i = 0; i <= MAX_ORDER; i++) free_lists[i] = nullptr;

        uint8_t *mb_ptr = (uint8_t *)p2v(mb_phys_addr);
        multiboot_tag_mmap *mmap = nullptr;

        uint64_t max_phys_addr = 0;

        for (multiboot_tag *tag = (multiboot_tag *)(mb_ptr + 8);
             tag->type != MULTIBOOT_TAG_TYPE_END;
             tag = (multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7))) {
            if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
                mmap = (multiboot_tag_mmap *)tag;
                for (auto *e = mmap->entries; (uint8_t *)e < (uint8_t *)mmap + mmap->size;
                     e = (multiboot_mmap_entry *)((uint8_t *)e + mmap->entry_size)) {
                    if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
                        if (e->addr + e->len > max_phys_addr) max_phys_addr = e->addr + e->len;
                    }
                }
            }
        }

        if (!mmap) return;

        total_pages = max_phys_addr / PAGE_SIZE;

        uint64_t initramfs_start = 0, initramfs_end = 0;
        for (multiboot_tag *tag = (multiboot_tag *)(mb_ptr + 8);
             tag->type != MULTIBOOT_TAG_TYPE_END;
             tag = (multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7))) {
            if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
                multiboot_tag_module *mod = (multiboot_tag_module *)tag;
                if (strcmp(mod->cmdline, "initramfs") == 0) {
                    initramfs_start = mod->mod_start;
                    initramfs_end = mod->mod_end;
                    break;
                }
            }
        }

        uint64_t reserved[] = {0,
                               0x100000,
                               (uint64_t)&_kernel_phys_start,
                               (uint64_t)&_kernel_phys_end,
                               (uint64_t)&pml4_table,
                               (uint64_t)&pml4_table_end,
                               (uint64_t)&pdp_table,
                               (uint64_t)&pdp_table_end,
                               (uint64_t)&page_directory,
                               (uint64_t)&page_directory_end,
                               (uint64_t)&phys_map_pdp_table,
                               (uint64_t)&phys_map_pdp_table_end,
                               (uint64_t)&phys_map_pd_table,
                               (uint64_t)&phys_map_pd_table_end,
                               (uint64_t)&high_pdp_table,
                               (uint64_t)&high_pdp_table_end,
                               (uint64_t)&high_pd_table,
                               (uint64_t)&high_pd_table_end,
                               mb_phys_addr,
                               mb_phys_addr + *(uint32_t *)mb_ptr,
                               initramfs_start,
                               initramfs_end};
        size_t res_count = sizeof(reserved) / (sizeof(uint64_t) * 2);

        for (auto *e = mmap->entries; (uint8_t *)e < (uint8_t *)mmap + mmap->size;
             e = (multiboot_mmap_entry *)((uint8_t *)e + mmap->entry_size)) {
            system_bytes += e->len;
            if (e->type != MULTIBOOT_MEMORY_AVAILABLE) continue;
            free_with_reservation(e->addr, e->addr + e->len, reserved, res_count);
        }

        // Allocate the ref_counts array from the newly initialized buddy allocator
        size_t ref_counts_size = total_pages * sizeof(uint32_t);
        uint64_t ref_counts_phys = alloc(ref_counts_size);
        ref_counts = (uint32_t *)p2v(ref_counts_phys);
        memset(ref_counts, 0, ref_counts_size);
    }

    void ref_page(uint64_t phys) {
        uint64_t idx = phys / PAGE_SIZE;
        uint64_t flags;
        pmm_lock.acquire(flags);
        if (idx < total_pages) ref_counts[idx]++;
        pmm_lock.release(flags);
    }

    void unref_page(uint64_t phys) {
        uint64_t idx = phys / PAGE_SIZE;
        uint64_t flags;
        pmm_lock.acquire(flags);
        if (idx < total_pages) {
            if (ref_counts[idx] > 0) ref_counts[idx]--;
            if (ref_counts[idx] == 0) {
                pmm_lock.release(flags);
                free(phys, PAGE_SIZE);
                return;
            }
        }
        pmm_lock.release(flags);
    }

    uint32_t get_ref(uint64_t phys) {
        uint64_t idx = phys / PAGE_SIZE;
        if (idx >= total_pages) return 0;
        return ref_counts[idx];
    }

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
            // If it's a single page allocation, initialize refcount to 1
            if (size <= PAGE_SIZE && ref_counts) { ref_counts[block / PAGE_SIZE] = 1; }
            pmm_lock.release(flags);
            return block;
        }
        pmm_lock.release(flags);
        return 0;
    }

    size_t get_total_bytes() { return managed_bytes; }
    size_t get_free_bytes() { return free_bytes; }
    size_t get_system_bytes() { return system_bytes; }

}  // namespace pmm