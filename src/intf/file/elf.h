#pragma once
#include <stdint.h>

#include "memory/pmm.h"

#define ELF_MAGIC 0x464C457F

namespace elf {
    static_assert(pmm::PAGE_SIZE == 4096, "ELF loader assumes 4 KiB pages");

    struct elf_header {
        uint32_t magic;
        uint8_t bit_width;
        uint8_t endianness;
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
        uint16_t shstrndx;
    } __attribute__((packed));

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

    struct elf_section_header {
        uint32_t name;
        uint32_t type;
        uint64_t flags;
        uint64_t addr;
        uint64_t offset;
        uint64_t size;
        uint32_t link;
        uint32_t info;
        uint64_t addralign;
        uint64_t entsize;
    } __attribute__((packed));

    struct elf_symbol {
        uint32_t name;
        uint8_t info;
        uint8_t other;
        uint16_t shndx;
        uint64_t value;
        uint64_t size;
    } __attribute__((packed));

    struct elf_rela {
        uint64_t offset;
        uint64_t info;
        int64_t addend;
    } __attribute__((packed));

    struct elf_dynamic {
        int64_t tag;
        uint64_t value;
    } __attribute__((packed));

#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffffL)
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_GOT32 3
#define R_X86_64_PLT32 4
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_IRELATIVE 37
#define R_X86_64_GOTPCREL 9
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_16 12
#define R_X86_64_PC16 13
#define R_X86_64_8 14
#define R_X86_64_PC8 15
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42

    static constexpr uint32_t MAX_LOAD_SEGMENTS = 16;

    struct elf_load_segment {
        uint64_t start;
        uint64_t end;
        uint32_t flags;
    };

    struct elf_info {
        uint64_t entry;
        uint64_t pml4;
        uint64_t heap_start;
        uint64_t load_base;
        uint64_t load_end;
        uint32_t segment_count;
        elf_load_segment segments[MAX_LOAD_SEGMENTS];
        bool pie;
    };

    elf_info load(const char *path);
}  // namespace elf

#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_DYNSYM 11
#define PF_X 1
#define PF_W 2
#define PF_R 4

#define SHF_WRITE (1 << 0)
#define SHF_ALLOC (1 << 1)
#define SHF_EXECINSTR (1 << 2)
#define SHF_MASKPROC 0xF0000000
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define STB_WEAK 2
#define STT_SECTION 3
#define ET_EXEC 2
#define ET_DYN 3

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_FLAGS 30
#define DT_FLAGS_1 0x6ffffffb

#define DF_BIND_NOW 0x8
#define DF_1_NOW 0x1
