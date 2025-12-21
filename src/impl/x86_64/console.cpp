/**
 * @file console.cpp
 * @brief Implementation of kernel console abstraction
 */

#include "console.h"
#include "vga.h"
#include "framebuffer.h"
#include <stdarg.h>
#ifdef DEBUG
 #include "serial.h"
#endif

namespace console {

/** @internal Currently active console backend */
static Backend active_backend;

void init(Backend backend, struct multiboot_tag_framebuffer* fb_tag) {
    active_backend = backend;

    switch (backend) {
        case Backend::VGA:
            vga::init();
            break;
        case Backend::FRAMEBUFFER:
            framebuffer::init(fb_tag);
            break;
    }


#ifdef DEBUG
    serial::init();
#endif
}

void putchar(char c) {
    switch (active_backend) {
        case Backend::VGA:
            vga::putchar(c);
            break;
        case Backend::FRAMEBUFFER:
            framebuffer::putchar(c);
            break;
    }

#ifdef DEBUG
    serial::putchar(c);
#endif
}

void putnum(uint64_t n) {
    if (n == 0) {
        putchar('0');
        return;
    }

    char buf[20];
    int i = 0;
    while (n > 0) {
        buf[i++] = (n % 10) + '0';
        n /= 10;
    }

    while (--i >= 0) {
        putchar(buf[i]);
    }
}

void puthex(uint64_t n) {
    const char* hex_digits = "0123456789ABCDEF";
    
    write("0x");

    if (n == 0) {
        putchar('0');
        return;
    }

    char buf[16];
    int i = 0;

    while (n > 0) {
        buf[i++] = hex_digits[n & 0xF];
        n >>= 4;
    }

    while (--i >= 0) {
        putchar(buf[i]);
    }
}

void write(const char* str) {
    while (*str) putchar(*str++);
}

void printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt == '%' && *(fmt + 1)) {
            fmt++;
            switch (*fmt) {
                case 's': {
                    const char* s = va_arg(args, const char*);
                    write(s);
                    break;
                }
                case 'd': {
                    uint64_t n = va_arg(args, uint64_t);
                    putnum(n);
                    break;
                }
                case 'x': {
                    uint64_t n = va_arg(args, uint64_t);
                    puthex(n);
                    break;
                }
                case 'p': {
                    uint64_t n = (uint64_t)va_arg(args, void*);
                    puthex(n);
                    break;
                }
                case '%':
                    putchar('%');
                    break;
                default:
                    putchar(*fmt);
                    break;
            }
        } else {
            putchar(*fmt);
        }
        fmt++;
    }

    va_end(args);
}

void clear() {
    switch (active_backend) {
        case Backend::VGA:
            vga::clear();
            break;
        case Backend::FRAMEBUFFER:
            framebuffer::clear();
            break;
    }
}

void move_cursor(uint16_t x, uint16_t y) {
    switch (active_backend) {
        case Backend::VGA:
            vga::move_cursor(x, y);
            break;
        case Backend::FRAMEBUFFER:
            framebuffer::move_cursor(x, y);
            break;
    }
}

void enable_cursor() {
    switch (active_backend) {
        case Backend::VGA:
            vga::enable_cursor();
            break;
        case Backend::FRAMEBUFFER:
            framebuffer::enable_cursor();
            break;
    }
}

void disable_cursor() {
    switch (active_backend) {
        case Backend::VGA:
            vga::disable_cursor();
            break;
        case Backend::FRAMEBUFFER:
            framebuffer::disable_cursor();
            break;
    }
}

}
