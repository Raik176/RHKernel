/**
 * @file heap.cpp
 * @brief Implementation of kernel slab allocator
 *
 * Provides memory allocation for the kernel using a slab allocator.
 * Supports kmalloc/kfree and C++ new/delete operators.
 */

#include "memory/heap.h"

#include "memory/pmm.h"
#include "smp/smp.h"
#include "string.h"
#include "util.h"

namespace heap {
    static constexpr uint32_t SLAB_MAGIC = 0x534C4142;
    static constexpr uint32_t LARGE_MAGIC = 0x4C415247;
    static constexpr size_t SIZE_MAX_VALUE = ~(size_t)0;

    struct LargeAllocHeader {
        uint32_t magic;
        uint32_t reserved;
        size_t total_size;
        size_t requested_size;
    };

    static spinlock_t heap_stat_lock;
    static size_t slab_page_bytes = 0;
    static size_t large_alloc_bytes = 0;
    static size_t large_alloc_count = 0;

    static void add_stat(size_t *slot, size_t value) {
        uint64_t flags;
        heap_stat_lock.acquire(flags);
        *slot += value;
        heap_stat_lock.release(flags);
    }

    static void inc_stat(size_t *slot) { add_stat(slot, 1); }

    static void sub_stat(size_t *slot, size_t value) {
        uint64_t flags;
        heap_stat_lock.acquire(flags);
        *slot = *slot < value ? 0 : *slot - value;
        heap_stat_lock.release(flags);
    }

    static void dec_stat(size_t *slot) { sub_stat(slot, 1); }

    static bool mul_overflow(size_t a, size_t b, size_t *out) {
        if (!out) return true;
        if (a != 0 && b > SIZE_MAX_VALUE / a) return true;
        *out = a * b;
        return false;
    }

    static bool add_overflow(size_t a, size_t b, size_t *out) {
        if (!out) return true;
        if (a > SIZE_MAX_VALUE - b) return true;
        *out = a + b;
        return false;
    }

    static_assert(sizeof(SlabHeader) < pmm::PAGE_SIZE, "SlabHeader must fit inside a single page");
    static_assert(pmm::PAGE_SIZE == 4096, "Large allocation logic assumes 4 KiB pages");

    /**
     * @internal Remove a slab from a linked list
     *
     * @param head Pointer to the head of the list
     * @param slab Slab to remove
     */
    static void list_remove(SlabHeader **head, SlabHeader *slab) {
        if (slab->prev) slab->prev->next = slab->next;
        if (slab->next) slab->next->prev = slab->prev;
        if (*head == slab) *head = slab->next;
        slab->next = slab->prev = nullptr;
    }

    /**
     * @internal Push a slab to the front of a linked list
     *
     * @param head Pointer to the head of the list
     * @param slab Slab to push
     */
    static void list_push(SlabHeader **head, SlabHeader *slab) {
        slab->next = *head;
        if (*head) (*head)->prev = slab;
        *head = slab;
        slab->prev = nullptr;
    }

    /**
     * @internal Create a new slab for a specific slot size
     *
     * Allocates a physical page from PMM, maps it to a slab header, and initializes the free list.
     *
     * @param slot_size Size of each allocation in this slab
     * @return Pointer to the newly created SlabHeader, or nullptr on failure
     */
    static SlabHeader *create_slab(size_t slot_size) {
        if (slot_size == 0) return nullptr;

        uint64_t phys = pmm::alloc(pmm::PAGE_SIZE);
        if (!phys) return nullptr;

        SlabHeader *slab = (SlabHeader *)p2v(phys);

        slab->magic = SLAB_MAGIC;
        slab->next = nullptr;
        slab->prev = nullptr;
        slab->owner = nullptr;
        slab->used_slots = 0;

        size_t header_size = align_up(sizeof(SlabHeader), 16);
        size_t available_space = pmm::PAGE_SIZE - header_size;
        slab->total_slots = available_space / slot_size;
        if (slab->total_slots == 0) {
            pmm::free(phys, pmm::PAGE_SIZE);
            return nullptr;
        }
        add_stat(&slab_page_bytes, pmm::PAGE_SIZE);

        uint8_t *first_slot = (uint8_t *)slab + header_size;
        slab->slot_size = slot_size;
        slab->free_list = (void *)first_slot;

        for (size_t i = 0; i < slab->total_slots - 1; i++) {
            void **current = (void **)(first_slot + (i * slot_size));
            *current = (void *)(first_slot + ((i + 1) * slot_size));
        }

        void **last = (void **)(first_slot + ((slab->total_slots - 1) * slot_size));
        *last = nullptr;

        return slab;
    }

    static inline size_t cache_index_for_size(size_t size) {
        for (size_t i = 0; i < CACHE_COUNT; i++) {
            if (size <= caches[i].slot_size) return i;
        }
        return CACHE_COUNT;
    }

    static void *alloc_from_slab(SlabHeader *slab) {
        if (!slab || slab->magic != SLAB_MAGIC || !slab->free_list) return nullptr;
        void *ptr = slab->free_list;
        slab->free_list = *(void **)ptr;
        slab->used_slots++;
        return ptr;
    }

    static void retire_full_local_slab(SlabCache *cache, SlabHeader *slab, size_t cache_idx,
                                       smp::cpu_local *cpu) {
        if (!cpu || slab->used_slots != slab->total_slots) return;
        uint64_t flags;
        cache->lock.acquire(flags);
        slab->owner = nullptr;
        list_push(&cache->full_slabs, slab);
        cache->lock.release(flags);
        cpu->heap_cache[cache_idx] = nullptr;
    }

    static void *alloc_large(size_t size) {
        size_t with_header = 0;
        if (add_overflow(size, pmm::PAGE_SIZE, &with_header)) return nullptr;
        size_t order = pmm::size_to_order(with_header);
        if (order > pmm::get_max_order()) return nullptr;

        size_t actual_size = (1ULL << order) * pmm::PAGE_SIZE;
        uint64_t phys = pmm::alloc(actual_size);
        if (!phys) return nullptr;

        uint64_t virt = reinterpret_cast<uint64_t>(p2v(phys));
        LargeAllocHeader *header = reinterpret_cast<LargeAllocHeader *>(virt);
        header->magic = LARGE_MAGIC;
        header->reserved = 0;
        header->total_size = actual_size;
        header->requested_size = size;
        add_stat(&large_alloc_bytes, actual_size);
        inc_stat(&large_alloc_count);
        return reinterpret_cast<void *>(virt + pmm::PAGE_SIZE);
    }

    void *kmalloc(size_t size) {
        if (size == 0) return nullptr;

        size_t cache_idx = cache_index_for_size(size);
        if (cache_idx == CACHE_COUNT) return alloc_large(size);

        SlabCache *cache = &caches[cache_idx];
        smp::cpu_local *cpu = smp::get_cpu();
        if (cpu && cpu->self == cpu) {
            SlabHeader *local = cpu->heap_cache[cache_idx];
            if (local && local->free_list) {
                void *ptr = alloc_from_slab(local);
                retire_full_local_slab(cache, local, cache_idx, cpu);
                return ptr;
            }
        }

        uint64_t flags;
        cache->lock.acquire(flags);

        if (!cache->partial_slabs) {
            SlabHeader *new_slab = create_slab(cache->slot_size);
            if (!new_slab) {
                cache->lock.release(flags);
                return nullptr;
            }
            new_slab->cache_index = cache_idx;
            list_push(&cache->partial_slabs, new_slab);
        }

        SlabHeader *slab = cache->partial_slabs;
        if (cpu && cpu->self == cpu) {
            list_remove(&cache->partial_slabs, slab);
            slab->owner = cpu;
            cpu->heap_cache[cache_idx] = slab;
            cache->lock.release(flags);
            void *ptr = alloc_from_slab(slab);
            retire_full_local_slab(cache, slab, cache_idx, cpu);
            return ptr;
        }

        void *ptr = alloc_from_slab(slab);
        if (slab->used_slots == slab->total_slots) {
            list_remove(&cache->partial_slabs, slab);
            list_push(&cache->full_slabs, slab);
        }

        cache->lock.release(flags);
        return ptr;
    }

    void *kzalloc(size_t size) {
        void *ptr = kmalloc(size);
        if (ptr) memset(ptr, 0, size);
        return ptr;
    }

    void *kmalloc_array(size_t count, size_t size) {
        size_t bytes = 0;
        if (mul_overflow(count, size, &bytes)) return nullptr;
        return kmalloc(bytes);
    }

    void *kcalloc(size_t count, size_t size) {
        size_t bytes = 0;
        if (mul_overflow(count, size, &bytes)) return nullptr;
        return kzalloc(bytes);
    }

    void kfree(void *ptr) {
        if (!ptr) return;
        uint64_t virt_addr = (uint64_t)ptr;

        if ((virt_addr & 0xFFF) == 0) {
            uint64_t metadata_page = virt_addr - pmm::PAGE_SIZE;
            LargeAllocHeader *header = reinterpret_cast<LargeAllocHeader *>(metadata_page);
            if (header->magic != LARGE_MAGIC) kpanic("heap: bad large allocation");
            size_t total_size = header->total_size;
            header->magic = 0;
            sub_stat(&large_alloc_bytes, total_size);
            dec_stat(&large_alloc_count);
            pmm::free(v2p(reinterpret_cast<void *>(metadata_page)), total_size);
            return;
        }

        SlabHeader *slab = (SlabHeader *)(virt_addr & ~0xFFF);
        if (slab->magic != SLAB_MAGIC || slab->cache_index >= CACHE_COUNT) kpanic("heap: bad slab free");
        SlabCache *cache = &caches[slab->cache_index];

        uint64_t flags;
        cache->lock.acquire(flags);

        if (slab->used_slots == 0) kpanic("heap: slab double free");
        bool was_full = (slab->used_slots == slab->total_slots);
        *(void **)ptr = slab->free_list;
        slab->free_list = ptr;
        slab->used_slots--;

        if (slab->used_slots == 0) {
            if (slab->owner == nullptr) {
                list_remove(&cache->partial_slabs, slab);
                slab->magic = 0;
                sub_stat(&slab_page_bytes, pmm::PAGE_SIZE);
                pmm::free(v2p(slab), pmm::PAGE_SIZE);
            }
        } else if (was_full) {
            list_remove(&cache->full_slabs, slab);
            list_push(&cache->partial_slabs, slab);
        }

        cache->lock.release(flags);
    }

    void *krealloc(void *ptr, size_t new_size) {
        if (!ptr) return kmalloc(new_size);
        if (new_size == 0) {
            kfree(ptr);
            return nullptr;
        }

        size_t old_size;
        uint64_t virt_addr = (uint64_t)ptr;

        if ((virt_addr & 0xFFF) == 0) {
            LargeAllocHeader *header = reinterpret_cast<LargeAllocHeader *>(virt_addr - pmm::PAGE_SIZE);
            if (header->magic != LARGE_MAGIC) return nullptr;
            old_size = header->requested_size;
        } else {
            SlabHeader *slab = (SlabHeader *)(virt_addr & ~0xFFF);
            if (slab->magic != SLAB_MAGIC || slab->cache_index >= CACHE_COUNT) return nullptr;
            old_size = slab->slot_size;
        }

        if (new_size <= old_size) return ptr;

        void *new_ptr = kmalloc(new_size);
        if (new_ptr) {
            memcpy(new_ptr, ptr, old_size);
            kfree(ptr);
        }
        return new_ptr;
    }

    void *kreallocarray(void *ptr, size_t count, size_t size) {
        size_t bytes = 0;
        if (mul_overflow(count, size, &bytes)) return nullptr;
        return krealloc(ptr, bytes);
    }

    void get_debug_info(DebugInfo *info) {
        if (!info) return;
        uint64_t flags;
        heap_stat_lock.acquire(flags);
        info->slab_page_bytes = slab_page_bytes;
        info->large_alloc_bytes = large_alloc_bytes;
        info->large_alloc_count = large_alloc_count;
        heap_stat_lock.release(flags);
    }

}  // namespace heap

void *operator new(size_t size) { return heap::kmalloc(size); }
void *operator new[](size_t size) { return heap::kmalloc(size); }
void operator delete(void *ptr) noexcept { heap::kfree(ptr); }
void operator delete[](void *ptr) noexcept { heap::kfree(ptr); }
void operator delete(void *ptr, size_t size) noexcept {
    (void)size;
    heap::kfree(ptr);
}
void operator delete[](void *ptr, size_t size) noexcept {
    (void)size;
    heap::kfree(ptr);
}