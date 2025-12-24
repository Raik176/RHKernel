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
#include "util.h"

namespace heap {
    /**
     * @internal Remove a slab from a linked list
     *
     * @param head Pointer to the head of the list
     * @param slab Slab to remove
     */
    static void list_remove(SlabHeader** head, SlabHeader* slab) {
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
    static void list_push(SlabHeader** head, SlabHeader* slab) {
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
    static SlabHeader* create_slab(size_t slot_size) {
        uint64_t phys = pmm::alloc(pmm::PAGE_SIZE);
        if (!phys) return nullptr;

        SlabHeader* slab = (SlabHeader*)p2v(phys);

        slab->next = nullptr;
        slab->used_slots = 0;

        size_t header_size = align_up(sizeof(SlabHeader), 16);
        size_t available_space = pmm::PAGE_SIZE - header_size;
        slab->total_slots = available_space / slot_size;

        uint8_t* first_slot = (uint8_t*)slab + header_size;
        slab->free_list = (void*)first_slot;

        for (size_t i = 0; i < slab->total_slots - 1; i++) {
            void** current = (void**)(first_slot + (i * slot_size));
            *current = (void*)(first_slot + ((i + 1) * slot_size));
        }

        void** last = (void**)(first_slot + ((slab->total_slots - 1) * slot_size));
        *last = nullptr;

        return slab;
    }

    void* kmalloc(size_t size) {
        size_t cache_idx = 0xFFFFFFFF;
        SlabCache* cache = nullptr;

        for (size_t i = 0; i < CACHE_COUNT; i++) {
            if (size <= caches[i].slot_size) {
                cache = &caches[i];
                cache_idx = i;
                break;
            }
        }

        if (!cache) {
            size_t order = pmm::size_to_order(size + pmm::PAGE_SIZE);
            size_t actual_size = (1ULL << order) * pmm::PAGE_SIZE;
            uint64_t phys = pmm::alloc(actual_size);
            if (!phys) return nullptr;
            uint64_t virt = reinterpret_cast<uint64_t>(p2v(phys));
            *(size_t*)virt = actual_size;
            return reinterpret_cast<void*>(virt + pmm::PAGE_SIZE);
        }

        smp::cpu_local* cpu = smp::get_cpu();
        if (cpu && cpu->self == cpu) {
            SlabHeader* local = cpu->heap_cache[cache_idx];

            if (local && local->free_list) {
                void* ptr = local->free_list;
                local->free_list = *(void**)ptr;
                local->used_slots++;

                if (local->used_slots == local->total_slots) {
                    cache->lock.acquire();
                    local->owner = nullptr;
                    list_push(&cache->full_slabs, local);
                    cache->lock.release();
                    cpu->heap_cache[cache_idx] = nullptr;
                }
                return ptr;
            }
        }

        // --- STAGE 1: GLOBAL CACHE (Fallback) ---
        cache->lock.acquire();

        if (!cache->partial_slabs) {
            SlabHeader* new_slab = create_slab(cache->slot_size);
            if (!new_slab) {
                cache->lock.release();
                return nullptr;
            }
            new_slab->cache_index = cache_idx;  // Store index for kfree
            new_slab->slot_size = cache->slot_size;
            list_push(&cache->partial_slabs, new_slab);
        }

        SlabHeader* slab = cache->partial_slabs;

        // If SMP is active, "Steal" the global partial slab for this CPU
        if (cpu && cpu->self == cpu) {
            list_remove(&cache->partial_slabs, slab);
            slab->owner = cpu;
            cpu->heap_cache[cache_idx] = slab;
            cache->lock.release();

            // Now that the CPU has a local slab, recurse once to use Fast Path
            return kmalloc(size);
        }

        // Standard Global Allocation (BSP Boot Stage)
        void* ptr = slab->free_list;
        slab->free_list = *(void**)ptr;
        slab->used_slots++;

        if (slab->used_slots == slab->total_slots) {
            list_remove(&cache->partial_slabs, slab);
            list_push(&cache->full_slabs, slab);
        }

        cache->lock.release();
        return ptr;
    }

    void kfree(void* ptr) {
        if (!ptr) return;
        uint64_t virt_addr = (uint64_t)ptr;

        if ((virt_addr & 0xFFF) == 0) {
            uint64_t metadata_page = virt_addr - pmm::PAGE_SIZE;
            size_t total_size = *(size_t*)metadata_page;
            pmm::free(v2p(reinterpret_cast<void*>(metadata_page)), total_size);
            return;
        }

        SlabHeader* slab = (SlabHeader*)(virt_addr & ~0xFFF);
        SlabCache* cache = &caches[slab->cache_index];

        cache->lock.acquire();

        bool was_full = (slab->used_slots == slab->total_slots);
        *(void**)ptr = slab->free_list;
        slab->free_list = ptr;
        slab->used_slots--;

        if (slab->used_slots == 0) {
            if (slab->owner == nullptr) {
                list_remove(&cache->partial_slabs, slab);
                pmm::free(v2p(slab), pmm::PAGE_SIZE);
            }
        } else if (was_full) {
            list_remove(&cache->full_slabs, slab);
            list_push(&cache->partial_slabs, slab);
        }

        cache->lock.release();
    }

}  // namespace heap

void* operator new(size_t size) { return heap::kmalloc(size); }
void* operator new[](size_t size) { return heap::kmalloc(size); }
void operator delete(void* ptr) noexcept { heap::kfree(ptr); }
void operator delete[](void* ptr) noexcept { heap::kfree(ptr); }
void operator delete(void* ptr, size_t size) noexcept {
    (void)size;
    heap::kfree(ptr);
}
void operator delete[](void* ptr, size_t size) noexcept {
    (void)size;
    heap::kfree(ptr);
}