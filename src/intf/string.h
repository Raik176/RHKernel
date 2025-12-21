#pragma once
#include <stddef.h>
#include <stdint.h>


static inline void *memcpy(void *dest, const void *src, size_t n) {
    void *ret = dest;

    asm volatile (
        "cld\n\t"
        "rep movsb"
        : "+D"(dest), "+S"(src), "+c"(n)
        :
        : "memory"
    );

    return ret;
}

static inline void *memset(void *s, int c, size_t n) {
    void *ret = s;

    asm volatile (
        "cld\n\t"
        "rep stosb"
        : "+D"(s), "+c"(n)
        : "a"((uint8_t)c)
        : "memory"
    );

    return ret;
}

inline void memzero(void* ptr, size_t size) {
    asm volatile (
        "cld\n\t"
        "xor %%al, %%al\n\t"
        "rep stosb"
        : "+D"(ptr), "+c"(size)
        :
        : "al", "memory"
    );
}

static inline int memcmp(const void *s1, const void *s2, size_t n) {
    if (n == 0) return 0;

    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    uint64_t res;
    asm volatile (
        "cld\n\t"
        "repe cmpsb\n\t"
        "setnz %b0\n\t"
        "movzbl %b0, %0"
        : "=a"(res), "+S"(p1), "+D"(p2), "+c"(n)
        :
        : "cc"
    );

    if (res == 0) return 0;
    return (int)(*(p1 - 1)) - (int)(*(p2 - 1));
}

static inline size_t strlen(const char *s) {
    const char *p = s;
    size_t count = -1UL;
    asm volatile (
        "cld\n\t"
        "xor %%al, %%al\n\t"
        "repne scasb"
        : "+D"(p), "+c"(count)
        :
        : "memory"
    );
    return -2UL - count;
}

static inline char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

static inline char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n-- && (*d++ = *src++));
    while (n--) *d++ = '\0';
    return dest;
}

static inline char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

static inline int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    while (n-- > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    if (n == (size_t)-1) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}