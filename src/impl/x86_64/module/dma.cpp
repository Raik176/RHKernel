#include "mod/dma.h"

#include "memory/dma.h"
#include "symbol.h"

int dma_alloc_coherent(size_t size, size_t align, uint64_t dma_mask, dma_allocation_t *out) {
    if (!out) return -1;
    *out = {};

    dma::Allocation allocation = dma::alloc_coherent(size, align, dma_mask);
    if (!allocation.phys) return -1;

    out->virt = allocation.virt;
    out->phys = allocation.phys;
    out->size = allocation.size;
    out->allocated_size = allocation.allocated_size;
    return 0;
}

void dma_free_coherent(dma_allocation_t *allocation) {
    if (!allocation) return;

    dma::Allocation kernel_allocation = {
        .virt = allocation->virt,
        .phys = allocation->phys,
        .size = allocation->size,
        .allocated_size = allocation->allocated_size,
    };
    dma::free_coherent(&kernel_allocation);
    *allocation = {};
}

KEXPORT(dma_alloc_coherent)
KEXPORT(dma_free_coherent)
