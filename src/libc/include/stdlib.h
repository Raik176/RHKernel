#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void *aligned_alloc(size_t alignment, size_t size);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
int atoi(const char *nptr);
long atol(const char *nptr);
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

#ifdef __cplusplus
}
#endif

#endif
