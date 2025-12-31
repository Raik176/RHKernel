#include "mod/logging.h"

#include <stdarg.h>

#include "console.h"
#include "symbol/ksym.h"

void klog(log_level lvl, const char *fmt, ...) {
    (void)lvl;

    va_list args;
    va_start(args, fmt);
    console::vprintf(fmt, args);
    va_end(args);
}

KEXPORT(klog)