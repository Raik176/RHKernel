#include <stddef.h>
#include <stdlib.h>

extern "C" void abort(void);

void *operator new(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) abort();
    return p;
}

void *operator new[](size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) abort();
    return p;
}

void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
