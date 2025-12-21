/**
 * @file heap.cpp
 * @brief Implementation of kernel slab allocator
 *
 * Provides memory allocation for the kernel using a slab allocator.
 * Supports kmalloc/kfree and C++ new/delete operators.
 */

#include "heap.h"

#include "pmm.h"
#include "util.h"
#include "vmm.h"

namespace heap {

    /** @internal Represents a single slab (page) in the allocator */
    struct SlabHeader {
        uint32_t slot_size;       ///< Size of each allocation slot
        uint32_t used_slots;      ///< Number of slots currently in use
        uint32_t total_slots;     ///< Total number of slots in the slab
        void* free_list;          ///< Linked list of free slots
        SlabHeader *next, *prev;  ///< Links for partial/full slab lists
    };

    /** @internal Cache for slabs of a specific allocation size */
    struct SlabCache {
        size_t slot_size;           ///< Size of each allocation slot
        SlabHeader* partial_slabs;  ///< Slabs with some free slots
        SlabHeader* full_slabs;     ///< Slabs completely used
    };

    /** @internal Predefined slab caches for common kernel allocation sizes */
    static SlabCache caches[] = {{16, nullptr, nullptr},   {32, nullptr, nullptr},
                                 {64, nullptr, nullptr},   {128, nullptr, nullptr},
                                 {256, nullptr, nullptr},  {512, nullptr, nullptr},
                                 {1024, nullptr, nullptr}, {2048, nullptr, nullptr}};

    static const size_t CACHE_COUNT = sizeof(caches) / sizeof(SlabCache);

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
     * @brief Initialize the kernel heap
     *
     * Currently, this only sets up internal slab structures.
     * In a more complex system, mutexes and bookkeeping would also be initialized.
     */
    void init() {}

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

    /**
     * @brief Allocate memory from the kernel heap
     *
     * Finds a suitable slab cache for the requested size and returns a free slot.
     * If no suitable slab exists, a new slab is created.
     *
     * @param size Number of bytes requested
     * @return Pointer to allocated memory, or nullptr if allocation fails
     */
    void* kmalloc(size_t size) {
        SlabCache* cache = nullptr;
        for (size_t i = 0; i < CACHE_COUNT; i++) {
            if (size <= caches[i].slot_size) {
                cache = &caches[i];
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

        if (!cache->partial_slabs) {
            SlabHeader* new_slab = create_slab(cache->slot_size);
            if (!new_slab) return nullptr;
            new_slab->slot_size = cache->slot_size;
            list_push(&cache->partial_slabs, new_slab);
        }

        SlabHeader* slab = cache->partial_slabs;
        void* ptr = slab->free_list;
        slab->free_list = *(void**)ptr;
        slab->used_slots++;

        if (slab->used_slots == slab->total_slots) {
            list_remove(&cache->partial_slabs, slab);
            list_push(&cache->full_slabs, slab);
        }

        return ptr;
    }

    /**
     * @brief Free previously allocated memory
     *
     * Returns memory to the appropriate slab or frees large allocations back to PMM.
     *
     * @param ptr Pointer to memory previously allocated with kmalloc
     */
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

        SlabCache* cache = nullptr;
        for (size_t i = 0; i < CACHE_COUNT; i++) {
            if (caches[i].slot_size == slab->slot_size) {
                cache = &caches[i];
                break;
            }
        }

        bool was_full = (slab->used_slots == slab->total_slots);

        *(void**)ptr = slab->free_list;
        slab->free_list = ptr;
        slab->used_slots--;

        if (slab->used_slots == 0) {
            list_remove(&cache->partial_slabs, slab);
            pmm::free(v2p(slab), pmm::PAGE_SIZE);
            return;
        }

        if (was_full) {
            list_remove(&cache->full_slabs, slab);
            list_push(&cache->partial_slabs, slab);
        }
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