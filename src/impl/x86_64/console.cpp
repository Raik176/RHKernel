/**
 * @file console.cpp
 * @brief Implementation of kernel console abstraction
 *
 * This file implements the console interface declared in console.h.
 * It handles output to different backends (VGA, framebuffer) and
 * optionally to the serial port when DEBUG is enabled.
 */

#include "console.h"

#include "framebuffer.h"
#include "smp/lock.h"
#include "vga.h"
#ifdef DEBUG
#include "serial.h"
#endif

namespace console {
    static spinlock_t console_lock;

    /** @internal Currently active console backend */
    static Backend active_backend;

    /**
     * @internal
     * Stores the active backend and initializes the relevant hardware.
     */
    void init(Backend backend, struct multiboot_tag_framebuffer *fb_tag) {
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

    static void backspace_internal() {
        switch (active_backend) {
            case Backend::VGA:
                vga::backspace();
                break;
            case Backend::FRAMEBUFFER:
                framebuffer::backspace();
                break;
        }

#ifdef DEBUG
        serial::putchar('\b');
        serial::putchar(' ');
        serial::putchar('\b');
#endif
    }

    void putchar_internal(char c) {
        if (c == '\b') {
            backspace_internal();
            return;
        }

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

    void backspace() {
        uint64_t flags;
        console_lock.acquire(flags);
        backspace_internal();
        console_lock.release(flags);
    }

    /**
     * @internal
     * Write a null-terminated string to the console by calling putchar repeatedly.
     *
     * @param str Pointer to string
     */
    static void write_internal(const char *str) {
        while (*str) putchar_internal(*str++);
    }

    void write(const char *str) {
        uint64_t flags;
        console_lock.acquire(flags);
        write_internal(str);
        console_lock.release(flags);
    }

    void putnum(uint64_t n, int width = 0, char pad = ' ') {
        char buf[20];
        int i = 0;
        uint64_t temp = n;

        do {
            buf[i++] = (temp % 10ULL) + '0';
            temp /= 10ULL;
        } while (temp > 0);

        while (i < width) {
            putchar_internal(pad);
            width--;
        }

        while (--i >= 0) putchar_internal(buf[i]);
    }

    void putint(int64_t n, int width = 0, char pad = ' ') {
        if (n < 0) {
            putchar_internal('-');
            if (width > 0) width--;
            putnum(0ULL - (uint64_t)n, width, pad);
            return;
        }
        putnum((uint64_t)n, width, pad);
    }

    void puthex(uint64_t n, int width = 0, char pad = ' ') {
        write_internal("0x");

        char buf[16];
        int i = 0;
        uint64_t temp = n;
        const char *hex_digits = "0123456789ABCDEF";

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

    void hexdump(const void *data, size_t size) {
        const uint8_t *ptr = reinterpret_cast<const uint8_t *>(data);
        const size_t bytes_per_line = 16;

        uint64_t flags;
        console_lock.acquire(flags);

        for (size_t i = 0; i < size; i += bytes_per_line) {
            puthex(i, 8, '0');
            write_internal(": ");

            size_t line_bytes = (i + bytes_per_line <= size) ? bytes_per_line : size - i;

            for (size_t j = 0; j < line_bytes; j++) {
                puthex(ptr[i + j], 2, '0');
                write_internal(" ");
            }

            for (size_t j = line_bytes; j < bytes_per_line; j++) { write_internal("     "); }

            write_internal(" |");
            for (size_t j = 0; j < line_bytes; j++) {
                char c = ptr[i + j];
                putchar_internal((c >= 32 && c <= 126) ? c : '.');
            }

            for (size_t j = line_bytes; j < bytes_per_line; j++) { putchar_internal(' '); }

            write_internal("|\n");
        }

        console_lock.release(flags);
    }


    void panic_unlock_output() {
        __atomic_clear(&console_lock.locked, __ATOMIC_RELEASE);
    }

    void printf(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }

    /**
     * @internal
     * Simplified vprintf implementation.
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
    void vprintf(const char *fmt, va_list args) {
        uint64_t flags;

        console_lock.acquire(flags);

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

                int long_count = 0;
                bool size_arg = false;
                while (*fmt == 'l' || *fmt == 'z' || *fmt == 't') {
                    if (*fmt == 'l') long_count++;
                    else size_arg = true;
                    fmt++;
                }

                switch (*fmt) {
                    case 's': {
                        const char *s = va_arg(args, const char *);
                        write_internal(s ? s : "(null)");
                        break;
                    }
                    case 'd': {
                        int64_t n;
                        if (size_arg || long_count >= 2) n = va_arg(args, int64_t);
                        else if (long_count == 1) n = va_arg(args, long);
                        else n = va_arg(args, int);
                        putint(n, width, pad);
                        break;
                    }
                    case 'u': {
                        uint64_t n;
                        if (size_arg || long_count >= 2) n = va_arg(args, uint64_t);
                        else if (long_count == 1) n = va_arg(args, unsigned long);
                        else n = va_arg(args, unsigned int);
                        putnum(n, width, pad);
                        break;
                    }
                    case 'x':
                    case 'X': {
                        uint64_t n;
                        if (size_arg || long_count >= 2) n = va_arg(args, uint64_t);
                        else if (long_count == 1) n = va_arg(args, unsigned long);
                        else n = va_arg(args, unsigned int);
                        puthex(n, width, pad);
                        break;
                    }
                    case 'p': {
                        uint64_t n = (uint64_t)va_arg(args, void *);
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

        console_lock.release(flags);
    }

    /**
     * @internal
     * Clear the console using the currently active backend.
     */
    void clear() {
        uint64_t flags;
        console_lock.acquire(flags);
        switch (active_backend) {
            case Backend::VGA:
                vga::clear();
                break;
            case Backend::FRAMEBUFFER:
                framebuffer::clear();
                break;
        }
        console_lock.release(flags);
    }

    /**
     * @internal
     * Move the cursor to a specific column and row on the active backend.
     *
     * @param x Column (0-based)
     * @param y Row (0-based)
     */
    void move_cursor(uint16_t x, uint16_t y) {
        uint64_t flags;
        console_lock.acquire(flags);
        switch (active_backend) {
            case Backend::VGA:
                vga::move_cursor(x, y);
                break;
            case Backend::FRAMEBUFFER:
                framebuffer::move_cursor(x, y);
                break;
        }
        console_lock.release(flags);
    }

    /**
     * @internal
     * Enable the cursor on the active backend.
     */
    void enable_cursor() {
        uint64_t flags;
        console_lock.acquire(flags);
        switch (active_backend) {
            case Backend::VGA:
                vga::enable_cursor();
                break;
            case Backend::FRAMEBUFFER:
                framebuffer::enable_cursor();
                break;
        }
        console_lock.release(flags);
    }

    /**
     * @internal
     * Disable the hardware cursor on the active backend.
     */
    void disable_cursor() {
        uint64_t flags;
        console_lock.acquire(flags);
        switch (active_backend) {
            case Backend::VGA:
                vga::disable_cursor();
                break;
            case Backend::FRAMEBUFFER:
                framebuffer::disable_cursor();
                break;
        }
        console_lock.release(flags);
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