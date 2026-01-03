#include "file/module_loader.h"

#include "console.h"
#include "file/elf.h"
#include "file/vfs.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "string.h"
#include "symbol/ksym.h"
#include "util.h"

namespace module_loader {
    static constexpr uint64_t MODULE_BASE = 0xFFFFFFFFA0000000ULL;
    static constexpr uint64_t MODULE_SIZE = 0x20000000ULL;  // 512MB space for modules

    static vmm::VirtualRangeAllocator *module_v_alloc = nullptr;

    void init() { module_v_alloc = new vmm::VirtualRangeAllocator(MODULE_BASE, MODULE_SIZE); }

    void load_module(const char *path) {
        vfs::vfs_node *file = vfs::open(path);
        if (!file) {
            console::printf("[MODULE] Could not open %s\n", path);
            return;
        }

        elf::elf_header header;
        vfs::read(file, 0, sizeof(elf::elf_header), &header);

        if (header.magic != ELF_MAGIC) {
            console::printf("[MODULE] %s is not a valid ELF file\n", path);
            return;
        }

        // 1. Calculate total size needed for all ALLOC sections
        size_t total_size = 0;
        for (int i = 0; i < header.sh_count; i++) {
            elf::elf_section_header sh;
            vfs::read(file, header.shoff + (i * header.sh_entry_size), sizeof(sh), &sh);
            if (sh.flags & SHF_ALLOC) {
                total_size = align_up(total_size, pmm::PAGE_SIZE);
                total_size += sh.size;
            }
        }

        size_t page_count = (total_size + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;
        uint64_t phys_base = pmm::alloc(page_count * pmm::PAGE_SIZE);
        uintptr_t virt_base = module_v_alloc->allocate(total_size);
        if (virt_base == 0) {
            console::printf("[MODULE] Out of virtual address space for %s\n", path);
            return;
        }

        vmm::map_range(virt_base, phys_base, page_count * pmm::PAGE_SIZE, vmm::PageFlags::Write);
        memset((void *)virt_base, 0, page_count * pmm::PAGE_SIZE);

        uintptr_t *section_addresses =
            (uintptr_t *)heap::kmalloc(sizeof(uintptr_t) * header.sh_count);
        uintptr_t current_offset = 0;

        for (int i = 0; i < header.sh_count; i++) {
            elf::elf_section_header sh;
            vfs::read(file, header.shoff + (i * header.sh_entry_size), sizeof(sh), &sh);

            if (sh.flags & SHF_ALLOC) {
                current_offset = align_up(current_offset, pmm::PAGE_SIZE);
                uintptr_t section_virt = virt_base + current_offset;

                if (sh.type != 8) {  // SHT_PROGBITS
                    vfs::read(file, sh.offset, sh.size, (void *)section_virt);
                } else {  // SHT_NOBITS (.bss)
                    memset((void *)section_virt, 0, sh.size);
                }

                section_addresses[i] = section_virt;
                current_offset += sh.size;
            } else {
                section_addresses[i] = 0;
            }
        }

        // 5. Relocations
        for (int i = 0; i < header.sh_count; i++) {
            elf::elf_section_header sh;
            vfs::read(file, header.shoff + (i * header.sh_entry_size), sizeof(sh), &sh);

            if (sh.type == SHT_RELA) {
                uintptr_t target_section = section_addresses[sh.info];
                if (!target_section) continue;

                elf::elf_section_header symtab_sh;
                vfs::read(file, header.shoff + (sh.link * header.sh_entry_size), sizeof(symtab_sh),
                          &symtab_sh);
                elf::elf_section_header strtab_sh;
                vfs::read(file, header.shoff + (symtab_sh.link * header.sh_entry_size),
                          sizeof(strtab_sh), &strtab_sh);

                size_t count = sh.size / sizeof(elf::elf_rela);
                for (size_t j = 0; j < count; j++) {
                    elf::elf_rela rela;
                    vfs::read(file, sh.offset + (j * sizeof(rela)), sizeof(rela), &rela);

                    elf::elf_symbol sym;
                    vfs::read(file, symtab_sh.offset + (ELF64_R_SYM(rela.info) * sizeof(sym)),
                              sizeof(sym), &sym);

                    uintptr_t S = 0;
                    if (sym.shndx == 0) {  // External Symbol
                        char name[128];
                        vfs::read(file, strtab_sh.offset + sym.name, 128, name);
                        S = ksym::resolve_symbol(name);
                        if (S == 0) {
                            console::printf("[MODULE] Failed to resolve: %s\n", name);
                            heap::kfree(section_addresses);
                            return;  // Add proper cleanup here in a real OS
                        }
                    } else {  // Internal Symbol
                        S = section_addresses[sym.shndx] + sym.value;
                    }

                    uintptr_t P = target_section + rela.offset;
                    uintptr_t A = rela.addend;
                    switch (ELF64_R_TYPE(rela.info)) {
                        case R_X86_64_64:  // Type 1
                            *(uint64_t *)P = S + A;
                            break;
                        case R_X86_64_32:  // Type 10
                            *(uint32_t *)P = (uint32_t)(S + A);
                            break;
                        case R_X86_64_32S:  // Type 11
                            *(int32_t *)P = (int32_t)(S + A);
                            break;
                        case R_X86_64_PC32:     // Type 2 (The one you are hitting)
                        case R_X86_64_PLT32: {  // Type 4
                            int64_t rel = (int64_t)(S + A - P);

                            // Check if it actually fits in 32 bits (important!)
                            if (rel < -2147483648LL || rel > 2147483647LL) {
                                console::printf("[MODULE] Relocation out of range! %p to %p\n", P,
                                                S);
                            }

                            *(uint32_t *)P = (uint32_t)rel;
                            break;
                        }
                        default:
                            console::printf("[MODULE] Unsupported relocation type: %d\n",
                                            ELF64_R_TYPE(rela.info));
                            break;
                    }
                }
            }
        }

        // 6. Finalize Permissions, find Metadata, and KEXPORT symbols
        elf::elf_section_header sh_strtab;
        vfs::read(file, header.shoff + (header.shstrndx * header.sh_entry_size), sizeof(sh_strtab),
                  &sh_strtab);

        module_metadata *meta = nullptr;

        for (int i = 0; i < header.sh_count; i++) {
            elf::elf_section_header sh;
            vfs::read(file, header.shoff + (i * header.sh_entry_size), sizeof(sh), &sh);

            if (!(sh.flags & SHF_ALLOC)) continue;

            char name[64];
            vfs::read(file, sh_strtab.offset + sh.name, 64, name);

            // Capture Metadata pointer
            if (strcmp(name, ".module_info") == 0) {
                meta = (module_metadata *)section_addresses[i];
            }

            if (strcmp(name, ".ksymtab") == 0) {
                kernel_symbol *syms = (kernel_symbol *)section_addresses[i];
                size_t count = sh.size / sizeof(kernel_symbol);

                for (size_t j = 0; j < count; j++) {
                    ksym::export_symbol(syms[j].name, syms[j].addr);
                }
            }

            /*
            vmm::PageFlags flags = vmm::PageFlags::Global;

            if (sh.flags & SHF_WRITE)
                flags |= vmm::PageFlags::Write;
            if (!(sh.flags & SHF_EXECINSTR))
                flags |= vmm::PageFlags::NX;


            vmm::map_range(section_addresses[i], v2p((void *)section_addresses[i]), //TODO: fix
            address calculation here align_up(sh.size, pmm::PAGE_SIZE), flags);
            */
        }

        // 7. Initialization
        if (meta && meta->init) {
            console::printf("[MODULE] Initializing %s... ", meta->name);
            int res = meta->init();
            if (res == 0) {
                console::printf("Success.\n");
            } else {
                console::printf("Failed: %d.\n", meta->name, res);
            }
        } else {
            console::printf("[MODULE] No metadata or init function in %s\n", path);
        }

        heap::kfree(section_addresses);
    }

}  // namespace module_loader