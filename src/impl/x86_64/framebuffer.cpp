#include "framebuffer.h"
#include "util.h"

extern "C" {
    extern uint8_t font_bitmap_start;
    extern uint8_t font_bitmap_end;
}

namespace framebuffer {

struct FramebufferInfo {
    uint8_t* addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  type;
    struct {
        uint8_t r_pos, r_size;
        uint8_t g_pos, g_size;
        uint8_t b_pos, b_size;
    } rgb;
};

static FramebufferInfo fb;
static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static bool cursor_visible = true;

static void (*putpixel_raw)(uint32_t x, uint32_t y, uint32_t color) = nullptr;

uint32_t pack_color(uint8_t r, uint8_t g, uint8_t b) {
    if (fb.type == 2) { // MULTIBOOT_FRAMEBUFFER_TYPE_EGA
        return 0x0F; // White text attribute
    }
    if (fb.type == 0) { // MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED
        return (r > 128) ? 0x0F : 0x00; 
    }
    return ((uint32_t)r >> (8 - fb.rgb.r_size) << fb.rgb.r_pos) |
           ((uint32_t)g >> (8 - fb.rgb.g_size) << fb.rgb.g_pos) |
           ((uint32_t)b >> (8 - fb.rgb.b_size) << fb.rgb.b_pos);
}

static void update_cursor_visual(bool show) {
    if (!cursor_visible) return;
    if (fb.type == 2) { // EGA
        uint16_t* buf = (uint16_t*)fb.addr;
        buf[cursor_y * fb.width + cursor_x] ^= 0x7700; // XOR Attribute Flip
    } else {
        uint32_t color = show ? pack_color(255, 255, 255) : pack_color(0, 0, 0);
        for (uint32_t i = 0; i < 8; i++) {
            putpixel_raw(cursor_x + i, cursor_y + 14, color);
            putpixel_raw(cursor_x + i, cursor_y + 15, color);
        }
    }
}

// --- Mode Specific Implementations ---

static void putpixel_rgb(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb.width || y >= fb.height) return;
    uint8_t* pixel = fb.addr + (y * fb.pitch) + (x * (fb.bpp / 8));
    if (fb.bpp == 32) *(uint32_t*)pixel = color;
    else if (fb.bpp == 24) {
        pixel[0] = color & 0xFF; pixel[1] = (color >> 8) & 0xFF; pixel[2] = (color >> 16) & 0xFF;
    } else if (fb.bpp == 16) *(uint16_t*)pixel = (uint16_t)color;
}

static void putpixel_indexed(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb.width || y >= fb.height) return;
    fb.addr[y * fb.pitch + x] = (uint8_t)color;
}

static void putpixel_ega(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb.width || y >= fb.height) return;
    ((uint16_t*)fb.addr)[y * fb.width + x] = (uint16_t)color;
}

void scroll() {
    update_cursor_visual(false);
    if (fb.type == 2) { // EGA
        for (uint32_t y = 0; y < fb.height - 1; y++) {
            for (uint32_t x = 0; x < fb.width; x++) {
                ((uint16_t*)fb.addr)[y * fb.width + x] = ((uint16_t*)fb.addr)[(y + 1) * fb.width + x];
            }
        }
        for (uint32_t x = 0; x < fb.width; x++) putpixel_ega(x, fb.height - 1, 0x0720);
    } else {
        uint32_t font_h = 16;
        for (uint32_t y = 0; y < fb.height - font_h; y++) {
            uint8_t* dest = fb.addr + (y * fb.pitch);
            uint8_t* src = fb.addr + ((y + font_h) * fb.pitch);
            for (uint32_t i = 0; i < fb.pitch; i++) dest[i] = src[i];
        }
        uint32_t bg = pack_color(0, 0, 0);
        for (uint32_t y = fb.height - font_h; y < fb.height; y++) {
            for (uint32_t x = 0; x < fb.width; x++) putpixel_raw(x, y, bg);
        }
    }
}

// --- API ---

void init(multiboot_tag_framebuffer* tag) {
    if (!tag) return;
    fb.addr = (uint8_t*)p2v(tag->addr);
    fb.pitch = tag->pitch; fb.width = tag->width; fb.height = tag->height;
    fb.bpp = tag->bpp; fb.type = tag->framebuffer_type;

    switch (fb.type) {
        case MULTIBOOT_FRAMEBUFFER_TYPE_RGB:
            fb.rgb.r_pos = tag->rgb.red_field_position; fb.rgb.r_size = tag->rgb.red_mask_size;
            fb.rgb.g_pos = tag->rgb.green_field_position; fb.rgb.g_size = tag->rgb.green_mask_size;
            fb.rgb.b_pos = tag->rgb.blue_field_position; fb.rgb.b_size = tag->rgb.blue_mask_size;
            putpixel_raw = putpixel_rgb; break;
        case MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED: putpixel_raw = putpixel_indexed; break;
        case MULTIBOOT_FRAMEBUFFER_TYPE_EGA: putpixel_raw = putpixel_ega; break;
    }
    clear(pack_color(0, 0, 0));
}

void putchar(char c) {
    update_cursor_visual(false);

    const uint32_t font_h = 16;
    const uint32_t font_w = 8;
    
    uint8_t* font_start = &font_bitmap_start;
    uint8_t* font_end = &font_bitmap_end;

    size_t font_blob_size = (size_t)(font_end - font_start);
    uint32_t max_chars = font_blob_size / font_h;

    if (c == '\n') {
        cursor_x = 0;
        cursor_y += (fb.type == 2) ? 1 : font_h;
    } 
    else if (c == '\r') {
        cursor_x = 0;
    } 
    else if (c == '\t') {
        cursor_x += (fb.type == 2) ? 4 : (font_w * 4);
    }
    else {
        if (cursor_x + font_w > fb.width) {
            cursor_x = 0;
            cursor_y += (fb.type == 2) ? 1 : font_h;
        }

        if (fb.type == 2) { // EGA Text Mode
            uint16_t attr = 0x0F00; // White on Black
            putpixel_ega(cursor_x, cursor_y, attr | (uint8_t)c);
            cursor_x++;
        } 
        else { // RGB / Indexed Graphics Modes
            uint32_t fg = pack_color(255, 255, 255);
            uint32_t bg = pack_color(0, 0, 0);
            uint8_t index = (uint8_t)c;

            if (index < max_chars) {
                uint8_t* char_data = &font_start[index * font_h];
                for (uint32_t r = 0; r < font_h; r++) {
                    uint8_t row_byte = char_data[r];
                    for (uint32_t col = 0; col < font_w; col++) {
                        uint32_t color = (row_byte & (0x80 >> col)) ? fg : bg;
                        putpixel_raw(cursor_x + col, cursor_y + r, color);
                    }
                }
            } else {
                for (uint32_t r = 0; r < font_h; r++) {
                    for (uint32_t col = 0; col < font_w; col++) {
                        bool is_border = (r == 0 || r == font_h - 1 || col == 0 || col == font_w - 1);
                        putpixel_raw(cursor_x + col, cursor_y + r, is_border ? fg : bg);
                    }
                }
            }
            cursor_x += font_w;
        }
    }

    uint32_t line_step = (fb.type == 2) ? 1 : font_h;
    while (cursor_y + line_step > fb.height) {
        scroll();
        cursor_y -= line_step;
    }

    update_cursor_visual(true);
}

void clear(uint32_t color) {
    if (fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_EGA) {
        uint16_t clear_val = (uint16_t)((0x07 << 8) | ' ');
        for (uint32_t i = 0; i < fb.width * fb.height; i++) ((uint16_t*)fb.addr)[i] = clear_val;
    } else {
        for (uint32_t y = 0; y < fb.height; y++) {
            for (uint32_t x = 0; x < fb.width; x++) putpixel_raw(x, y, color);
        }
    }
    cursor_x = 0; cursor_y = 0;
    update_cursor_visual(true);
}

void set_cursor_enabled(bool enabled) {
    update_cursor_visual(false);
    cursor_visible = enabled;
    update_cursor_visual(true);
}

void move_cursor(uint16_t x, uint16_t y) {
    update_cursor_visual(false); // Remove from old spot
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

} // namespace framebuffer