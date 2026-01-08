#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Convert a physical address to a virtual address
 * @param phys The physical address
 * @return The corresponding virtual address
 */
void *p2v(uint64_t phys);

/**
 * @brief Convert a virtual address to a physical address
 * @param virt The virtual address
 * @return The corresponding physical address
 */
uint64_t v2p(void *virt);

#ifdef __cplusplus
}
#endif