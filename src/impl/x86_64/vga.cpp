#include "vga.h"
#include "util.h" // for outb/inb

namespace vga {

static volatile uint16_t* buffer = (volatile uint16_t*)VGA_VIRT;
static uint16_t cursor_x = 0;
static uint16_t cursor_y = 0;
static uint8_t color = (uint8_t)Color::LightGray | ((uint8_t)Color::Black << 4);

static inline uint16_t make_entry(char c) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void update_hardware_cursor() {
    uint16_t pos = cursor_y * WIDTH + cursor_x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

void init() {
    clear();
    enable_cursor();
}

void clear() {
    for (uint16_t y = 0; y < HEIGHT; y++) {
        for (uint16_t x = 0; x < WIDTH; x++) {
            buffer[y * WIDTH + x] = make_entry(' ');
        }
    }
    cursor_x = 0;
    cursor_y = 0;
    update_hardware_cursor();
}

static void newline() {
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= HEIGHT) {
        // scroll
        for (uint16_t y = 1; y < HEIGHT; y++)
            for (uint16_t x = 0; x < WIDTH; x++)
                buffer[(y - 1) * WIDTH + x] = buffer[y * WIDTH + x];
        for (uint16_t x = 0; x < WIDTH; x++)
            buffer[(HEIGHT - 1) * WIDTH + x] = make_entry(' ');
        cursor_y = HEIGHT - 1;
    }
}

void putchar(char c) {
    if (!buffer) return;

    if (c == '\n') {
        newline();
    } else {
        buffer[cursor_y * WIDTH + cursor_x] = make_entry(c);
        cursor_x++;
        if (cursor_x >= WIDTH) newline();
    }

    update_hardware_cursor();
}

void move_cursor(uint16_t x, uint16_t y) {
    if (x >= WIDTH) x = WIDTH - 1;
    if (y >= HEIGHT) y = HEIGHT - 1;
    cursor_x = x;
    cursor_y = y;
    update_hardware_cursor();
}

void enable_cursor(uint8_t start, uint8_t end) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, (inb(0x3D5) & 0xC0) | start);
    outb(0x3D4, 0x0B);
    outb(0x3D5, (inb(0x3D5) & 0xE0) | end);
    update_hardware_cursor();
}

void disable_cursor() {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20); // disable cursor
}

}
