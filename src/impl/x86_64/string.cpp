#include "string.h"

#include "memory/heap.h"
#include "symbol/ksym.h"

void *memcpy(void *dest, const void *src, size_t n) {
    void *ret = dest;
    asm volatile("cld; rep movsb" : "+D"(dest), "+S"(src), "+c"(n) : : "memory");
    return ret;
}
KEXPORT(memcpy)

void *memset(void *s, int c, size_t n) {
    void *ret = s;
    asm volatile("cld; rep stosb" : "+D"(s), "+c"(n) : "a"((uint8_t)c) : "memory");
    return ret;
}
KEXPORT(memset)

int memcmp(const void *s1, const void *s2, size_t n) {
    if (n == 0) return 0;
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    unsigned char res = 0;
    asm volatile("cld; repe cmpsb; setnz %0" : "=q"(res), "+S"(p1), "+D"(p2), "+c"(n) : : "cc");
    if (res == 0) return 0;
    return (int)(*(p1 - 1)) - (int)(*(p2 - 1));
}
KEXPORT(memcmp)

size_t strlen(const char *s) {
    const char *p = s;
    size_t count = -1UL;
    asm volatile("cld; xor %%al, %%al; repne scasb" : "+D"(p), "+c"(count) : : "memory");
    return -2UL - count;
}
KEXPORT(strlen);

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}
KEXPORT(strcpy)

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n-- && (*d++ = *src++));
    while (n--) *d++ = '\0';
    return dest;
}
KEXPORT(strncpy)

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}
KEXPORT(strcat)

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}
KEXPORT(strcmp)

int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    while (n-- > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    if (n == (size_t)-1) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}
KEXPORT(strncmp)

char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char *)s;
        s++;
    }
    return (ch == 0) ? (char *)s : nullptr;
}
KEXPORT(strchr)

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *dest = (char *)heap::kmalloc(len);
    if (dest) memcpy(dest, s, len);
    return dest;
}
KEXPORT(strdup)

char *strtok_r(char *str, const char *delim, char **saveptr) {
    char *token;
    if (str == nullptr) str = *saveptr;
    while (*str && strchr(delim, *str)) str++;
    if (*str == '\0') {
        *saveptr = str;
        return nullptr;
    }
    token = str;
    while (*str && !strchr(delim, *str)) str++;
    if (*str) {
        *str = '\0';
        *saveptr = str + 1;
    } else {
        *saveptr = str;
    }
    return token;
}
KEXPORT(strtok_r)

#include <stdarg.h>

#include "string.h"
#include "symbol/ksym.h"

static void itoa(char **buf, size_t *limit, uint64_t n, int base, int width, char pad) {
    char tmp[64];
    int i = 0;
    const char *digits = "0123456789abcdef";

    do {
        tmp[i++] = digits[n % base];
        n /= base;
    } while (n > 0);

    while (i < width && i < 63) { tmp[i++] = pad; }

    while (--i >= 0 && *limit > 1) {
        **buf = tmp[i];
        (*buf)++;
        (*limit)--;
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    if (size == 0) return 0;

    char *start = buf;
    size_t limit = size;

    while (*fmt && limit > 1) {
        if (*fmt == '%') {
            fmt++;
            char pad = ' ';
            int width = 0;

            if (*fmt == '0') {
                pad = '0';
                fmt++;
            }
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }

            switch (*fmt) {
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if (!s) s = "(null)";
                    while (*s && limit > 1) {
                        *buf++ = *s++;
                        limit--;
                    }
                    break;
                }
                case 'd':
                    itoa(&buf, &limit, va_arg(args, uint64_t), 10, width, pad);
                    break;
                case 'x':
                case 'p':
                    itoa(&buf, &limit, va_arg(args, uint64_t), 16, width, pad);
                    break;
                case 'c':
                    *buf++ = (char)va_arg(args, int);
                    limit--;
                    break;
                case '%':
                    *buf++ = '%';
                    limit--;
                    break;
                default:
                    *buf++ = *fmt;
                    limit--;
                    break;
            }
        } else {
            *buf++ = *fmt;
            limit--;
        }
        fmt++;
    }

    *buf = '\0';
    return (int)(buf - start);
}
KEXPORT(vsnprintf)

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int res = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return res;
}
KEXPORT(snprintf)