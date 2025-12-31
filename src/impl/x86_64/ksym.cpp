#include "symbol/ksym.h"

#include "console.h"
#include "memory/heap.h"
#include "string.h"

namespace ksym {
    struct dynamic_symbol {
        uintptr_t addr;
        char *name;
        dynamic_symbol *next;
    };

    static dynamic_symbol *dynamic_table = nullptr;

    bool export_symbol(const char *name, uintptr_t addr) {
        if (resolve_symbol(name) != 0) { return false; }

        dynamic_symbol *sym = (dynamic_symbol *)heap::kmalloc(sizeof(dynamic_symbol));
        sym->addr = addr;
        sym->name = strdup(name);
        sym->next = dynamic_table;
        dynamic_table = sym;

        return true;
    }

    uintptr_t resolve_symbol(const char *name) {
        for (const kernel_symbol *s = (kernel_symbol *)_ksymtab_start;
             s < (kernel_symbol *)_ksymtab_end; ++s) {
            if (strcmp(s->name, name) == 0) { return s->addr; }
        }

        for (dynamic_symbol *ds = dynamic_table; ds != nullptr; ds = ds->next) {
            if (strcmp(ds->name, name) == 0) { return ds->addr; }
        }

        return 0;
    }

    const char *get_name(uintptr_t addr, uintptr_t *offset) {
        const char *best_name = nullptr;
        uintptr_t best_addr = 0;

        for (const kernel_symbol *s = (kernel_symbol *)_ksymtab_start;
             s < (kernel_symbol *)_ksymtab_end; ++s) {
            if (s->addr <= addr && s->addr >= best_addr) {
                best_addr = s->addr;
                best_name = s->name;
            }
        }

        for (dynamic_symbol *ds = dynamic_table; ds != nullptr; ds = ds->next) {
            if (ds->addr <= addr && ds->addr >= best_addr) {
                best_addr = ds->addr;
                best_name = ds->name;
            }
        }

        if (best_name) {
            if (offset) *offset = addr - best_addr;
            return best_name;
        }

        return nullptr;
    }

}  // namespace ksym
