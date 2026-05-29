#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

static unsigned long parse_unsigned(const char *nptr, char **endptr, int base, int *neg) {
    const char *s = nptr;
    while (isspace((unsigned char)*s)) s++;

    *neg = 0;
    if (*s == '+' || *s == '-') *neg = *s++ == '-';

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    if (base < 2 || base > 36) {
        if (endptr) *endptr = (char *)nptr;
        return 0;
    }

    unsigned long acc = 0;
    int any = 0;
    for (;;) {
        int d = digit_value((unsigned char)*s);
        if (d < 0 || d >= base) break;
        unsigned long ud = (unsigned long)d;
        if (acc > (ULONG_MAX - ud) / (unsigned long)base) {
            acc = ULONG_MAX;
            any = 1;
            do { s++; } while ((d = digit_value((unsigned char)*s)) >= 0 && d < base);
            break;
        }
        acc = acc * (unsigned long)base + ud;
        any = 1;
        s++;
    }

    if (endptr) *endptr = (char *)(any ? s : nptr);
    return acc;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    int neg = 0;
    unsigned long value = parse_unsigned(nptr, endptr, base, &neg);
    return neg ? (unsigned long)(0UL - value) : value;
}

long strtol(const char *nptr, char **endptr, int base) {
    int neg = 0;
    unsigned long value = parse_unsigned(nptr, endptr, base, &neg);
    unsigned long limit = neg ? (unsigned long)LONG_MAX + 1UL : (unsigned long)LONG_MAX;
    if (value > limit) value = limit;
    if (neg) return value == (unsigned long)LONG_MAX + 1UL ? LONG_MIN : -(long)value;
    return (long)value;
}

int atoi(const char *nptr) { return (int)strtol(nptr, 0, 10); }
long atol(const char *nptr) { return strtol(nptr, 0, 10); }

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    const unsigned char *b = (const unsigned char *)base;
    while (nmemb) {
        size_t mid = nmemb / 2;
        const void *elem = b + mid * size;
        int cmp = compar(key, elem);
        if (cmp == 0) return (void *)elem;
        if (cmp > 0) {
            b += (mid + 1) * size;
            nmemb -= mid + 1;
        } else {
            nmemb = mid;
        }
    }
    return 0;
}

static void byte_swap(unsigned char *a, unsigned char *b, size_t size) {
    for (size_t i = 0; i < size; i++) {
        unsigned char t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

static void insertion_sort(unsigned char *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    for (size_t i = 1; i < nmemb; i++) {
        size_t j = i;
        while (j > 0 && compar(base + j * size, base + (j - 1) * size) < 0) {
            byte_swap(base + j * size, base + (j - 1) * size, size);
            j--;
        }
    }
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    if (!base || !compar || size == 0 || nmemb < 2) return;
    insertion_sort((unsigned char *)base, nmemb, size, compar);
}
