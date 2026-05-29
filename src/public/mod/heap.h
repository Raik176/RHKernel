#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kmalloc_array(size_t count, size_t size);
void *kcalloc(size_t count, size_t size);
void kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);
void *kreallocarray(void *ptr, size_t count, size_t size);

#ifdef __cplusplus
}
#endif
