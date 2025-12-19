#pragma once
#include <stdint.h>

#define VGA_VIRT 0xFFFF8000000B8000ULL

namespace vga {

constexpr uint16_t WIDTH  = 80;
constexpr uint16_t HEIGHT = 25;

enum class Color : uint8_t {
    Black = 0,
    Blue = 1,
    Green = 2,
    Cyan = 3,
    Red = 4,
    Magenta = 5,
    Brown = 6,
    LightGray = 7,
    DarkGray = 8,
    LightBlue = 9,
    LightGreen = 10,
    LightCyan = 11,
    LightRed = 12,
    Pink = 13,
    Yellow = 14,
    White = 15
};

// Initialize VGA buffer and set text mode (if needed)
void init();

// Put a character at the current cursor
void putchar(char c);

// Clear the screen
void clear();

// Move hardware cursor to (x, y)
void move_cursor(uint16_t x, uint16_t y);

// Enable hardware cursor
void enable_cursor(uint8_t start = 0, uint8_t end = 15);

// Disable hardware cursor
void disable_cursor();

}
