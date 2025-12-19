#pragma once
#include <stdint.h>

namespace console {

enum class Backend {
    VGA,
    FRAMEBUFFER
};

// Initialize console backend
void init(Backend backend);

// Print a single character
void putchar(char c);

// Print a null-terminated string
void write(const char* str);

// Clear the screen
void clear();

// Move cursor to (x, y)
void move_cursor(uint16_t x, uint16_t y);

// Enable hardware cursor
void enable_cursor();

// Disable hardware cursor
void disable_cursor();

}
