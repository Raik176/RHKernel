#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mod/acpi.h"

namespace acpi {
    void init(uint64_t mb_phys_addr);
    SDTHeader *find_table(const char *signature);

}  // namespace acpi