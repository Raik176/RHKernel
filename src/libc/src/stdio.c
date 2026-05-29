#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct outbuf {
    char *buf;
    size_t size;
    size_t used;
};

static void out_ch(struct outbuf *out, char c) {
    if (out->size && out->used + 1 < out->size) out->buf[out->used] = c;
    out->used++;
}

static void out_str(struct outbuf *out, const char *s) {
    if (!s) s = "(null)";
    while (*s) out_ch(out, *s++);
}

static void out_uint(struct outbuf *out, uint64_t value, unsigned base, int upper) {
    char tmp[32];
    size_t n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value);
    while (n) out_ch(out, tmp[--n]);
}

int vsnprintf(char *restrict buf, size_t size, const char *restrict fmt, va_list ap) {
    struct outbuf out = {buf, size, 0};
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_ch(&out, *fmt);
            continue;
        }
        fmt++;
        int long_arg = 0;
        if (*fmt == 'l') {
            long_arg = 1;
            fmt++;
            if (*fmt == 'l') fmt++;
        }
        switch (*fmt) {
        case 0:
            fmt--;
            break;
        case '%':
            out_ch(&out, '%');
            break;
        case 'c':
            out_ch(&out, (char)va_arg(ap, int));
            break;
        case 's':
            out_str(&out, va_arg(ap, const char *));
            break;
        case 'd':
        case 'i': {
            int64_t v = long_arg ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) {
                out_ch(&out, '-');
                out_uint(&out, (uint64_t)(0 - (uint64_t)v), 10, 0);
            } else {
                out_uint(&out, (uint64_t)v, 10, 0);
            }
            break;
        }
        case 'u':
            out_uint(&out, long_arg ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 10, 0);
            break;
        case 'x':
            out_uint(&out, long_arg ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, 0);
            break;
        case 'X':
            out_uint(&out, long_arg ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int), 16, 1);
            break;
        case 'p':
            out_str(&out, "0x");
            out_uint(&out, (uintptr_t)va_arg(ap, void *), 16, 0);
            break;
        default:
            out_ch(&out, '%');
            out_ch(&out, *fmt);
            break;
        }
    }
    if (size) buf[out.used < size ? out.used : size - 1] = 0;
    return out.used > (size_t)INT32_MAX ? INT32_MAX : (int)out.used;
}

int snprintf(char *restrict buf, size_t size, const char *restrict fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int putchar(int c) {
    unsigned char ch = (unsigned char)c;
    return write(STDOUT_FILENO, &ch, 1) == 1 ? c : EOF;
}

int puts(const char *s) {
    size_t n = strlen(s);
    if (write(STDOUT_FILENO, s, n) != (ssize_t)n) return EOF;
    if (write(STDOUT_FILENO, "\n", 1) != 1) return EOF;
    return 0;
}
