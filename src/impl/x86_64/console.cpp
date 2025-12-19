#include "console.h"
#include "vga.h"

namespace console {

static Backend active_backend;

void init(Backend backend) {
    active_backend = backend;

    switch (backend) {
        case Backend::VGA:
            vga::init();
            break;
        case Backend::FRAMEBUFFER:
            // framebuffer init
            break;
    }
}

void putchar(char c) {
    switch (active_backend) {
        case Backend::VGA:
            vga::putchar(c);
            break;
        case Backend::FRAMEBUFFER:
            break;
    }
}

void write(const char* str) {
    while (*str) putchar(*str++);
}

void clear() {
    switch (active_backend) {
        case Backend::VGA:
            vga::clear();
            break;
        case Backend::FRAMEBUFFER:
            break;
    }
}

void move_cursor(uint16_t x, uint16_t y) {
    switch (active_backend) {
        case Backend::VGA:
            vga::move_cursor(x, y);
            break;
        case Backend::FRAMEBUFFER:
            break;
    }
}

void enable_cursor() {
    switch (active_backend) {
        case Backend::VGA:
            vga::enable_cursor();
            break;
        case Backend::FRAMEBUFFER:
            break;
    }
}

void disable_cursor() {
    switch (active_backend) {
        case Backend::VGA:
            vga::disable_cursor();
            break;
        case Backend::FRAMEBUFFER:
            break;
    }
}

}
