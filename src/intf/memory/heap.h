#pragma once
#include <stddef.h>
#include <stdint.h>

#include "smp/lock.h"

namespace heap {
    /** @internal Represents a single slab (page) in the allocator */
    struct SlabHeader {
        uint32_t slot_size;  ///< Size of each allocation slot
        uint32_t cache_index;
        uint32_t used_slots;      ///< Number of slots currently in use
        uint32_t total_slots;     ///< Total number of slots in the slab
        void *free_list;          ///< Linked list of free slots
        SlabHeader *next, *prev;  ///< Links for partial/full slab lists

        void *owner;
    };

    /** @internal Cache for slabs of a specific allocation size */
    struct SlabCache {
        size_t slot_size;  ///< Size of each allocation slot
        spinlock_t lock;
        SlabHeader *partial_slabs;  ///< Slabs with some free slots
        SlabHeader *full_slabs;     ///< Slabs completely used
    };

    /** @internal Predefined slab caches for common kernel allocation sizes */
    static SlabCache caches[] = {{16, {}, nullptr, nullptr},   {32, {}, nullptr, nullptr},
                                 {64, {}, nullptr, nullptr},   {128, {}, nullptr, nullptr},
                                 {256, {}, nullptr, nullptr},  {512, {}, nullptr, nullptr},
                                 {1024, {}, nullptr, nullptr}, {2048, {}, nullptr, nullptr}};

    static const size_t CACHE_COUNT = sizeof(caches) / sizeof(SlabCache);

    static_assert(CACHE_COUNT > 0, "At least one slab cache required");

    /**
     * @brief Allocate memory from the kernel heap
     *
     * Allocates a memory block of at least `size` bytes using the slab allocator.
     *
     * @param size Number of bytes to allocate
     * @return Pointer to the allocated memory, or nullptr if allocation fails
     */
    void *kmalloc(size_t size);

    /**
     * @brief Free previously allocated memory
     *
     * Returns the memory block pointed to by `ptr` back to the heap.
     *
     * @param ptr Pointer to memory previously allocated with kmalloc
     */
    void kfree(void *ptr);

    void *krealloc(void *ptr, size_t new_size);

}  // namespace heap

/**
 * @brief C++ new operator using the kernel heap
 *
 * Allocates memory using the kernel heap (kmalloc).
 *
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory
 */
void *operator new(size_t size);

/**
 * @brief C++ array new operator using the kernel heap
 *
 * Allocates memory for an array using the kernel heap (kmalloc).
 *
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory
 */
void *operator new[](size_t size);

/**
 * @brief C++ delete operator using the kernel heap
 *
 * Frees memory previously allocated with operator new.
 *
 * @param ptr Pointer to memory to free
 */
void operator delete(void *ptr) noexcept;

/**
 * @brief C++ array delete operator using the kernel heap
 *
 * Frees memory previously allocated with operator new[].
 *
 * @param ptr Pointer to memory to free
 */
void operator delete[](void *ptr) noexcept;

/**
 * @brief C++ sized delete operator
 *
 * Frees memory of a known size previously allocated with operator new.
 *
 * @param ptr Pointer to memory to free
 * @param size Size of the memory block
 */
void operator delete(void *ptr, size_t size) noexcept;

/**
 * @brief C++ sized array delete operator
 *
 * Frees memory of a known size previously allocated with operator new[].
 *
 * @param ptr Pointer to memory to free
 * @param size Size of the memory block
 */
void operator delete[](void *ptr, size_t size) noexcept;