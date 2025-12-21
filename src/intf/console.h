/**
 * @file console.h
 * @brief Abstraction layer for kernel console output
 *
 * Provides a unified interface for writing characters and strings to
 * different backends.
 */

#pragma once
#include <stdint.h>
#include "multiboot2.h"

namespace console {

/**
 * @brief Available console backends
 */
enum class Backend {
    VGA,          ///< Use VGA text mode
    FRAMEBUFFER   ///< @todo Implement framebuffer support
};

/** @defgroup Console_Core Console Core Functions
 * Initialization and basic console operations
 * @{
 */

/**
 * @brief Initialize the console with a specific backend
 * @param backend The console backend to use (VGA, FRAMEBUFFER)
 */
void init(Backend backend, multiboot_tag_framebuffer* fb_tag);

/**
 * @brief Clear the screen
 *
 * Clears the output using the active backend and resets the cursor.
 */
void clear();

/**
 * @brief Enable the hardware cursor
 *
 * Only effective if the backend supports a visible cursor.
 */
void enable_cursor();

/**
 * @brief Disable the hardware cursor
 */
void disable_cursor();

/** @} */

/** @defgroup Console_Output Character Output
 * Functions for writing characters and strings
 * @{
 */

/**
 * @brief Write a single character to the console
 * @param c Character to write
 */
void putchar(char c);

void putnum(uint64_t n);

void puthex(uint64_t n);

/**
 * @brief Write a null-terminated string to the console
 * @param str Pointer to the string
 */
void write(const char* str);

void printf(const char* fmt, ...);

/**
 * @brief Move the cursor to a specific position
 * @param x Column (0-based)
 * @param y Row (0-based)
 */
void move_cursor(uint16_t x, uint16_t y);

/** @} */

}
