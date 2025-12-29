#pragma once
#include <stdint.h>

#include "memory/pmm.h"

#define ELF_MAGIC 0x464C457F  // "\x7FELF"

namespace elf {
    static_assert(pmm::PAGE_SIZE == 4096, "ELF loader assumes 4 KiB pages");

    struct elf_header {
        uint32_t magic;
        uint8_t endianness;
        uint8_t bit_width;  // 2 = 64-bit
        uint8_t version;
        uint8_t abi;
        uint8_t unused[8];
        uint16_t type;
        uint16_t machine;
        uint32_t version2;
        uint64_t entry;
        uint64_t phoff;
        uint64_t shoff;
        uint32_t flags;
        uint16_t header_size;
        uint16_t ph_entry_size;
        uint16_t ph_count;
        uint16_t sh_entry_size;
        uint16_t sh_count;
        uint16_t str_table_index;
    } __attribute__((packed));

    static_assert(sizeof(elf::elf_header) == 64, "ELF64 header must be exactly 64 bytes");

    struct elf_program_header {
        uint32_t type;
        uint32_t flags;
        uint64_t offset;
        uint64_t vaddr;
        uint64_t paddr;
        uint64_t filesz;
        uint64_t memsz;
        uint64_t align;
    } __attribute__((packed));

    static_assert(sizeof(elf::elf_program_header) == 56,
                  "ELF64 program header must be exactly 56 bytes");

    struct elf_info {
        uint64_t entry;
        uint64_t pml4;
    };

    elf_info load(const char *path);
}  // namespace elf

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4