/**
 * @file multiboot2.h
 * @brief Multiboot2 data structures and tag type definitions
 *
 * This file defines structures provided by the Multiboot2 bootloader
 * for passing information to the kernel. Includes memory map, module info,
 * bootloader name, VBE info, framebuffer, and other tags.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Generic Multiboot2 tag header
 *
 * All Multiboot2 tags start with this header.
 */
struct multiboot_tag {
    uint32_t type;  ///< Tag type identifier
    uint32_t size;  ///< Total size of the tag, including header
};

/**
 * @name Multiboot2 Tag Types
 * @{
 */
#define MULTIBOOT_TAG_TYPE_END 0               ///< Marks the end of the tag list
#define MULTIBOOT_TAG_TYPE_CMDLINE 1           ///< Kernel command line string
#define MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME 2  ///< Bootloader name
#define MULTIBOOT_TAG_TYPE_MODULE 3            ///< Loaded module information
#define MULTIBOOT_TAG_TYPE_BASIC_MEMINFO 4     ///< Basic memory info (lower/upper memory)
#define MULTIBOOT_TAG_TYPE_BOOTDEV 5           ///< Boot device information
#define MULTIBOOT_TAG_TYPE_MMAP 6              ///< Memory map (physical memory layout)
#define MULTIBOOT_TAG_TYPE_VBE 7               ///< VBE (VESA BIOS Extensions) info
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER 8       ///< Framebuffer info
#define MULTIBOOT_TAG_TYPE_ELF_SECTIONS 9      ///< ELF section headers
#define MULTIBOOT_TAG_TYPE_APM 10              ///< Advanced Power Management info
#define MULTIBOOT_TAG_TYPE_EFI32 11            ///< EFI 32-bit system table
#define MULTIBOOT_TAG_TYPE_EFI64 12            ///< EFI 64-bit system table
#define MULTIBOOT_TAG_TYPE_SMBIOS 13           ///< SMBIOS tables
#define MULTIBOOT_TAG_TYPE_ACPI_OLD 14         ///< ACPI v1.0 RSDP
#define MULTIBOOT_TAG_TYPE_ACPI_NEW 15         ///< ACPI v2.0+ RSDP
#define MULTIBOOT_TAG_TYPE_NETWORK 16          ///< Network information
#define MULTIBOOT_TAG_TYPE_EFI_MMAP 17         ///< EFI memory map
#define MULTIBOOT_TAG_TYPE_EFI_BS 18           ///< EFI boot services info
#define MULTIBOOT_TAG_TYPE_EFI32_IH 19         ///< EFI 32-bit image handle
#define MULTIBOOT_TAG_TYPE_EFI64_IH 20         ///< EFI 64-bit image handle
#define MULTIBOOT_TAG_TYPE_LOAD_BASE_ADDR 21   ///< Load base address info
/** @} */

/** @brief Framebuffer types */
#define MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED 0  ///< Indexed color
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB 1      ///< Direct RGB
#define MULTIBOOT_FRAMEBUFFER_TYPE_EGA 2      ///< EGA text mode

/**
 * @brief Multiboot2 memory region types
 */
enum multiboot_memory_type {
    MULTIBOOT_MEMORY_AVAILABLE = 1,         ///< RAM available for use
    MULTIBOOT_MEMORY_RESERVED = 2,          ///< Reserved memory
    MULTIBOOT_MEMORY_ACPI_RECLAIMABLE = 3,  ///< ACPI reclaimable memory
    MULTIBOOT_MEMORY_NVS = 4,               ///< Non-volatile storage
    MULTIBOOT_MEMORY_BADRAM = 5             ///< Defective memory
};

/**
 * @brief Single memory map entry
 *
 * Represents a region of memory reported by the bootloader.
 */
struct multiboot_mmap_entry {
    uint64_t addr;  ///< Base address of the memory region
    uint64_t len;   ///< Length of the memory region in bytes
    uint32_t type;  ///< Memory type. See #multiboot_memory_type
    uint32_t zero;  ///< Reserved, must be 0
};

/**
 * @brief Memory map tag
 *
 * Contains a sequence of memory map entries describing the physical memory layout.
 */
struct multiboot_tag_mmap {
    uint32_t type;                           ///< Tag type (MULTIBOOT_TAG_TYPE_MMAP)
    uint32_t size;                           ///< Total size of the tag, including header
    uint32_t entry_size;                     ///< Size of each memory map entry
    uint32_t entry_version;                  ///< Version number of entries
    struct multiboot_mmap_entry entries[0];  ///< Array of memory map entries
};

/**
 * @brief Represents an RGB color
 */
struct multiboot_color {
    uint8_t red;    ///< Red component
    uint8_t green;  ///< Green component
    uint8_t blue;   ///< Blue component
};

/**
 * @brief Framebuffer information tag
 *
 * Contains details about the framebuffer provided by the bootloader.
 */
struct multiboot_tag_framebuffer {
    uint32_t type;             ///< Tag type (MULTIBOOT_TAG_TYPE_FRAMEBUFFER)
    uint32_t size;             ///< Total size of the tag
    uint64_t addr;             ///< Physical address of the framebuffer
    uint32_t pitch;            ///< Number of bytes per scanline
    uint32_t width;            ///< Framebuffer width in pixels
    uint32_t height;           ///< Framebuffer height in pixels
    uint8_t bpp;               ///< Bits per pixel
    uint8_t framebuffer_type;  ///< Type of framebuffer (indexed, RGB, EGA)
    uint16_t reserved;         ///< Reserved for alignment

    union {
        struct {
            uint32_t palette_num_colors;  ///< Number of colors in the palette
            /* Followed by struct multiboot_color palette[palette_num_colors] */
        } indexed;

        struct {
            uint8_t red_field_position;    ///< Bit position of red component
            uint8_t red_mask_size;         ///< Bit size of red component
            uint8_t green_field_position;  ///< Bit position of green component
            uint8_t green_mask_size;       ///< Bit size of green component
            uint8_t blue_field_position;   ///< Bit position of blue component
            uint8_t blue_mask_size;        ///< Bit size of blue component
        } rgb;

        struct {
        } ega;
    };
};

/**
 * @brief Multiboot2 Module tag
 * * Reports the location and command line of a loaded module (like an initramfs).
 */
struct multiboot_tag_module {
    uint32_t type;       ///< Tag type (MULTIBOOT_TAG_TYPE_MODULE)
    uint32_t size;       ///< Total size of the tag
    uint32_t mod_start;  ///< Physical start address of the module
    uint32_t mod_end;    ///< Physical end address of the module
    char cmdline[0];     ///< Command line string (null-terminated)
};