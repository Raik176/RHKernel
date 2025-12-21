#pragma once
#include <stddef.h>
#include <stdint.h>

namespace heap {

void init();

void* kmalloc(size_t size);
void kfree(void* ptr);

} // namespace heap

void* operator new(size_t size);
void* operator new[](size_t size);
void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, size_t size) noexcept;
void operator delete[](void* ptr, size_t size) noexcept;