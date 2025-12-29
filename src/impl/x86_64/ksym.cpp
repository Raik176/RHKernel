#include "ksym.h"

namespace ksym {
    const char *get_name(uintptr_t addr, uintptr_t *offset) {
        const kernel_symbol *best_match = nullptr;

        for (const kernel_symbol *s = _ksymtab_start; s < _ksymtab_end; s++) {
            if (s->addr <= addr) {
                if (!best_match || s->addr > best_match->addr) { best_match = s; }
            }
        }

        if (best_match) {
            if (offset) *offset = addr - best_match->addr;
            return best_match->name;
        }
        return "unknown";
    }
}  // namespace ksym