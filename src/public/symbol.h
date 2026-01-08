#pragma once

#include <stdint.h>

#ifdef __cplusplus
#define EXTERN_C_LINKAGE extern "C"
#else
#define EXTERN_C_LINKAGE extern
#endif

struct kernel_symbol {
    uintptr_t addr;
    const char *name;
} __attribute__((packed));

#define KEXPORT(sym)                           \
    EXTERN_C_LINKAGE __typeof__(sym) sym;      \
    __attribute__((section(".ksymtab"), used)) \
    const struct kernel_symbol __ksym_##sym = {.addr = (uintptr_t)&sym, .name = #sym};
