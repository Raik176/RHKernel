/**
 * @file console.h
 * @brief Abstraction layer for kernel console output.
 *
 * This file provides a unified interface for writing characters and strings
 * to different console backends. It supports VGA text mode and framebuffer
 * backends, and provides basic operations such as clearing the screen,
 * moving the cursor, and formatted output.
 */

#pragma once
#define DEBUG
#include <stdarg.h>
#include <stdint.h>

#include "multiboot2.h"

namespace console {
    enum class Color : uint8_t {
        Black = 0,
        Blue,
        Green,
        Cyan,
        Red,
        Magenta,
        Brown,
        LightGray,
        DarkGray,
        LightBlue,
        LightGreen,
        LightCyan,
        LightRed,
        LightMagenta,
        Yellow,
        White
    };

    /**
     * @brief Console backends that can be used for output
     */
    enum class Backend {
        VGA,         ///< VGA text mode output
        FRAMEBUFFER  ///< Framebuffer output
    };

    /** @defgroup Console_Core Console Core Functions
     *  @brief Initialization and basic console operations
     *  @{
     */

    /**
     * @brief Initialize the console with a specific backend
     *
     * Sets up the console for output and prepares the cursor.
     *
     * @param backend The backend to use (VGA or FRAMEBUFFER)
     * @param fb_tag Optional pointer to a multiboot framebuffer tag (required for FRAMEBUFFER)
     */
    void init(Backend backend, multiboot_tag_framebuffer *fb_tag);

    /**
     * @brief Clear the console screen
     *
     * Clears all output using the active backend and resets the cursor to (0,0).
     */
    void clear();

    /**
     * @brief Enable the hardware cursor
     *
     * Makes the cursor visible if the backend supports it.
     */
    void enable_cursor();

    /**
     * @brief Disable the hardware cursor
     *
     * Hides the cursor if the backend supports it.
     */
    void disable_cursor();

    /** @} */

    /** @defgroup Console_Output Character Output
     *  @brief Functions for writing characters and strings to the console
     *  @{
     */

    /**
     * @brief Write a single character to the console
     *
     * @param c Character to write
     */
    void putchar(char c);

    /**
     * @brief Formatted output to the console
     *
     * Supports:
     * - %s: null-terminated string
     * - %d: unsigned decimal
     * - %x: unsigned hexadecimal
     * - %p: pointer (printed as hexadecimal)
     * - %%: literal '%'
     *
     * @param fmt Format string
     * @param ... Additional arguments
     */
    void printf(const char *fmt, ...);
    void vprintf(const char *fmt, va_list args);

    void hexdump(const void *data, size_t size);

    /**
     * @brief Move the cursor to a specific position
     *
     * @param x Column (0-based)
     * @param y Row (0-based)
     */
    void move_cursor(uint16_t x, uint16_t y);

    /** @} */

    void set_color(Color fg, Color bg = Color::Black);

}  // namespace console