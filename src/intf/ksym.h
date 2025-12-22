#include <stdint.h>

struct kernel_symbol {
    uintptr_t addr;
    const char* name;
};

#define KEXPORT(sym) \
    extern "C" void* sym; \
    __attribute__((section(".ksymtab"), used)) \
    const kernel_symbol __ksym_##sym = { (uintptr_t)&sym, #sym }

namespace ksym {
    extern "C" const kernel_symbol _ksymtab_start[];
    extern "C" const kernel_symbol _ksymtab_end[];

    const char* get_name(uintptr_t addr, uintptr_t* offset = nullptr);
}