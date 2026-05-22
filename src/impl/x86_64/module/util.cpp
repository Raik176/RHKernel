#include "mod/util.h"
#include "symbol.h"

#define PHYS_MAP_BASE 0xFFFF800000000000ULL

extern "C" void *p2v(uint64_t phys) { return (void *)(phys + PHYS_MAP_BASE); }

extern "C" uint64_t v2p(void *virt) { return (uint64_t)virt - PHYS_MAP_BASE; }

KEXPORT(p2v)
KEXPORT(v2p)
