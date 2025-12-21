#include "pmm.h"
#include "util.h"
#include "multiboot2.h"
#include "console.h"

extern "C" {
    extern uint8_t _kernel_phys_start[];
    extern uint8_t _kernel_phys_end[];
    extern uint8_t pml4_table[];
    extern uint8_t pdp_table[];
    extern uint8_t page_directory[];
    extern uint8_t phys_map_pdp_table[];
    extern uint8_t phys_map_pd_table[];
    extern uint8_t high_pdp_table[];
    extern uint8_t high_pd_table[];
}

namespace {
    size_t managed_bytes = 0;
    size_t free_bytes = 0;
    size_t system_bytes = 0;
}

namespace pmm {

struct FreeBlock {
    FreeBlock* next;
};

static FreeBlock* free_lists[MAX_ORDER + 1];

static inline size_t align_up(size_t x, size_t a) {
    return (x + a - 1) & ~(a - 1);
}

size_t size_to_order(size_t size) {
    size = align_up(size, PAGE_SIZE);
    size_t pages = size / PAGE_SIZE;
    size_t order = 0;
    while ((1ULL << order) < pages) order++;
    return order;
}

static uint64_t block_size(size_t order) {
    return (1ULL << order) * PAGE_SIZE;
}

static uint64_t buddy_of(uint64_t addr, size_t order) {
    return addr ^ block_size(order);
}

static void push_block(uint64_t addr, size_t order) {
    FreeBlock* block = (FreeBlock*)p2v(addr);
    block->next = free_lists[order];
    free_lists[order] = block;

    free_bytes += block_size(order);
}

static uint64_t pop_block(size_t order) {
    FreeBlock* block = free_lists[order];
    if (!block) return 0;
    free_lists[order] = block->next;

    free_bytes -= block_size(order);
    return v2p(block);
}

static inline bool overlaps(uint64_t start, uint64_t len, uint64_t r_start, uint64_t r_size) {
    return (start < r_start + r_size) && (r_start < start + len);
}

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
 * Recursively carves a reserved range out of an available range.
 */
void free_with_reservation(uint64_t start, uint64_t end, const uint64_t* reserved, size_t res_count) {
    if (start >= end) return;

    for (size_t i = 0; i < res_count; i++) {
        uint64_t res_start = reserved[i * 2];
        uint64_t res_end = reserved[i * 2 + 1];

        // If this reserved block overlaps the current range
        if (start < res_end && res_start < end) {
            // Region before the reservation
            if (start < res_start) {
                free_with_reservation(start, res_start, reserved + (i + 1) * 2, res_count - (i + 1));
            }
            // Region after the reservation
            if (end > res_end) {
                free_with_reservation(res_end, end, reserved + (i + 1) * 2, res_count - (i + 1));
            }
            return;
        }
    }
    // No more overlaps, add this "clean" span
    add_span(start, end);
}

void init(uint64_t mb_phys_addr) {
    managed_bytes = 0;
    free_bytes = 0;
    system_bytes = 0;

    for (size_t i = 0; i <= MAX_ORDER; i++) free_lists[i] = nullptr;

    uint8_t* mb_ptr = (uint8_t*)p2v(mb_phys_addr);
    uint32_t mb_size = *(uint32_t*)mb_ptr;

    multiboot_tag_mmap* mmap = nullptr;
    for (multiboot_tag* tag = (multiboot_tag*)(mb_ptr + 8);
         tag->type != MULTIBOOT_TAG_TYPE_END;
         tag = (multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7))) {
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            mmap = (multiboot_tag_mmap*)tag;
            break;
        }
    }
    if (!mmap) return;

    uint64_t reserved[] = {
        0, 0x100000,
        (uint64_t)&_kernel_phys_start, (uint64_t)&_kernel_phys_end,
        
        (uint64_t)&pml4_table,           (uint64_t)&pml4_table + PAGE_SIZE,
        (uint64_t)&pdp_table,            (uint64_t)&pdp_table + PAGE_SIZE,
        (uint64_t)&page_directory,       (uint64_t)&page_directory + PAGE_SIZE,
        (uint64_t)&phys_map_pdp_table,   (uint64_t)&phys_map_pdp_table + PAGE_SIZE,
        (uint64_t)&phys_map_pd_table,    (uint64_t)&phys_map_pd_table + PAGE_SIZE,
        (uint64_t)&high_pdp_table,       (uint64_t)&high_pdp_table + PAGE_SIZE,
        (uint64_t)&high_pd_table,        (uint64_t)&high_pd_table + PAGE_SIZE,

        mb_phys_addr, mb_phys_addr + mb_size,
    };
    size_t res_count = sizeof(reserved) / (sizeof(uint64_t) * 2);

    for (auto* e = mmap->entries; (uint8_t*)e < (uint8_t*)mmap + mmap->size; 
         e = (multiboot_mmap_entry*)((uint8_t*)e + mmap->entry_size)) {
        
        system_bytes += e->len;

        if (e->type != MULTIBOOT_MEMORY_AVAILABLE) continue;

        free_with_reservation(e->addr, e->addr + e->len, reserved, res_count);
    }
}

void free(uint64_t phys, size_t size) {
    if (!phys || size == 0) return;
    size_t order = size_to_order(size);
    uint64_t addr = phys;
    while (order < MAX_ORDER) {
        uint64_t buddy = buddy_of(addr, order);
        if (!remove_block(buddy, order)) break;
        addr = (addr < buddy) ? addr : buddy;
        order++;
    }
    push_block(addr, order);
}

uint64_t alloc(size_t size) {
    if (size == 0) return 0;
    size_t order = size_to_order(size);
    for (size_t i = order; i <= MAX_ORDER; i++) {
        uint64_t block = pop_block(i);
        if (!block) continue;

        while (i > order) {
            i--;
            uint64_t buddy = block + block_size(i);
            push_block(buddy, i);
        }
        return block;
    }
    return 0;
}

size_t get_total_kb() { 
    return managed_bytes / 1024; 
}

size_t get_free_kb() { 
    return free_bytes / 1024; 
}

size_t get_system_kb() {
    return system_bytes / 1024;
}

} // namespace pmm