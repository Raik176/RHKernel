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
#include "memory/heap.h"
#include "smp/lock.h"
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
    static uint32_t current_fg = 0xFFFFFFFF;
    static uint32_t current_bg = 0x00000000;
    static uint8_t current_ega_attr = 0x0F;
    static uint32_t glyph_count = 0;
    static spinlock_t framebuffer_lock;
    static uint64_t fb_generation = 1;
    static bool console_claimed = false;
    static uint64_t console_claim_owner = 0;

    struct TextCell {
        uint32_t fg;
        uint32_t bg;
        uint16_t ega_attr;
        uint8_t ch;
        uint8_t valid;
    };

    static TextCell *text_cells = nullptr;
    static uint32_t text_cell_cols = 0;
    static uint32_t text_cell_rows = 0;
    static uint64_t text_cell_valid = 0;

    static constexpr uint32_t palette_rgb[] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
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

    static void clear_locked();
    static void store_text_cell(uint32_t px, uint32_t py, uint8_t ch);

    static inline uint32_t cell_w() { return (fb.type == 2) ? 1 : font_w; }
    static inline uint32_t cell_h() { return (fb.type == 2) ? 1 : font_h; }
    static inline uint32_t text_cols() { return fb.width / cell_w(); }
    static inline uint32_t text_rows() { return fb.height / cell_h(); }
    static inline uint32_t usable_width() { return text_cols() * cell_w(); }
    static inline uint32_t usable_height() { return text_rows() * cell_h(); }

    static uint32_t font_glyph_count() {
        uint8_t *font_start = &font_bitmap_start;
        uint8_t *font_end = &font_bitmap_end;
        size_t font_blob_size = (size_t)(font_end - font_start);
        if (font_glyph_bytes == 0 || (font_blob_size % font_glyph_bytes) != 0) return 0;
        return font_blob_size / font_glyph_bytes;
    }

    static inline bool font_bit_set(const uint8_t *glyph, uint32_t row, uint32_t col) {
        uint8_t byte = glyph[row * font_row_bytes + (col >> 3)];
        return (byte & (0x80 >> (col & 7))) != 0;
    }

    static inline uint32_t bytes_per_pixel() { return fb.bpp >> 3; }

    static inline uint8_t *draw_base() { return fb.addr; }

    static bool rgb_layout_valid(const multiboot_tag_framebuffer *tag) {
        const uint8_t bpp = tag->bpp;
        const uint8_t r_pos = tag->rgb.red_field_position;
        const uint8_t r_size = tag->rgb.red_mask_size;
        const uint8_t g_pos = tag->rgb.green_field_position;
        const uint8_t g_size = tag->rgb.green_mask_size;
        const uint8_t b_pos = tag->rgb.blue_field_position;
        const uint8_t b_size = tag->rgb.blue_mask_size;
        return r_size && r_size <= 8 && g_size && g_size <= 8 && b_size && b_size <= 8 && r_pos < bpp &&
               g_pos < bpp && b_pos < bpp && r_pos + r_size <= bpp && g_pos + g_size <= bpp &&
               b_pos + b_size <= bpp;
    }

    static void validate_tag(const multiboot_tag_framebuffer *tag) {
        if (tag->width == 0 || tag->height == 0 || tag->pitch == 0) kpanic("framebuffer: empty mode");

        switch (tag->framebuffer_type) {
            case MULTIBOOT_FRAMEBUFFER_TYPE_RGB: {
                if (tag->bpp != 16 && tag->bpp != 24 && tag->bpp != 32) kpanic("framebuffer: unsupported rgb bpp");
                uint64_t min_pitch = (uint64_t)tag->width * (tag->bpp >> 3);
                if ((uint64_t)tag->pitch < min_pitch || !rgb_layout_valid(tag)) kpanic("framebuffer: bad rgb mode");
                break;
            }
            case MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED:
                if (tag->bpp != 8 || tag->pitch < tag->width) kpanic("framebuffer: bad indexed mode");
                break;
            case MULTIBOOT_FRAMEBUFFER_TYPE_EGA:
                if (tag->bpp != 16) kpanic("framebuffer: bad ega mode");
                break;
            default:
                kpanic("framebuffer: unsupported mode");
        }
    }

    static inline uint8_t *pixel_addr(uint32_t x, uint32_t y) {
        return draw_base() + (uint64_t)y * fb.pitch + (uint64_t)x * bytes_per_pixel();
    }

    static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
        if (x >= fb.width || y >= fb.height) return;
        if (w > fb.width - x) w = fb.width - x;
        if (h > fb.height - y) h = fb.height - y;
        if (w == 0 || h == 0) return;

        if (fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_EGA) {
            uint16_t value = (uint16_t)color;
            for (uint32_t row = 0; row < h; row++) {
                uint16_t *dst = ((uint16_t *)fb.addr) + (uint64_t)(y + row) * fb.width + x;
                for (uint32_t col = 0; col < w; col++) dst[col] = value;
            }
            return;
        }

        if (fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED) {
            for (uint32_t row = 0; row < h; row++) {
                memset(draw_base() + (uint64_t)(y + row) * fb.pitch + x, (uint8_t)color, w);
            }
            return;
        }

        if (fb.bpp == 32) {
            for (uint32_t row = 0; row < h; row++) {
                uint32_t *dst = (uint32_t *)pixel_addr(x, y + row);
                for (uint32_t col = 0; col < w; col++) dst[col] = color;
            }
        } else if (fb.bpp == 24) {
            uint8_t b = color & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t r = (color >> 16) & 0xFF;
            for (uint32_t row = 0; row < h; row++) {
                uint8_t *dst = pixel_addr(x, y + row);
                for (uint32_t col = 0; col < w; col++) {
                    dst[0] = b;
                    dst[1] = g;
                    dst[2] = r;
                    dst += 3;
                }
            }
        } else if (fb.bpp == 16) {
            uint16_t value = (uint16_t)color;
            for (uint32_t row = 0; row < h; row++) {
                uint16_t *dst = (uint16_t *)pixel_addr(x, y + row);
                for (uint32_t col = 0; col < w; col++) dst[col] = value;
            }
        }
    }

    static void draw_glyph(uint32_t x, uint32_t y, const uint8_t *glyph) {
        if (fb.bpp == 32 && fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_RGB) {
            for (uint32_t row = 0; row < font_h; row++) {
                uint32_t *dst = (uint32_t *)pixel_addr(x, y + row);
                for (uint32_t col = 0; col < font_w; col++) {
                    dst[col] = font_bit_set(glyph, row, col) ? current_fg : current_bg;
                }
            }
            return;
        }

        if (fb.bpp == 16 && fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_RGB) {
            uint16_t fg = (uint16_t)current_fg;
            uint16_t bg = (uint16_t)current_bg;
            for (uint32_t row = 0; row < font_h; row++) {
                uint16_t *dst = (uint16_t *)pixel_addr(x, y + row);
                for (uint32_t col = 0; col < font_w; col++) {
                    dst[col] = font_bit_set(glyph, row, col) ? fg : bg;
                }
            }
            return;
        }

        for (uint32_t row = 0; row < font_h; row++) {
            for (uint32_t col = 0; col < font_w; col++) {
                putpixel_raw(x + col, y + row, font_bit_set(glyph, row, col) ? current_fg : current_bg);
            }
        }
    }

    static void draw_missing_glyph(uint32_t x, uint32_t y) {
        fill_rect(x, y, font_w, font_h, current_bg);
        fill_rect(x, y, font_w, 1, current_fg);
        fill_rect(x, y + font_h - 1, font_w, 1, current_fg);
        fill_rect(x, y, 1, font_h, current_fg);
        fill_rect(x + font_w - 1, y, 1, font_h, current_fg);
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
        if (!cursor_visible || text_cols() == 0 || text_rows() == 0) return;

        uint32_t x = cursor_x;
        uint32_t y = cursor_y;
        uint32_t max_x = usable_width() - cell_w();
        uint32_t max_y = usable_height() - cell_h();
        if (x > max_x) x = max_x;
        if (y > max_y) y = max_y;

        if (fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_EGA) {
            uint16_t *buf = (uint16_t *)fb.addr;
            buf[(uint64_t)y * fb.width + x] ^= 0x7700;
        } else {
            uint32_t underline_h = font_h >= 12 ? 2 : 1;
            uint32_t y0 = y + font_h - underline_h;
            fill_rect(x, y0, font_w, underline_h, show ? current_fg : current_bg);
        }
    }

    static void putpixel_rgb(uint32_t x, uint32_t y, uint32_t color) {
        if (x >= fb.width || y >= fb.height) return;
        uint8_t *pixel = pixel_addr(x, y);
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
        draw_base()[(uint64_t)y * fb.pitch + x] = (uint8_t)color;
    }

    static void putpixel_ega(uint32_t x, uint32_t y, uint32_t color) {
        if (x >= fb.width || y >= fb.height) return;
        ((uint16_t *)fb.addr)[y * fb.width + x] = (uint16_t)color;
    }

    static bool text_cells_active() {
        return text_cells && text_cell_cols == text_cols() && text_cell_rows == text_rows();
    }

    static uint64_t text_cell_count() {
        return (uint64_t)text_cell_cols * text_cell_rows;
    }

    static bool text_cells_complete() {
        return text_cells_active() && text_cell_valid == text_cell_count();
    }

    static void set_text_cell(uint32_t col, uint32_t row, uint8_t ch) {
        if (!text_cells_active() || col >= text_cell_cols || row >= text_cell_rows) return;
        TextCell *cell = &text_cells[(uint64_t)row * text_cell_cols + col];
        if (!cell->valid) text_cell_valid++;
        cell->fg = current_fg;
        cell->bg = current_bg;
        cell->ega_attr = current_ega_attr;
        cell->ch = ch;
        cell->valid = 1;
    }

    static void store_text_cell(uint32_t px, uint32_t py, uint8_t ch) {
        uint32_t cw = cell_w();
        uint32_t ch_h = cell_h();
        if (cw == 0 || ch_h == 0) return;
        set_text_cell(px / cw, py / ch_h, ch);
    }

    static void blank_text_cell(TextCell *cell) {
        if (!cell->valid) text_cell_valid++;
        cell->fg = current_fg;
        cell->bg = current_bg;
        cell->ega_attr = current_ega_attr;
        cell->ch = ' ';
        cell->valid = 1;
    }

    static void draw_cell(uint32_t col, uint32_t row, const TextCell &cell) {
        if (fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_EGA) return;
        uint32_t old_fg = current_fg;
        uint32_t old_bg = current_bg;
        uint8_t old_attr = current_ega_attr;
        current_fg = cell.fg;
        current_bg = cell.bg;
        current_ega_attr = (uint8_t)cell.ega_attr;

        uint8_t *font_start = &font_bitmap_start;
        uint32_t x = col * font_w;
        uint32_t y = row * font_h;
        if (cell.ch < glyph_count) {
            draw_glyph(x, y, &font_start[(uint32_t)cell.ch * font_glyph_bytes]);
        } else {
            draw_missing_glyph(x, y);
        }

        current_fg = old_fg;
        current_bg = old_bg;
        current_ega_attr = old_attr;
    }

    static void redraw_text_cells() {
        if (!text_cells_active()) return;
        for (uint32_t row = 0; row < text_cell_rows; row++) {
            for (uint32_t col = 0; col < text_cell_cols; col++) {
                draw_cell(col, row, text_cells[(uint64_t)row * text_cell_cols + col]);
            }
        }
    }

    static void reset_text_cells() {
        if (!text_cells_active()) return;
        text_cell_valid = 0;
        uint64_t count = text_cell_count();
        for (uint64_t i = 0; i < count; i++) {
            text_cells[i].valid = 0;
            blank_text_cell(&text_cells[i]);
        }
    }

    static void ensure_text_cells() {
        if (fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_EGA || text_cells_active()) return;
        uint32_t cols = text_cols();
        uint32_t rows = text_rows();
        if (cols == 0 || rows == 0) return;
        uint64_t count = (uint64_t)cols * rows;
        if (count > ((uint64_t)~(size_t)0) / sizeof(TextCell)) return;
        TextCell *cells = (TextCell *)heap::kmalloc((size_t)(count * sizeof(TextCell)));
        if (!cells) return;
        memset(cells, 0, (size_t)(count * sizeof(TextCell)));
        text_cells = cells;
        text_cell_cols = cols;
        text_cell_rows = rows;
        text_cell_valid = 0;
    }

    /**
     * @internal
     * Scroll the framebuffer content up by one line
     */
    static void scroll_locked() {
        update_cursor_visual(false);

        uint32_t rows = text_rows();
        if (rows == 0) return;

        if (fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_EGA) {
            uint16_t *cells = (uint16_t *)fb.addr;
            uint64_t total_cells = (uint64_t)(rows - 1) * fb.width;
            memmove(cells, cells + fb.width, (size_t)(total_cells * sizeof(uint16_t)));

            uint16_t clear_val = (uint16_t)(current_ega_attr << 8) | ' ';
            uint16_t *last_line = cells + total_cells;
            for (uint32_t x = 0; x < fb.width; x++) last_line[x] = clear_val;
            return;
        }

        bool can_redraw = text_cells_complete();
        if (text_cells_active()) {
            uint64_t row_bytes = (uint64_t)text_cell_cols * sizeof(TextCell);
            memmove(text_cells, text_cells + text_cell_cols,
                    (size_t)((uint64_t)(text_cell_rows - 1) * row_bytes));
            text_cell_valid = 0;
            uint64_t kept = (uint64_t)(text_cell_rows - 1) * text_cell_cols;
            for (uint64_t i = 0; i < kept; i++) {
                if (text_cells[i].valid) text_cell_valid++;
            }
            TextCell *last = text_cells + (uint64_t)(text_cell_rows - 1) * text_cell_cols;
            for (uint32_t col = 0; col < text_cell_cols; col++) {
                last[col].valid = 0;
                blank_text_cell(&last[col]);
            }
            if (can_redraw) {
                redraw_text_cells();
                return;
            }
        }

        uint32_t bytes_per_row = fb.pitch;
        uint32_t step = font_h;
        uint32_t visible_h = usable_height();
        uint64_t bytes_to_copy = (uint64_t)(visible_h - step) * bytes_per_row;
        uint64_t clear_bytes = (uint64_t)step * bytes_per_row;

        uint8_t *base = draw_base();
        memmove(base, base + (uint64_t)step * bytes_per_row, (size_t)bytes_to_copy);

        if (current_bg == 0) {
            memset(base + bytes_to_copy, 0, (size_t)clear_bytes);
        } else {
            fill_rect(0, visible_h - step, fb.width, step, current_bg);
        }

    }

    void scroll() {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        scroll_locked();
        framebuffer_lock.release(flags);
    }

    static void advance_line() {
        cursor_x = 0;
        uint32_t rows = text_rows();
        if (rows == 0) return;

        uint32_t step = cell_h();
        uint32_t last_y = (rows - 1) * step;
        if (cursor_y >= last_y) {
            scroll_locked();
            cursor_y = last_y;
        } else {
            cursor_y += step;
        }
    }

    void init(multiboot_tag_framebuffer *tag) {
        if (!tag) return;
        validate_tag(tag);

        uint64_t flags;
        framebuffer_lock.acquire(flags);
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

        glyph_count = font_glyph_count();
        if (fb.type != MULTIBOOT_FRAMEBUFFER_TYPE_EGA && glyph_count < 128) {
            framebuffer_lock.release(flags);
            kpanic("framebuffer: invalid font bitmap");
        }

        clear_locked();
        framebuffer_lock.release(flags);
    }



    bool info(struct fb_mode *out) {
        if (!out) return false;
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        if (!fb.addr || fb.width == 0 || fb.height == 0 || fb.pitch == 0 || fb.bytes == 0) {
            framebuffer_lock.release(flags);
            return false;
        }
        memset(out, 0, sizeof(*out));
        out->width = fb.width;
        out->height = fb.height;
        out->pitch = fb.pitch;
        out->bytes_per_pixel = fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_EGA ? 2 : bytes_per_pixel();
        out->format = fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_RGB ? FB_FORMAT_RGB :
                      (fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED ? FB_FORMAT_INDEXED : FB_FORMAT_TEXT);
        out->red_shift = fb.rgb.r_pos;
        out->red_bits = fb.rgb.r_size;
        out->green_shift = fb.rgb.g_pos;
        out->green_bits = fb.rgb.g_size;
        out->blue_shift = fb.rgb.b_pos;
        out->blue_bits = fb.rgb.b_size;
        out->framebuffer_bytes = fb.bytes;
        out->generation = fb_generation;
        framebuffer_lock.release(flags);
        return true;
    }

    bool map_region(uint64_t offset, uint64_t size, uint64_t *phys_out, uint64_t *bytes_out) {
        if (!phys_out || !bytes_out || size == 0) return false;
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        bool ok = fb.phys && fb.bytes && offset <= fb.bytes && size <= fb.bytes - offset;
        if (ok) {
            *phys_out = fb.phys + offset;
            *bytes_out = size;
        }
        framebuffer_lock.release(flags);
        return ok;
    }

    void claim_console(bool claimed) {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        console_claimed = claimed;
        console_claim_owner = 0;
        cursor_visible = !claimed;
        fb_generation++;
        framebuffer_lock.release(flags);
    }

    void release_console_claim(uint64_t owner) {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        if (console_claimed && (console_claim_owner == 0 || console_claim_owner == owner)) {
            console_claimed = false;
            console_claim_owner = 0;
            cursor_visible = true;
            fb_generation++;
        }
        framebuffer_lock.release(flags);
    }

    bool dirty(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        bool ok = fb.addr && width && height && x < fb.width && y < fb.height &&
                  width <= fb.width - x && height <= fb.height - y;
        if (ok) fb_generation++;
        framebuffer_lock.release(flags);
        return ok;
    }

    bool present(uint32_t flags_in) {
        if ((flags_in & ~FB_PRESENT_SYNC) != 0) return false;
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        bool ok = fb.addr != nullptr;
        if (ok) fb_generation++;
        framebuffer_lock.release(flags);
        return ok;
    }

    void remap_wc() {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        if (fb.phys && fb.bytes && fb.type != MULTIBOOT_FRAMEBUFFER_TYPE_EGA) {
            void *mapped = vmm::mmio_map_wc(fb.phys, fb.bytes);
            if (mapped) fb.addr = (uint8_t *)mapped;
            ensure_text_cells();
        }
        framebuffer_lock.release(flags);
    }

    /**
     * @internal
     * Draw a character at the current cursor position, handling
     * newline, carriage return, tab, and wrapping. Calls scroll
     * if necessary.
     */
    static void putchar_locked(char c) {
        if (text_cols() == 0 || text_rows() == 0) return;

        update_cursor_visual(false);

        uint8_t *font_start = &font_bitmap_start;
        uint32_t step_x = cell_w();

        if (c == '\n') {
            uint32_t col = step_x ? cursor_x / step_x : 0;
            uint32_t cols = text_cols();
            while (col < cols) set_text_cell(col++, cursor_y / cell_h(), ' ');
            advance_line();
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\t') {
            uint32_t col = cursor_x / step_x;
            uint32_t target = (col + 4) & ~3U;
            while (col < target) {
                putchar_locked(' ');
                col++;
            }
            return;
        } else {
            if (cursor_x + step_x > usable_width()) advance_line();

            if (fb.type == 2) {
                uint16_t data = (uint16_t)(current_ega_attr << 8) | (uint8_t)c;
                putpixel_ega(cursor_x, cursor_y, data);
                store_text_cell(cursor_x, cursor_y, (uint8_t)c);
                cursor_x += step_x;
            } else {
                uint8_t index = (uint8_t)c;
                if (index < glyph_count) {
                    draw_glyph(cursor_x, cursor_y, &font_start[index * font_glyph_bytes]);
                } else {
                    draw_missing_glyph(cursor_x, cursor_y);
                }
                store_text_cell(cursor_x, cursor_y, (uint8_t)c);
                cursor_x += step_x;
            }
        }

        update_cursor_visual(true);
    }

    void putchar(char c) {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        putchar_locked(c);
        framebuffer_lock.release(flags);
    }

    static void backspace_locked() {
        update_cursor_visual(false);

        uint32_t step_x = cell_w();
        uint32_t step_y = cell_h();

        if (cursor_x >= step_x) {
            cursor_x -= step_x;
        } else if (cursor_y >= step_y && text_cols() != 0) {
            cursor_y -= step_y;
            cursor_x = usable_width() - step_x;
        } else {
            update_cursor_visual(true);
            return;
        }

        // Overwrite the character area with the background color
        if (fb.type == 2) {  // EGA Text Mode
            uint16_t data = (uint16_t)(current_ega_attr << 8) | ' ';
            putpixel_ega(cursor_x, cursor_y, data);
        } else {  // Graphics Modes
            fill_rect(cursor_x, cursor_y, font_w, font_h, current_bg);
        }
        store_text_cell(cursor_x, cursor_y, ' ');

        update_cursor_visual(true);
    }

    void backspace() {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        backspace_locked();
        framebuffer_lock.release(flags);
    }

    void putpixel(uint32_t x, uint32_t y, uint32_t color) {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        if (putpixel_raw) {
            putpixel_raw(x, y, color);
        }
        framebuffer_lock.release(flags);
    }

    /**
     * @internal
     * Clear the framebuffer with a specified color
     *
     * @param color ARGB color to fill the screen with
     */
    static void clear_locked() {
        if (fb.type == MULTIBOOT_FRAMEBUFFER_TYPE_EGA) {
            uint16_t clear_val = (uint16_t)(current_ega_attr << 8) | ' ';
            uint16_t *dst = (uint16_t *)fb.addr;
            uint64_t cells = (uint64_t)fb.width * fb.height;
            for (uint64_t i = 0; i < cells; i++) dst[i] = clear_val;
        } else {
            fill_rect(0, 0, fb.width, fb.height, current_bg);
        }
        reset_text_cells();
        cursor_x = 0;
        cursor_y = 0;
        update_cursor_visual(true);
    }

    void clear() {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        clear_locked();
        framebuffer_lock.release(flags);
    }

    /**
     * @internal
     * Set cursor enabled/disabled state
     *
     * @param enabled true to enable cursor, false to disable
     */
    static void set_cursor_enabled_locked(bool enabled) {
        update_cursor_visual(false);
        cursor_visible = enabled;
        update_cursor_visual(true);
    }

    static void move_cursor_locked(uint16_t x, uint16_t y) {
        update_cursor_visual(false);
        uint32_t cols = text_cols();
        uint32_t rows = text_rows();
        if (cols == 0 || rows == 0) {
            cursor_x = 0;
            cursor_y = 0;
        } else {
            if (x >= cols) x = cols - 1;
            if (y >= rows) y = rows - 1;
            cursor_x = (uint32_t)x * cell_w();
            cursor_y = (uint32_t)y * cell_h();
        }
        update_cursor_visual(true);
    }

    void move_cursor(uint16_t x, uint16_t y) {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        move_cursor_locked(x, y);
        framebuffer_lock.release(flags);
    }

    void enable_cursor() {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        set_cursor_enabled_locked(true);
        framebuffer_lock.release(flags);
    }

    void disable_cursor() {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        set_cursor_enabled_locked(false);
        framebuffer_lock.release(flags);
    }

    static void set_color_locked(uint8_t fg_idx, uint8_t bg_idx) {
        fg_idx &= 0x0F;
        bg_idx &= 0x0F;
        uint32_t raw_fg = palette_rgb[fg_idx];
        uint32_t raw_bg = palette_rgb[bg_idx];
        current_fg = pack_color((raw_fg >> 16) & 0xFF, (raw_fg >> 8) & 0xFF, raw_fg & 0xFF);
        current_bg = pack_color((raw_bg >> 16) & 0xFF, (raw_bg >> 8) & 0xFF, raw_bg & 0xFF);

        current_ega_attr = (bg_idx << 4) | (fg_idx & 0x0F);
    }

    void set_color(uint8_t fg_idx, uint8_t bg_idx) {
        uint64_t flags;
        framebuffer_lock.acquire(flags);
        set_color_locked(fg_idx, bg_idx);
        framebuffer_lock.release(flags);
    }
}  // namespace framebuffer
