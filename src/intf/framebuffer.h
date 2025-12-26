#pragma once
#include <stdint.h>

#include "multiboot2.h"

namespace framebuffer {

    /**
     * @brief Initialize the framebuffer
     *
     * Sets up the framebuffer console using information provided by Multiboot2.
     *
     * @param fb_tag Pointer to the Multiboot2 framebuffer tag
     */
    void init(multiboot_tag_framebuffer* fb_tag);

    /**
     * @brief Write a single character to the framebuffer
     *
     * @param c Character to write
     */
    void putchar(char c);

    /**
     * @brief Clear the framebuffer with a specific color
     *
     * @param color 32-bit ARGB color value to fill the screen with (default: black 0x000000)
     */
    void clear();

    /**
     * @brief Draw a single pixel at a specific location
     *
     * @param x X coordinate (0-based)
     * @param y Y coordinate (0-based)
     * @param color 32-bit ARGB color value of the pixel
     */
    void putpixel(uint32_t x, uint32_t y, uint32_t color);

    /**
     * @brief Move the cursor to a specific position on the screen
     *
     * @param x Column (0-based)
     * @param y Row (0-based)
     */
    void move_cursor(uint16_t x, uint16_t y);

    /**
     * @brief Enable the cursor
     *
     * Makes the cursor visible.
     */
    void enable_cursor();

    /**
     * @brief Disable the cursor
     *
     * Hides the cursor.
     */
    void disable_cursor();

    void set_color(uint8_t fg, uint8_t bg);

}  // namespace framebuffer