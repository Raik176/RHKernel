#include "mod/vmm.h"

#include "memory/vmm.h"
#include "symbol.h"

void *vmm_mmio_map(uint64_t phys_addr, uint64_t size) { return vmm::mmio_map(phys_addr, size); }

void vmm_mmio_unmap(void *virt_addr, uint64_t size) { return vmm::mmio_unmap(virt_addr, size); }

KEXPORT(vmm_mmio_map)
KEXPORT(vmm_mmio_unmap)