#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *kmalloc(size_t size);
void kfree(void *ptr);
void *krealloc(void *ptr, size_t new_size);

#ifdef __cplusplus
}
#endif
