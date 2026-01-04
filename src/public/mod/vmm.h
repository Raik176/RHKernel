#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *vmm_mmio_map(uint64_t phys_addr, uint64_t size);
void vmm_mmio_unmap(void *virt_addr, uint64_t size);

#ifdef __cplusplus
}
#endif
