#pragma once
#include <stddef.h>
#include <stdint.h>

namespace heap {

/**
 * @brief Initialize the kernel heap
 *
 * Sets up the slab allocator structures and prepares memory
 * for dynamic allocation using kmalloc/kfree.
 */
void init();

/**
 * @brief Allocate memory from the kernel heap
 *
 * Allocates a memory block of at least `size` bytes using the slab allocator.
 *
 * @param size Number of bytes to allocate
 * @return Pointer to the allocated memory, or nullptr if allocation fails
 */
void* kmalloc(size_t size);

/**
 * @brief Free previously allocated memory
 *
 * Returns the memory block pointed to by `ptr` back to the heap.
 *
 * @param ptr Pointer to memory previously allocated with kmalloc
 */
void kfree(void* ptr);

} // namespace heap

/**
 * @brief C++ new operator using the kernel heap
 *
 * Allocates memory using the kernel heap (kmalloc).
 *
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory
 */
void* operator new(size_t size);

/**
 * @brief C++ array new operator using the kernel heap
 *
 * Allocates memory for an array using the kernel heap (kmalloc).
 *
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory
 */
void* operator new[](size_t size);

/**
 * @brief C++ delete operator using the kernel heap
 *
 * Frees memory previously allocated with operator new.
 *
 * @param ptr Pointer to memory to free
 */
void operator delete(void* ptr) noexcept;

/**
 * @brief C++ array delete operator using the kernel heap
 *
 * Frees memory previously allocated with operator new[].
 *
 * @param ptr Pointer to memory to free
 */
void operator delete[](void* ptr) noexcept;

/**
 * @brief C++ sized delete operator
 *
 * Frees memory of a known size previously allocated with operator new.
 *
 * @param ptr Pointer to memory to free
 * @param size Size of the memory block
 */
void operator delete(void* ptr, size_t size) noexcept;

/**
 * @brief C++ sized array delete operator
 *
 * Frees memory of a known size previously allocated with operator new[].
 *
 * @param ptr Pointer to memory to free
 * @param size Size of the memory block
 */
void operator delete[](void* ptr, size_t size) noexcept;