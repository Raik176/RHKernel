#pragma once
#include <stddef.h>
#include <stdint.h>

namespace dma {

    constexpr uint64_t DMA_MASK_24 = 0x00FFFFFFULL;
    constexpr uint64_t DMA_MASK_32 = 0xFFFFFFFFULL;
    constexpr uint64_t DMA_MASK_64 = 0xFFFFFFFFFFFFFFFFULL;

    struct Allocation {
        void *virt;
        uint64_t phys;
        size_t size;
        size_t allocated_size;
    };

    Allocation alloc_coherent(size_t size, size_t align, uint64_t dma_mask, bool zero = true);
    void free_coherent(Allocation *allocation);

}  // namespace dma
