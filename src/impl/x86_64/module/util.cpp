#include "mod/util.h"
#include "symbol.h"

extern "C" uint64_t paging_phys_map_base_value();

extern "C" void *p2v(uint64_t phys) { return (void *)(phys + paging_phys_map_base_value()); }

extern "C" uint64_t v2p(void *virt) { return (uint64_t)virt - paging_phys_map_base_value(); }

KEXPORT(p2v)
KEXPORT(v2p)
