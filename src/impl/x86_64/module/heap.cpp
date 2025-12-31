#include "mod/heap.h"

#include "memory/heap.h"
#include "symbol.h"

void *kmalloc(size_t size) { return heap::kmalloc(size); }

void kfree(void *ptr) { return heap::kfree(ptr); }

void *krealloc(void *ptr, size_t new_size) { return heap::krealloc(ptr, new_size); }

KEXPORT(kmalloc);
KEXPORT(kfree);
KEXPORT(krealloc)