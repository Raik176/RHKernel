#pragma once
#include <stddef.h>
#include <stdint.h>

#include "file/vfs.h"
#include "mod/module.h"
#include "symbol.h"

namespace module_loader {
    static constexpr uint64_t MODULE_BASE = 0xFFFFFFFFA0000000ULL;
    static constexpr uint64_t MODULE_SIZE = 0x20000000ULL;  // 512 MiB

    struct ModuleSection {
        const char *name;
        uintptr_t start;
        uintptr_t end;
        uint64_t flags;
    };

    struct LoadedModule {
        const char *name;
        const char *path;
        uintptr_t base;
        uintptr_t end;
        uintptr_t exec_start;
        uintptr_t exec_end;
        uint64_t image_size;
        uint64_t mapped_size;
        uint64_t file_size;
        uint64_t section_count;
        uint64_t symbol_count;
        ModuleSection *sections;
        kernel_symbol *symbols;
        LoadedModule *next;
    };

    void init();

    void load_module(const char *path);
    bool load_by_name(const char *name);

    void list_modules();
    bool is_module_loaded(const char *name);
    const LoadedModule *first_module();
    const LoadedModule *find_module(const char *name);
    bool address_in_kernel(uintptr_t addr);
    bool address_in_kernel_text(uintptr_t addr);
    bool address_in_kernel_or_module(uintptr_t addr, uint64_t size = 1);
    const LoadedModule *find_module_containing(uintptr_t addr, uint64_t size = 1);
    bool address_in_module_text(const LoadedModule *m, uintptr_t addr);
    uint64_t kernel_image_size();
    uint64_t module_total_mapped_size();
}  // namespace module_loader
