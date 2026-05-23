/**
 * @file framebuffer.cpp
 * @brief Implementation of the framebuffer console backend
 *
 * Provides support for writing characters, pixels, and managing the cursor
 * in VGA, indexed, and RGB framebuffer modes. Handles scrolling, cursor
 * drawing, and font rendering.
 */

#include "framebuffer.h"

#include "string.h"
#include "memory/vmm.h"
#include "util.h"

extern "C" {
extern uint8_t font_bitmap_start;  ///< Start of embedded font bitmap
extern uint8_t font_bitmap_end;    ///< End of embedded font bitmap
}

#ifndef KERNEL_FONT_WIDTH
#define KERNEL_FONT_WIDTH 8
#endif
#ifndef KERNEL_FONT_HEIGHT
#define KERNEL_FONT_HEIGHT 12
#endif

static_assert(KERNEL_FONT_WIDTH >= 1 && KERNEL_FONT_WIDTH <= 32, "bad font width");
static_assert(KERNEL_FONT_HEIGHT >= 1 && KERNEL_FONT_HEIGHT <= 64, "bad font height");

namespace framebuffer {
    static volatile uint32_t current_fg = 0xFFFFFFFF;  // White
    static volatile uint32_t current_bg = 0x00000000;  // Black
    static volatile uint8_t current_ega_attr = 0x0F;   // White on Black

    static uint32_t palette_rgb[] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
                                     0xAA5500, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
                                     0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

    /**
     * @internal
     * Stores information about the active framebuffer
     */
    struct FramebufferInfo {
        uint8_t *addr;    ///< Framebuffer base address
        uint64_t phys;     ///< Physical framebuffer base
        uint64_t bytes;    ///< Mapped framebuffer bytes
        uint32_t pitch;   ///< Bytes per row
        uint32_t width;   ///< Screen width in pixels
        uint32_t height;  ///< Screen height in pixels
        uint8_t bpp;      ///< Bits per pixel
        uint8_t type;     ///< Framebuffer type (RGB, indexed, EGA)
        struct {
            uint8_t r_pos, r_size;
            uint8_t g_pos, g_size;
            uint8_t b_pos, b_size;
        } rgb;  ///< RGB bit positions and sizes
    };

    /// Active framebuffer info
    static FramebufferInfo fb;
    /// Cursor coordinates and visibility state
    static uint32_t cursor_x = 0;       ///< Cursor X
    static uint32_t cursor_y = 0;       ///< Cursor Y
    static bool cursor_visible = true;  ///< Cursor Visible

    static constexpr uint32_t font_w = KERNEL_FONT_WIDTH;
    static constexpr uint32_t font_h = KERNEL_FONT_HEIGHT;
    static constexpr uint32_t font_row_bytes = (font_w + 7) / 8;
    static constexpr uint32_t font_glyph_bytes = font_row_bytes * font_h;

    /// Function pointer for mode-specific putpixel
    static void (*putpixel_raw)(uint32_t x, uint32_t y, uint32_t color) = nullptr;

    static uint32_t cell_w() { return (fb.type == 2) ? 1 : font_w; }
    static uint32_t cell_h() { return (fb.type == 2) ? 1 : font_h; }

    static uint32_t font_glyph_count() {
        uint8_t *font_start = &font_bitmap_start;
        uint8_t *font_end = &font_bitmap_end;
        size_t font_blob_size = (size_t)(font_end - font_start);
        if (font_glyph_bytes == 0 || (font_blob_size % font_glyph_bytes) != 0) return 0;
        return font_blob_size / font_glyph_bytes;
    }

    static bool font_bit_set(uint8_t *glyph, uint32_t row, uint32_t col) {
        uint8_t byte = glyph[row * font_row_bytes + (col >> 3)];
        return (byte & (0x80 >> (col & 7))) != 0;
    }

    /**
     * @internal
     * Pack RGB values into the framebuffer's native color format
     *
     * @param r Red component (0-255)
     * @param g Green component (0-255)
     * @param b Blue component (0-255)
     * @return Packed framebuffer color
     */
    uint32_t pack_color(uint8_t r, uint8_t g, uint8_t b) {
        if (fb.type == 2) {  // MULTIBOOT_FRAMEBUFFER_TYPE_EGA
            return current_ega_attr;
        }
        if (fb.type == 0) {  // MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED
            return (r > 128) ? 0x0F : 0x00;
        }
        return ((uint32_t)r >> (8 - fb.rgb.r_size) << fb.rgb.r_pos) |
               ((uint32_t)g >> (8 - fb.rgb.g_size) << fb.rgb.g_pos) |
               ((uint32_t)b >> (8 - fb.rgb.b_size) << fb.rgb.b_pos);
    }

    /**
     * @internal
     * Update the visual representation of the cursor
     *
     * @param show If true, draw the cursor; otherwise hide it
     */
    static void update_cursor_visual(bool show) {
        if (!cursor_visible) return;
        if (fb.type == 2) {  // EGA
            uint16_t *buf = (uint16_t *)fb.addr;
            buf[cursor_y * fb.width + cursor_x] ^= 0x7700;  // XOR Attribute Flip
        } else {
            uint32_t color = show ? pack_color(255, 255, 255) : pack_color(0, 0, 0);
            uint32_t underline_h = font_h >= 12 ? 2 : 1;
            uint32_t y0 = cursor_y + font_h - underline_h;
            for (uint32_t y = y0; y < cursor_y + font_h; y++) {
                for (uint32_t x = 0; x < font_w; x++) putpixel_raw(cursor_x + x, y, color);
            }
        }
    }

    static void putpixel_rgb(uint32_t x, uint32_t y, uint32_t color) {
        if (x >= fb.width || y >= fb.height) return;
        uint8_t *pixel = fb.addr + (y * fb.pitch) + (x * (fb.bpp / 8));
        if (fb.bpp == 32)
            *(uint32_t *)pixel = color;
        else if (fb.bpp == 24) {
            pixel[0] = color & 0xFF;
            pixel[1] = (color >> 8) & 0xFF;
            pixel[2] = (color >> 16) & 0xFF;
        } else if (fb.bpp == 16)
            *(uint16_t *)pixel = (uint16_t)color;
    }

    static void putpixel_indexed(uint32_t x, uint32_t y, uint32_t color) {
        if (x >= fb.width || y >= fb.height) return;
        fb.addr[y * fb.pitch + x] = (uint8_t)color;
    }

    static void putpixel_ega(uint32_t x, uint32_t y, uint32_t color) {
        if (x >= fb.width || y >= fb.height) return;
        ((uint16_t *)fb.addr)[y * fb.width + x] = (uint16_t)color;
    }

    /**
     * @internal
     * Scroll the framebuffer content up by one line
     */
    void scroll() {
        update_cursor_visual(false);

        if (fb.type == 2) {  // EGA / VGA Text Mode
            uint32_t total_cells = (fb.height - 1) * fb.width;
            memcpy(fb.addr, fb.addr + (fb.width * 2), total_cells * 2);

            uint16_t clear_val = (uint16_t)(current_ega_attr << 8) | ' ';
            uint16_t *last_line = ((uint16_t *)fb.addr) + total_cells;
            for (uint32_t x = 0; x < fb.width; x++) last_line[x] = clear_val;
        } else {  // RGB / Indexed Graphics Modes
            uint32_t bytes_per_row = fb.pitch;

            uint8_t *dest = fb.addr;
            uint8_t *src = fb.addr + (font_h * bytes_per_row);
            size_t bytes_to_copy = (fb.height - font_h) * bytes_per_row;

            memcpy(dest, src, bytes_to_copy);

            uint8_t *bottom_start = fb.addr + bytes_to_copy;
            size_t bottom_size = font_h * bytes_per_row;

            if (current_bg == 0) {
                memset(bottom_start, 0, bottom_size);
            } else {
                for (uint32_t x = 0; x < fb.width; x++)
                    putpixel_raw(x, fb.height - font_h, current_bg);
                for (uint32_t y = 1; y < font_h; y++) {
                    memcpy(fb.addr + (fb.height - font_h + y) * fb.pitch, bottom_start, fb.pitch);
                }
            }
        }
    }

    void init(multiboot_tag_framebuffer *tag) {
        if (!tag) return;
        fb.phys = tag->addr;
        fb.bytes = (uint64_t)tag->pitch * (uint64_t)tag->height;
        fb.addr = (uint8_t *)p2v(tag->addr);
        fb.pitch = tag->pitch;
        fb.width = tag->width;
        fb.height = tag->height;
        fb.bpp = tag->bpp;
        fb.type = tag->framebuffer_type;

        switch (fb.type) {
            case MULTIBOOT_FRAMEBUFFER_TYPE_RGB:
                fb.rgb.r_pos = tag->rgb.red_field_position;
                fb.rgb.r_size = tag->rgb.red_mask_size;
                fb.rgb.g_pos = tag->rgb.green_field_position;
                fb.rgb.g_size = tag->rgb.green_mask_size;
                fb.rgb.b_pos = tag->rgb.blue_field_position;
                fb.rgb.b_size = tag->rgb.blue_mask_size;
                putpixel_raw = putpixel_rgb;
                break;
            case MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED:
                putpixel_raw = putpixel_indexed;
                break;
            case MULTIBOOT_FRAMEBUFFER_TYPE_EGA:
                putpixel_raw = putpixel_ega;
                break;
        }

        if (fb.type != MULTIBOOT_FRAMEBUFFER_TYPE_EGA && font_glyph_count() < 128) {
            kpanic("framebuffer: invalid font bitmap");
        }

        clear();
    }


    void remap_wc() {
        if (!fb.phys || !fb.bytes || fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_EGA) return;
        void *mapped = vmm::mmio_map_wc(fb.phys, fb.bytes);
        if (mapped) fb.addr = (uint8_t *)mapped;
    }

    /**
     * @internal
     * Draw a character at the current cursor position, handling
     * newline, carriage return, tab, and wrapping. Calls scroll
     * if necessary.
     */
    void putchar(char c) {
        update_cursor_visual(false);

        uint8_t *font_start = &font_bitmap_start;
        uint32_t max_chars = font_glyph_count();

        if (c == '\n') {
            cursor_x = 0;
            cursor_y += cell_h();
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\t') {
            cursor_x += cell_w() * 4;
        } else {
            if (cursor_x + cell_w() > fb.width) {
                cursor_x = 0;
                cursor_y += cell_h();
            }

            if (fb.type == 2) {  // EGA Text Mode
                uint16_t data = (uint16_t)(current_ega_attr << 8) | (uint8_t)c;
                putpixel_ega(cursor_x, cursor_y, data);
                cursor_x++;
            } else {  // RGB / Indexed Graphics Modes
                uint8_t index = (uint8_t)c;

                if (index < max_chars) {
                    uint8_t *char_data = &font_start[index * font_glyph_bytes];
                    for (uint32_t r = 0; r < font_h; r++) {
                        for (uint32_t col = 0; col < font_w; col++) {
                            uint32_t color =
                                font_bit_set(char_data, r, col) ? current_fg : current_bg;
                            putpixel_raw(cursor_x + col, cursor_y + r, color);
                        }
                    }
                } else {
                    for (uint32_t r = 0; r < font_h; r++) {
                        for (uint32_t col = 0; col < font_w; col++) {
                            bool is_border =
                                (r == 0 || r == font_h - 1 || col == 0 || col == font_w - 1);
                            putpixel_raw(cursor_x + col, cursor_y + r,
                                         is_border ? current_fg : current_bg);
                        }
                    }
                }
                cursor_x += cell_w();
            }
        }

        uint32_t line_step = cell_h();
        while (cursor_y + line_step > fb.height) {
            scroll();
            cursor_y -= line_step;
        }

        update_cursor_visual(true);
    }

    void backspace() {
        update_cursor_visual(false);

        uint32_t step_x = cell_w();
        uint32_t step_y = cell_h();

        if (cursor_x >= step_x) {
            cursor_x -= step_x;
        } else if (cursor_y >= step_y) {
            cursor_y -= step_y;
            cursor_x = (fb.width / step_x) * step_x - step_x;
        } else {
            update_cursor_visual(true);
            return;
        }

        // Overwrite the character area with the background color
        if (fb.type == 2) {  // EGA Text Mode
            uint16_t data = (uint16_t)(current_ega_attr << 8) | ' ';
            putpixel_ega(cursor_x, cursor_y, data);
        } else {  // Graphics Modes
            for (uint32_t r = 0; r < font_h; r++) {
                for (uint32_t col = 0; col < font_w; col++) {
                    putpixel_raw(cursor_x + col, cursor_y + r, current_bg);
                }
            }
        }

        update_cursor_visual(true);
    }

    /**
     * @internal
     * Clear the framebuffer with a specified color
     *
     * @param color ARGB color to fill the screen with
     */
    void clear() {
        if (fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_EGA) {
            uint16_t clear_val = (uint16_t)(current_ega_attr << 8) | ' ';
            for (uint32_t i = 0; i < fb.width * fb.height; i++)
                ((uint16_t *)fb.addr)[i] = clear_val;
        } else {
            for (uint32_t y = 0; y < fb.height; y++) {
                for (uint32_t x = 0; x < fb.width; x++) putpixel_raw(x, y, current_bg);
            }
        }
        cursor_x = 0;
        cursor_y = 0;
        update_cursor_visual(true);
    }

    /**
     * @internal
     * Set cursor enabled/disabled state
     *
     * @param enabled true to enable cursor, false to disable
     */
    void set_cursor_enabled(bool enabled) {
        update_cursor_visual(false);
        cursor_visible = enabled;
        update_cursor_visual(true);
    }

    void move_cursor(uint16_t x, uint16_t y) {
        update_cursor_visual(false);  // Remove from old spot
        cursor_x = x;
        cursor_y = y;
        update_cursor_visual(true);  // Draw in new spot
    }

    void enable_cursor() {
        cursor_visible = true;
        update_cursor_visual(true);
    }

    void disable_cursor() {
        update_cursor_visual(false);
        cursor_visible = false;
    }

    void set_color(uint8_t fg_idx, uint8_t bg_idx) {
        uint32_t raw_fg = palette_rgb[fg_idx];
        uint32_t raw_bg = palette_rgb[bg_idx];
        current_fg = pack_color((raw_fg >> 16) & 0xFF, (raw_fg >> 8) & 0xFF, raw_fg & 0xFF);
        current_bg = pack_color((raw_bg >> 16) & 0xFF, (raw_bg >> 8) & 0xFF, raw_bg & 0xFF);

        current_ega_attr = (bg_idx << 4) | (fg_idx & 0x0F);
    }
}  // namespace framebuffer