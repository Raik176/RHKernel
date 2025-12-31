#pragma once
#include <stddef.h>
#include <stdint.h>

#include "mod/module.h"

namespace module_loader {
    struct loaded_module {
        char *name;
        uintptr_t base;
        size_t size;
        module_exit_t exit_func;
        loaded_module *next;
    };

    void init();

    void load_module(const char *path);
    bool load_by_name(const char *name);

    void list_modules();
    bool is_module_loaded(const char *name);
}  // namespace module_loader