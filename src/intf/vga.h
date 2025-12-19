/**
 * @file vga.h
 * @brief VGA text-mode driver for the kernel
 *
 * Provides functions for writing characters to the VGA text buffer,
 * clearing the screen, and controlling the hardware cursor.
 */

#pragma once
#include <stdint.h>

/// VGA text buffer virtual address
#define VGA_VIRT 0xFFFF8000000B8000ULL

namespace vga {

/**
 * @name Screen Dimensions
 * @{
 */
constexpr uint16_t WIDTH  = 80;  ///< Number of columns
constexpr uint16_t HEIGHT = 25;  ///< Number of rows
/** @} */

/**
 * @brief VGA color codes
 */
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

/** @defgroup VGA_Core VGA Core Functions
 * Functions for initializing and clearing the screen.
 * @{
 */

/**
 * @brief Initialize VGA text mode
 *
 * Clears the screen and enables the hardware cursor.
 */
void init();

/**
 * @brief Clear the screen
 *
 * Fills the VGA buffer with spaces and resets the cursor to (0,0).
 */
void clear();

/** @} */

/** @defgroup VGA_Output Character Output
 * Functions for writing characters and controlling the cursor.
 * @{
 */

/**
 * @brief Write a single character to the screen
 * @param c Character to write
 *
 * Handles newline characters and scrolling automatically.
 */
void putchar(char c);

/**
 * @brief Move the hardware cursor
 * @param x Column (0-based)
 * @param y Row (0-based)
 */
void move_cursor(uint16_t x, uint16_t y);

/**
 * @brief Enable the hardware cursor
 * @param start Start scanline (default 0)
 * @param end End scanline (default 15)
 */
void enable_cursor(uint8_t start = 0, uint8_t end = 15);

/**
 * @brief Disable the hardware cursor
 */
void disable_cursor();

/** @} */

}
