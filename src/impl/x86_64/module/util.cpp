#include "mod/util.h"
#include "file/module_loader.h"
#include "symbol.h"

/**
 * @brief Convert a physical address to a virtual address
 * @param phys The physical address
 * @return The corresponding virtual address
 */
inline void *p2v(uint64_t phys) { return (void *)(phys + module_loader::MODULE_BASE); }

/**
 * @brief Convert a virtual address to a physical address
 * @param virt The virtual address
 * @return The corresponding physical address
 */
inline uint64_t v2p(void *virt) { return (uint64_t)virt - module_loader::MODULE_BASE; }

KEXPORT(p2v);
KEXPORT(v2p);