#pragma once
#include <stdint.h>
#include "multiboot2.h"

namespace framebuffer {

/**
 * @brief Initialize the framebuffer with data from Multiboot2
 */
void init(multiboot_tag_framebuffer* fb_tag);

void putchar(char c);
void clear(uint32_t color = 0x000000);
void putpixel(uint32_t x, uint32_t y, uint32_t color);
void move_cursor(uint16_t x, uint16_t y);
void enable_cursor();
void disable_cursor();

} // namespace framebuffer