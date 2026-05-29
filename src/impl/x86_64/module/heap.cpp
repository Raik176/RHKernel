#include "mod/heap.h"

#include "memory/heap.h"
#include "symbol.h"

void *kmalloc(size_t size) { return heap::kmalloc(size); }

void *kzalloc(size_t size) { return heap::kzalloc(size); }

void *kmalloc_array(size_t count, size_t size) { return heap::kmalloc_array(count, size); }

void *kcalloc(size_t count, size_t size) { return heap::kcalloc(count, size); }

void kfree(void *ptr) { return heap::kfree(ptr); }

void *krealloc(void *ptr, size_t new_size) { return heap::krealloc(ptr, new_size); }

void *kreallocarray(void *ptr, size_t count, size_t size) {
    return heap::kreallocarray(ptr, count, size);
}

KEXPORT(kmalloc);
KEXPORT(kzalloc);
KEXPORT(kmalloc_array);
KEXPORT(kcalloc);
KEXPORT(kfree);
KEXPORT(krealloc);
KEXPORT(kreallocarray)