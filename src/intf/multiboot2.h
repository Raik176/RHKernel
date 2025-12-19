/**
 * @file multiboot2.h
 * @brief Multiboot2 data structures
 *
 * Definitions of structures provided by the Multiboot2 bootloader
 * for passing information to the kernel.
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

/** @brief Multiboot2 Memory Region Types */
enum multiboot_memory_type {
    MULTIBOOT_MEMORY_AVAILABLE        = 1, ///< Available RAM
    MULTIBOOT_MEMORY_RESERVED         = 2, ///< Reserved (unusable)
    MULTIBOOT_MEMORY_ACPI_RECLAIMABLE = 3, ///< ACPI tables (can be reclaimed)
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
    uint32_t zero;  ///< @note This field is reserved for alignment and must be 0.
};

/**
 * @brief Memory map tag
 *
 * This tag contains a sequence of memory map entries describing
 * the physical memory layout.
 */
struct multiboot_tag_mmap {
    uint32_t type;                          ///< Tag type (should equal MULTIBOOT_TAG_TYPE_MMAP)
    uint32_t size;                          ///< Total size of this tag, including header
    uint32_t entry_size;                    ///< Size of each memory map entry
    uint32_t entry_version;                 ///< Version number of the entries
    struct multiboot_mmap_entry entries[0]; ///< Array of memory map entries
};