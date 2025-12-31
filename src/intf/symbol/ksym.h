#pragma once
#include <stdint.h>

#include "symbol.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t _ksymtab_start[];
extern const uint8_t _ksymtab_end[];

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace ksym {
    const char *get_name(uintptr_t addr, uintptr_t *offset = nullptr);
    uintptr_t resolve_symbol(const char *name);
    bool export_symbol(const char *name, uintptr_t addr);
}  // namespace ksym
#endif
