#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mod/module.h"

namespace module_loader {
    static constexpr uint64_t MODULE_BASE = 0xFFFFFFFFA0000000ULL;
    static constexpr uint64_t MODULE_SIZE = 0x20000000ULL;  // 512MB space for modules

    void init();

    void load_module(const char *path);
    bool load_by_name(const char *name);

    void list_modules();
    bool is_module_loaded(const char *name);
}  // namespace module_loader