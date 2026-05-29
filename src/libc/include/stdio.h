#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>

#define EOF (-1)

#ifdef __cplusplus
extern "C" {
#endif

int putchar(int c);
int puts(const char *s);
int snprintf(char *restrict buf, size_t size, const char *restrict fmt, ...);
int vsnprintf(char *restrict buf, size_t size, const char *restrict fmt, va_list ap);

#ifdef __cplusplus
}
#endif

#endif
