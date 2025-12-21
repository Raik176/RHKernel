#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "util.h"

namespace heap {

struct SlabHeader {
    uint32_t slot_size;
    uint32_t used_slots;
    uint32_t total_slots;
    void* free_list;
    SlabHeader *next, *prev;
};

struct SlabCache {
    size_t slot_size;
    SlabHeader* partial_slabs; // Slabs that have some free slots
    SlabHeader* full_slabs;    // Slabs with no free slots
};

// Common allocation sizes for a kernel
static SlabCache caches[] = {
    {16, nullptr, nullptr},
    {32, nullptr, nullptr},
    {64, nullptr, nullptr},
    {128, nullptr, nullptr},
    {256, nullptr, nullptr},
    {512, nullptr, nullptr},
    {1024, nullptr, nullptr},
    {2048, nullptr, nullptr}
};

static const size_t CACHE_COUNT = sizeof(caches) / sizeof(SlabCache);

static void list_remove(SlabHeader** head, SlabHeader* slab) {
    if (slab->prev) slab->prev->next = slab->next;
    if (slab->next) slab->next->prev = slab->prev;
    if (*head == slab) *head = slab->next;
    slab->next = slab->prev = nullptr;
}

// Helper to push a slab to the front of a list
static void list_push(SlabHeader** head, SlabHeader* slab) {
    slab->next = *head;
    if (*head) (*head)->prev = slab;
    *head = slab;
    slab->prev = nullptr;
}

void init() {
    // In a more complex version, you'd initialize mutexes here
}

static SlabHeader* create_slab(size_t slot_size) {
    // 1. Get a physical page
    uint64_t phys = pmm::alloc(pmm::PAGE_SIZE);
    if (!phys) return nullptr;

    // 2. We assume the VMM has a direct mapping (Higher Half)
    // If not, you must map this phys page to a virtual address first
    SlabHeader* slab = (SlabHeader*)p2v(phys);

    // 3. Setup the header
    slab->next = nullptr;
    slab->used_slots = 0;
    
    // We store the header at the start, slots follow
    size_t header_size = align_up(sizeof(SlabHeader), 16);
    size_t available_space = pmm::PAGE_SIZE - header_size;
    slab->total_slots = available_space / slot_size;

    // 4. Build the internal linked list of free slots
    uint8_t* first_slot = (uint8_t*)slab + header_size;
    slab->free_list = (void*)first_slot;

    for (size_t i = 0; i < slab->total_slots - 1; i++) {
        void** current = (void**)(first_slot + (i * slot_size));
        *current = (void*)(first_slot + ((i + 1) * slot_size));
    }
    
    // Last slot points to null
    void** last = (void**)(first_slot + ((slab->total_slots - 1) * slot_size));
    *last = nullptr;

    return slab;
}

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
        size_t actual_size = (1ULL << order) * pmm::PAGE_SIZE; // The real size allocated
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

    // If slab is now full, move it to full_slabs
    if (slab->used_slots == slab->total_slots) {
        list_remove(&cache->partial_slabs, slab);
        list_push(&cache->full_slabs, slab);
    }

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
    
    SlabCache* cache = nullptr;
    for (size_t i = 0; i < CACHE_COUNT; i++) {
        if (caches[i].slot_size == slab->slot_size) {
            cache = &caches[i];
            break;
        }
    }

    bool was_full = (slab->used_slots == slab->total_slots);

    // 2. Return the slot to the free list
    *(void**)ptr = slab->free_list;
    slab->free_list = ptr;
    slab->used_slots--;

    // 3. Logic: Returning empty slabs to PMM
    if (slab->used_slots == 0) {
        // Remove from partial list and give page back to physical memory
        list_remove(&cache->partial_slabs, slab);
        pmm::free(v2p(slab), pmm::PAGE_SIZE);
        return;
    }

    // 4. Logic: Move from Full to Partial
    if (was_full) {
        list_remove(&cache->full_slabs, slab);
        list_push(&cache->partial_slabs, slab);
    }
}

} // namespace heap

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