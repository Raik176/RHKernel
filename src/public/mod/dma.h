#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static const uint64_t DMA_MASK_24 = UINT64_C(0x00FFFFFF);
static const uint64_t DMA_MASK_32 = UINT64_C(0xFFFFFFFF);
static const uint64_t DMA_MASK_64 = UINT64_C(0xFFFFFFFFFFFFFFFF);

typedef struct dma_allocation {
    void *virt;
    uint64_t phys;
    size_t size;
    size_t allocated_size;
} dma_allocation_t;

int dma_alloc_coherent(size_t size, size_t align, uint64_t dma_mask, dma_allocation_t *out);
void dma_free_coherent(dma_allocation_t *allocation);

#ifdef __cplusplus
}
#endif
