#pragma once
#include <stdint.h>
#include <stddef.h>

namespace pmm {

constexpr size_t PAGE_SIZE = 4096;
constexpr size_t MAX_ORDER = 11; // 2^11 pages = 8 MiB blocks

/**
 * @brief Initialize the physical memory manager
 */
void init(uint64_t mb_phys_addr);

size_t size_to_order(size_t size);

/**
 * @brief Allocate physical memory
 * @param size Number of bytes requested
 * @return Physical address of allocation, or 0 on failure
 */
uint64_t alloc(size_t size);

/**
 * @brief Free physical memory
 * @param phys Physical address
 * @param size Size originally allocated
 */
void free(uint64_t phys, size_t size);

size_t get_total_kb();
size_t get_free_kb();
size_t get_system_kb(); // total amount of ram, usable or not.

}