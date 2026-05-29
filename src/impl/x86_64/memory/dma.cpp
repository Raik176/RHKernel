#include "memory/dma.h"

#include "memory/pmm.h"
#include "util.h"

namespace {
    static bool is_power_of_two(uint64_t value) { return value != 0 && (value & (value - 1)) == 0; }
}

namespace dma {

    Allocation alloc_coherent(size_t size, size_t align, uint64_t dma_mask, bool zero) {
        Allocation allocation = {};
        if (size == 0) return allocation;
        if (align == 0) align = pmm::PAGE_SIZE;
        if (!is_power_of_two(align)) return allocation;
        if (align < pmm::PAGE_SIZE) align = pmm::PAGE_SIZE;

        uint64_t max_exclusive = dma_mask == UINT64_MAX ? UINT64_MAX : dma_mask + 1;
        if (max_exclusive == 0) return allocation;

        pmm::AllocConstraints constraints = {
            .min_phys = 0,
            .max_phys_exclusive = max_exclusive,
            .align = align,
            .zero = zero,
        };

        size_t allocated_size = 0;
        uint64_t phys = pmm::alloc_constrained(size, constraints, &allocated_size);
        if (!phys) return allocation;

        allocation.virt = p2v(phys);
        allocation.phys = phys;
        allocation.size = size;
        allocation.allocated_size = allocated_size;
        return allocation;
    }

    void free_coherent(Allocation *allocation) {
        if (!allocation || !allocation->phys) return;
        pmm::free(allocation->phys, allocation->allocated_size);
        *allocation = {};
    }

}  // namespace dma
