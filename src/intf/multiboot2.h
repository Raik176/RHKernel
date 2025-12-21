/**
 * @file multiboot2.h
 * @brief Multiboot2 data structures and tag type definitions
 *
 * Definitions of structures provided by the Multiboot2 bootloader
 * for passing information to the kernel. Includes memory map,
 * module info, bootloader name, VBE info, and more.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Generic Multiboot2 tag
 *
 * All tags start with a common header.
 */
struct multiboot_tag {
    uint32_t type;  ///< Tag type identifier
    uint32_t size;  ///< Total size of this tag, including header
};

/**
 * @name Multiboot2 Tag Types
 * @{
 */
#define MULTIBOOT_TAG_TYPE_END                0 ///< Marks the end of the tag list
#define MULTIBOOT_TAG_TYPE_CMDLINE            1 ///< Kernel command line string
#define MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME   2 ///< Name of the bootloader
#define MULTIBOOT_TAG_TYPE_MODULE             3 ///< Loaded module information
#define MULTIBOOT_TAG_TYPE_BASIC_MEMINFO      4 ///< Basic memory info (lower/upper memory)
#define MULTIBOOT_TAG_TYPE_BOOTDEV            5 ///< Boot device information
#define MULTIBOOT_TAG_TYPE_MMAP               6 ///< Memory map (full physical memory layout)
#define MULTIBOOT_TAG_TYPE_VBE                7 ///< VBE (VESA BIOS Extensions) info
#define MULTIBOOT_TAG_TYPE_FRAMEBUFFER        8 ///< Framebuffer info
#define MULTIBOOT_TAG_TYPE_ELF_SECTIONS       9 ///< ELF section headers
#define MULTIBOOT_TAG_TYPE_APM                10 ///< Advanced Power Management info
#define MULTIBOOT_TAG_TYPE_EFI32              11 ///< EFI 32-bit system table
#define MULTIBOOT_TAG_TYPE_EFI64              12 ///< EFI 64-bit system table
#define MULTIBOOT_TAG_TYPE_SMBIOS             13 ///< SMBIOS tables
#define MULTIBOOT_TAG_TYPE_ACPI_OLD           14 ///< ACPI v1.0 RSDP
#define MULTIBOOT_TAG_TYPE_ACPI_NEW           15 ///< ACPI v2.0+ RSDP
#define MULTIBOOT_TAG_TYPE_NETWORK            16 ///< Network information
#define MULTIBOOT_TAG_TYPE_EFI_MMAP           17 ///< EFI memory map
#define MULTIBOOT_TAG_TYPE_EFI_BS             18 ///< EFI boot services info
#define MULTIBOOT_TAG_TYPE_EFI32_IH           19 ///< EFI 32-bit image handle
#define MULTIBOOT_TAG_TYPE_EFI64_IH           20 ///< EFI 64-bit image handle
#define MULTIBOOT_TAG_TYPE_LOAD_BASE_ADDR     21 ///< Load base address info
/** @} */

#define MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED 0
#define MULTIBOOT_FRAMEBUFFER_TYPE_RGB     1
#define MULTIBOOT_FRAMEBUFFER_TYPE_EGA     2

/** @brief Multiboot2 Memory Region Types */
enum multiboot_memory_type {
    MULTIBOOT_MEMORY_AVAILABLE        = 1, ///< Available RAM
    MULTIBOOT_MEMORY_RESERVED         = 2, ///< Reserved (unusable)
    MULTIBOOT_MEMORY_ACPI_RECLAIMABLE = 3, ///< ACPI tables
    MULTIBOOT_MEMORY_NVS              = 4, ///< Non-volatile storage
    MULTIBOOT_MEMORY_BADRAM           = 5  ///< Defective RAM modules
};

/**
 * @brief Single memory map entry
 *
 * Describes a memory region reported by the bootloader.
 */
struct multiboot_mmap_entry {
    uint64_t addr;  ///< Base address of the memory region
    uint64_t len;   ///< Length of the memory region in bytes
    uint32_t type;  ///< Region type. See #multiboot_memory_type.
    uint32_t zero;  ///< Reserved, must be 0 for alignment
};

/**
 * @brief Memory map tag
 *
 * This tag contains a sequence of memory map entries describing
 * the physical memory layout. Usually returned by bootloaders
 * like GRUB.
 */
struct multiboot_tag_mmap {
    uint32_t type;                          ///< Tag type (should equal MULTIBOOT_TAG_TYPE_MMAP)
    uint32_t size;                          ///< Total size of this tag, including header
    uint32_t entry_size;                    ///< Size of each memory map entry
    uint32_t entry_version;                 ///< Version number of the entries
    struct multiboot_mmap_entry entries[0]; ///< Array of memory map entries
};

struct multiboot_color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

struct multiboot_tag_framebuffer {
    uint32_t type;        // = 8
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;

    union {
        struct {
            uint32_t palette_num_colors;
            /* Followed by palette entries */
            /* struct multiboot_color palette[palette_num_colors]; */
        } indexed;

        struct {
            uint8_t red_field_position;
            uint8_t red_mask_size;
            uint8_t green_field_position;
            uint8_t green_mask_size;
            uint8_t blue_field_position;
            uint8_t blue_mask_size;
        } rgb;

        struct {
            /* No additional fields (EGA text mode) */
        } ega;
    };
};