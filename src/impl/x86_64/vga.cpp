/**
 * @file vga.cpp
 * @brief Implementation of VGA text-mode driver
 */

#include "vga.h"

#include "console.h"
#include "string.h"
#include "util.h"

namespace vga {

    /**
     * @internal
     * VGA text buffer pointer
     */
    static volatile uint16_t *buffer = (volatile uint16_t *)p2v(0xB8000);

    /**
     * @name Current cursor position
     * @{
     */
    static uint16_t cursor_x = 0;  ///< X Position
    static uint16_t cursor_y = 0;  ///< Y Position
    /** @} */

    /** @internal Current text color (foreground | background << 4) */
    static uint8_t color = (uint8_t)console::Color::White | ((uint8_t)console::Color::Black << 4);

    /**
     * @internal
     * Create a VGA entry from a character and color
     * @param c Character
     * @return 16-bit VGA entry
     */
    static inline uint16_t make_entry(uint8_t c) { return (uint16_t)c | ((uint16_t)color << 8); }

    /**
     * @internal
     * Update hardware cursor to current `cursor_x` and `cursor_y`
     */
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
        uint16_t entry = make_entry(' ');

        for (uint16_t x = 0; x < WIDTH; x++) { buffer[x] = entry; }

        for (uint16_t y = 1; y < HEIGHT; y++) {
            memcpy((void *)&buffer[y * WIDTH], (void *)&buffer[0], WIDTH * 2);
        }

        cursor_x = 0;
        cursor_y = 0;
        update_hardware_cursor();
    }

    /**
     * @internal
     * Handle newline and scrolling
     */
    static void newline() {
        cursor_x = 0;
        cursor_y++;

        if (cursor_y >= HEIGHT) {
            size_t bytes_to_move = (HEIGHT - 1) * WIDTH * 2;
            memcpy((void *)buffer, (void *)(buffer + WIDTH), bytes_to_move);

            uint16_t entry = make_entry(' ');
            uint16_t *last_line = (uint16_t *)(buffer + (HEIGHT - 1) * WIDTH);
            for (uint16_t x = 0; x < WIDTH; x++) { last_line[x] = entry; }

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
        outb(0x3D5, 0x20);
    }

    void set_color(uint8_t fg, uint8_t bg) { color = fg | (bg << 4); }
}  // namespace vga
