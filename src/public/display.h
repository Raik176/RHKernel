#pragma once
#include <stdint.h>

#define FB_FORMAT_RGB 1u
#define FB_FORMAT_INDEXED 2u
#define FB_FORMAT_TEXT 3u
#define FB_FORMAT_XRGB8888 FB_FORMAT_RGB

#define FB_CMD_ACQUIRE 1u
#define FB_CMD_RELEASE 2u
#define FB_CMD_DIRTY 3u
#define FB_CMD_PRESENT 4u

#define FB_PRESENT_SYNC 1u

struct fb_mode {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bytes_per_pixel;
    uint32_t format;
    uint32_t red_shift;
    uint32_t red_bits;
    uint32_t green_shift;
    uint32_t green_bits;
    uint32_t blue_shift;
    uint32_t blue_bits;
    uint64_t framebuffer_bytes;
    uint64_t generation;
};

struct fb_command {
    uint32_t command;
    uint32_t flags;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};
