#include "string.h"

#include "memory/heap.h"
#include "symbol/ksym.h"

static inline bool same_u64_alignment(const void *a, const void *b) {
    return (((uintptr_t)a ^ (uintptr_t)b) & 7U) == 0;
}

void *memcpy(void *dest, const void *src, size_t n) {
    void *ret = dest;
    if (n == 0 || dest == src) return ret;

    if (n >= 64 && same_u64_alignment(dest, src)) {
        uint8_t *d8 = (uint8_t *)dest;
        const uint8_t *s8 = (const uint8_t *)src;
        while (((uintptr_t)d8 & 7U) != 0) {
            *d8++ = *s8++;
            n--;
        }

        size_t qwords = n >> 3;
        size_t tail = n & 7U;
        asm volatile("cld; rep movsq" : "+D"(d8), "+S"(s8), "+c"(qwords) : : "memory");
        n = tail;
        dest = d8;
        src = s8;
    }

    asm volatile("cld; rep movsb" : "+D"(dest), "+S"(src), "+c"(n) : : "memory");
    return ret;
}
KEXPORT(memcpy)

void *memmove(void *dest, const void *src, size_t n) {
    void *ret = dest;
    if (n == 0 || dest == src) return ret;

    uintptr_t d = (uintptr_t)dest;
    uintptr_t s = (uintptr_t)src;
    if (d < s || d - s >= n) return memcpy(dest, src, n);

    uint8_t *d8 = (uint8_t *)dest + n;
    const uint8_t *s8 = (const uint8_t *)src + n;

    if (n >= 64 && (((uintptr_t)d8 ^ (uintptr_t)s8) & 7U) == 0) {
        while (((uintptr_t)d8 & 7U) != 0) {
            *--d8 = *--s8;
            n--;
        }

        uint64_t *d64 = (uint64_t *)d8;
        const uint64_t *s64 = (const uint64_t *)s8;
        size_t qwords = n >> 3;
        while (qwords--) *--d64 = *--s64;
        d8 = (uint8_t *)d64;
        s8 = (const uint8_t *)s64;
        n &= 7U;
    }

    while (n--) *--d8 = *--s8;
    return ret;
}
KEXPORT(memmove)

void *memset(void *s, int c, size_t n) {
    void *ret = s;
    if (n == 0) return ret;

    uint8_t *d8 = (uint8_t *)s;
    uint8_t byte = (uint8_t)c;

    if (n >= 32) {
        while (((uintptr_t)d8 & 7U) != 0) {
            *d8++ = byte;
            n--;
        }

        uint64_t pattern = byte;
        pattern |= pattern << 8;
        pattern |= pattern << 16;
        pattern |= pattern << 32;

        size_t qwords = n >> 3;
        asm volatile("cld; rep stosq" : "+D"(d8), "+c"(qwords) : "a"(pattern) : "memory");
        n &= 7U;
    }

    while (n--) *d8++ = byte;
    return ret;
}
KEXPORT(memset)

int memcmp(const void *s1, const void *s2, size_t n) {
    if (n == 0 || s1 == s2) return 0;
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
KEXPORT(strlen)

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}
KEXPORT(strcpy)

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;

    while (n > 0 && *src) {
        *d++ = *src++;
        n--;
    }

    if (n) memset(d, 0, n);
    return dest;
}
KEXPORT(strncpy)

char *strcat(char *dest, const char *src) {
    char *d = dest + strlen(dest);
    strcpy(d, src);
    return dest;
}
KEXPORT(strcat)

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}
KEXPORT(strcmp)

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}
KEXPORT(strncmp)

char *strchr(const char *s, int c) {
    unsigned char ch = (unsigned char)c;
    for (;;) {
        if ((unsigned char)*s == ch) return (char *)s;
        if (*s == '\0') return nullptr;
        s++;
    }
}
KEXPORT(strchr)

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *dest = (char *)heap::kmalloc(len);
    if (dest) memcpy(dest, s, len);
    return dest;
}
KEXPORT(strdup)

static void build_delim_map(const char *delim, uint8_t map[32]) {
    memset(map, 0, 32);
    while (*delim) {
        uint8_t c = (uint8_t)*delim++;
        map[c >> 3] |= (uint8_t)(1U << (c & 7));
    }
}

static inline bool delim_contains(const uint8_t map[32], char c) {
    uint8_t uc = (uint8_t)c;
    return (map[uc >> 3] & (uint8_t)(1U << (uc & 7))) != 0;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    uint8_t delim_map[32];
    build_delim_map(delim, delim_map);

    if (str == nullptr) str = *saveptr;
    while (*str && delim_contains(delim_map, *str)) str++;
    if (*str == '\0') {
        *saveptr = str;
        return nullptr;
    }

    char *token = str;
    while (*str && !delim_contains(delim_map, *str)) str++;
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

static void emit_char(char **buf, size_t *remaining, size_t *produced, char c) {
    if (*remaining > 1) {
        **buf = c;
        (*buf)++;
        (*remaining)--;
    }
    (*produced)++;
}

static void emit_unsigned(char **buf, size_t *remaining, size_t *produced,
                          uint64_t n, unsigned base, int width, char pad, bool upper) {
    char tmp[64];
    int i = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (base < 2 || base > 16) base = 10;
    do {
        tmp[i++] = digits[n % base];
        n /= base;
    } while (n != 0);

    while (i < width) {
        emit_char(buf, remaining, produced, pad);
        width--;
    }
    while (i > 0) emit_char(buf, remaining, produced, tmp[--i]);
}

static void emit_signed(char **buf, size_t *remaining, size_t *produced, int64_t n,
                        int width, char pad) {
    uint64_t magnitude;
    if (n < 0) {
        emit_char(buf, remaining, produced, '-');
        magnitude = 0ULL - (uint64_t)n;
        if (width > 0) width--;
    } else {
        magnitude = (uint64_t)n;
    }
    emit_unsigned(buf, remaining, produced, magnitude, 10, width, pad, false);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    if (!fmt) return -1;

    char sink = 0;
    if (!buf) {
        if (size != 0) return -1;
        buf = &sink;
    }

    char *out = buf;
    size_t remaining = size;
    size_t produced = 0;

    while (*fmt) {
        if (*fmt != '%') {
            emit_char(&out, &remaining, &produced, *fmt++);
            continue;
        }

        fmt++;
        if (*fmt == 0) break;

        char pad = ' ';
        int width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            if (width < 1000000) width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int long_count = 0;
        bool size_arg = false;
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 't') {
            if (*fmt == 'l') long_count++;
            else size_arg = true;
            fmt++;
        }

        switch (*fmt) {
            case 's': {
                const char *str = va_arg(args, const char *);
                if (!str) str = "(null)";
                while (*str) emit_char(&out, &remaining, &produced, *str++);
                break;
            }
            case 'd':
            case 'i': {
                int64_t value;
                if (size_arg || long_count >= 2) value = va_arg(args, int64_t);
                else if (long_count == 1) value = va_arg(args, long);
                else value = va_arg(args, int);
                emit_signed(&out, &remaining, &produced, value, width, pad);
                break;
            }
            case 'u': {
                uint64_t value;
                if (size_arg || long_count >= 2) value = va_arg(args, uint64_t);
                else if (long_count == 1) value = va_arg(args, unsigned long);
                else value = va_arg(args, unsigned int);
                emit_unsigned(&out, &remaining, &produced, value, 10, width, pad, false);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t value;
                if (size_arg || long_count >= 2) value = va_arg(args, uint64_t);
                else if (long_count == 1) value = va_arg(args, unsigned long);
                else value = va_arg(args, unsigned int);
                emit_unsigned(&out, &remaining, &produced, value, 16, width, pad, *fmt == 'X');
                break;
            }
            case 'p': {
                uintptr_t value = (uintptr_t)va_arg(args, void *);
                emit_unsigned(&out, &remaining, &produced, value, 16, 0, ' ', false);
                break;
            }
            case 'c':
                emit_char(&out, &remaining, &produced, (char)va_arg(args, int));
                break;
            case '%':
                emit_char(&out, &remaining, &produced, '%');
                break;
            default:
                emit_char(&out, &remaining, &produced, '%');
                emit_char(&out, &remaining, &produced, *fmt);
                break;
        }
        fmt++;
    }

    if (size != 0) {
        if (remaining == 0) buf[size - 1] = 0;
        else *out = 0;
    }
    if (produced > 0x7fffffffU) return -1;
    return (int)produced;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int res = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return res;
}
KEXPORT(snprintf)
