#include "mod/acpi.h"

#include "acpi.h"
#include "symbol.h"

SDTHeader *acpi_find_table(const char *signature) { return acpi::find_table(signature); }

KEXPORT(acpi_find_table)