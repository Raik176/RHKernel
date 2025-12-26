/**
 * @file console.cpp
 * @brief Implementation of kernel console abstraction
 *
 * This file implements the console interface declared in console.h.
 * It handles output to different backends (VGA, framebuffer) and
 * optionally to the serial port when DEBUG is enabled.
 */

#include "console.h"

#define DEBUG // TODO: REMOVE
#include <stdarg.h>

#include "framebuffer.h"
#include "smp/lock.h"
#include "vga.h"
#ifdef DEBUG
#include "serial.h"
#endif

namespace console {
    static lock::spinlock console_lock;

    /** @internal Currently active console backend */
    static Backend active_backend;

    /**
     * @internal
     * Stores the active backend and initializes the relevant hardware.
     */
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

    void putchar_internal(char c) {
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

    /**
     * @internal
     * Write a single character to the currently active backend.
     * Also sends the character to the serial port if DEBUG is enabled.
     */
    void putchar(char c) {
        uint64_t flags;

        console_lock.acquire(flags);
        putchar_internal(c);
        console_lock.release(flags);
    }

    /**
     * @internal
     * Write a null-terminated string to the console by calling putchar repeatedly.
     *
     * @param str Pointer to string
     */
    void write(const char* str) {
        while (*str) putchar_internal(*str++);
    }

    void putnum(uint64_t n, int width = 0, char pad = ' ') {
        char buf[20];
        int i = 0;
        uint64_t temp = n;

        do {
            buf[i++] = (temp % 10) + '0';
            temp /= 10;
        } while (temp > 0);

        while (i < width) {
            putchar_internal(pad);
            width--;
        }

        while (--i >= 0) putchar_internal(buf[i]);
    }

    void puthex(uint64_t n, int width = 0, char pad = ' ') {
        write("0x");

        char buf[16];
        int i = 0;
        uint64_t temp = n;
        const char* hex_digits = "0123456789ABCDEF";

        do {
            buf[i++] = hex_digits[temp & 0xF];
            temp >>= 4;
        } while (temp > 0);

        while (i < width) {
            putchar_internal(pad);
            width--;
        }

        while (--i >= 0) putchar_internal(buf[i]);
    }

    /**
     * @internal
     * Simplified printf implementation.
     *
     * Supports:
     * - %s: null-terminated string
     * - %d: unsigned decimal
     * - %x: unsigned hexadecimal
     * - %p: pointer (printed as hexadecimal)
     * - %%: literal '%'
     *
     * @param fmt Format string
     * @param ... Variable arguments
     */
    void printf(const char* fmt, ...) {
        uint64_t flags;

        console_lock.acquire(flags);
        va_list args;
        va_start(args, fmt);

        while (*fmt) {
            if (*fmt == '%' && *(fmt + 1)) {
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
                        const char* s = va_arg(args, const char*);
                        write(s);
                        break;
                    }
                    case 'd': {
                        uint64_t n = va_arg(args, uint64_t);
                        putnum(n, width, pad);
                        break;
                    }
                    case 'x': {
                        uint64_t n = va_arg(args, uint64_t);
                        puthex(n, width, pad);
                        break;
                    }
                    case 'p': {
                        uint64_t n = (uint64_t)va_arg(args, void*);
                        puthex(n, 16, '0');
                        break;
                    }
                    case '%':
                        putchar_internal('%');
                        break;
                    default:
                        putchar_internal(*fmt);
                        break;
                }
            } else {
                putchar_internal(*fmt);
            }
            fmt++;
        }

        va_end(args);
        console_lock.release(flags);
    }

    /**
     * @internal
     * Clear the console using the currently active backend.
     */
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

    /**
     * @internal
     * Move the cursor to a specific column and row on the active backend.
     *
     * @param x Column (0-based)
     * @param y Row (0-based)
     */
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

    /**
     * @internal
     * Enable the cursor on the active backend.
     */
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

    /**
     * @internal
     * Disable the hardware cursor on the active backend.
     */
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

    void set_color(Color fg, Color bg) {
        uint64_t flags;
        console_lock.acquire(flags);

        switch (active_backend) {
            case Backend::VGA:
                vga::set_color((uint8_t)fg, (uint8_t)bg);
                break;
            case Backend::FRAMEBUFFER:
                framebuffer::set_color((uint8_t)fg, (uint8_t)bg);
                break;
        }

        console_lock.release(flags);
    }

}  // namespace console