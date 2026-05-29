#include "file/elf.h"

#include "console.h"
#include "file/vfs.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "security/random.h"
#include "string.h"
#include "util.h"

namespace elf {
    static constexpr uint64_t PIE_BASE_MIN = 0x0000000100000000ULL;
    static constexpr uint64_t PIE_BASE_SPAN = 0x0000003f00000000ULL;
    static constexpr uint64_t USER32_TOP = 0x00000000F0000000ULL;
    static constexpr uint64_t PIE32_BASE_MIN = 0x08000000ULL;
    static constexpr uint64_t PIE32_BASE_SPAN = 0x60000000ULL;
    static constexpr uint64_t BRK_RANDOM_PAGES = 4096;

    static bool add_overflow(uint64_t a, uint64_t b, uint64_t *out) {
        *out = a + b;
        return *out < a;
    }

    static bool range_in_file(vfs::vfs_node *file, uint64_t off, uint64_t size) {
        uint64_t end;
        if (add_overflow(off, size, &end)) return false;
        return end <= file->size;
    }

    static uint64_t random_page_offset(uint64_t page_count) {
        if (page_count == 0) return 0;
        return (random::next_u64() % page_count) * pmm::PAGE_SIZE;
    }

    static elf_info fail() {
        elf_info info{};
        return info;
    }

    static bool read_exact(vfs::vfs_node *file, uint64_t off, uint64_t size, void *out) {
        if (!range_in_file(file, off, size)) return false;
        return vfs::read(file, off, size, out) == size;
    }

    static bool read_phdr(vfs::vfs_node *file, const elf_header &header, uint16_t index,
                          elf_program_header *out) {
        if (index >= header.ph_count) return false;
        uint64_t off = header.phoff + (uint64_t)index * header.ph_entry_size;
        return read_exact(file, off, sizeof(*out), out);
    }

    static bool read_phdr32(vfs::vfs_node *file, const elf32_header &header, uint16_t index,
                            elf32_program_header *out) {
        if (index >= header.ph_count) return false;
        uint64_t off = (uint64_t)header.phoff + (uint64_t)index * header.ph_entry_size;
        return read_exact(file, off, sizeof(*out), out);
    }

    static void *mapped_user_ptr(uint64_t user_va, uint64_t len, uint64_t pml4) {
        if (len == 0) return nullptr;
        uint64_t end;
        if (add_overflow(user_va, len, &end)) return nullptr;
        if (((user_va ^ (end - 1)) & ~(pmm::PAGE_SIZE - 1)) != 0) return nullptr;
        uint64_t phys = vmm::get_mapping(user_va, pml4);
        if (!phys) return nullptr;
        return (void *)((uintptr_t)p2v(phys) + (user_va & (pmm::PAGE_SIZE - 1)));
    }

    static bool load_u64(uint64_t user_va, uint64_t pml4, uint64_t *out) {
        void *p = mapped_user_ptr(user_va, sizeof(uint64_t), pml4);
        if (!p || !out) return false;
        *out = *(uint64_t *)p;
        return true;
    }

    static bool store_u64(uint64_t user_va, uint64_t pml4, uint64_t value) {
        void *p = mapped_user_ptr(user_va, sizeof(uint64_t), pml4);
        if (!p) return false;
        *(uint64_t *)p = value;
        return true;
    }

    static bool load_u32(uint64_t user_va, uint64_t pml4, uint32_t *out) {
        void *p = mapped_user_ptr(user_va, sizeof(uint32_t), pml4);
        if (!p || !out) return false;
        *out = *(uint32_t *)p;
        return true;
    }

    static bool store_u32(uint64_t user_va, uint64_t pml4, uint32_t value) {
        void *p = mapped_user_ptr(user_va, sizeof(uint32_t), pml4);
        if (!p) return false;
        *(uint32_t *)p = value;
        return true;
    }

    static bool read_rela_from_memory(uint64_t user_va, uint64_t pml4, elf_rela *out) {
        if (!out) return false;
        uint64_t offset = 0;
        uint64_t info = 0;
        uint64_t addend = 0;
        if (!load_u64(user_va, pml4, &offset) || !load_u64(user_va + 8, pml4, &info) ||
            !load_u64(user_va + 16, pml4, &addend)) {
            return false;
        }
        out->offset = offset;
        out->info = info;
        out->addend = (int64_t)addend;
        return true;
    }

    static bool add_signed(uint64_t base, int64_t addend, uint64_t *out) {
        if (addend >= 0) {
            *out = base + (uint64_t)addend;
            return *out < base;
        }
        uint64_t sub = (uint64_t)(-addend);
        if (base < sub) return true;
        *out = base - sub;
        return false;
    }

    static bool va_in_loaded_range(uint64_t va, uint64_t size, uint64_t image_start,
                                   uint64_t image_end) {
        uint64_t end;
        if (add_overflow(va, size, &end)) return false;
        return va >= image_start && end <= image_end;
    }

    static bool apply_dynamic_relocations(vfs::vfs_node *file,
                                          const elf_header &header, uint64_t pml4,
                                          uint64_t load_bias, uint64_t image_start,
                                          uint64_t image_end) {
        elf_program_header dynamic_ph{};
        bool saw_dynamic = false;
        for (uint16_t i = 0; i < header.ph_count; i++) {
            elf_program_header ph;
            if (!read_phdr(file, header, i, &ph)) return false;
            if (ph.type == PT_DYNAMIC) {
                if (saw_dynamic) {
                    return false;
                }
                dynamic_ph = ph;
                saw_dynamic = true;
            }
        }

        if (!saw_dynamic) {
            return false;
        }
        if (dynamic_ph.filesz == 0 || dynamic_ph.filesz % sizeof(elf_dynamic) != 0) {
            return false;
        }

        uint64_t dynamic_va;
        if (add_overflow(load_bias, dynamic_ph.vaddr, &dynamic_va) ||
            !va_in_loaded_range(dynamic_va, dynamic_ph.filesz, image_start, image_end)) {
            return false;
        }

        uint64_t rela_va = 0;
        uint64_t rela_size = 0;
        uint64_t rela_ent = 0;

        uint64_t count = dynamic_ph.filesz / sizeof(elf_dynamic);
        for (uint64_t i = 0; i < count; i++) {
            uint64_t tag = 0;
            uint64_t value = 0;
            uint64_t ent_va = dynamic_va + i * sizeof(elf_dynamic);
            if (!load_u64(ent_va, pml4, &tag) || !load_u64(ent_va + 8, pml4, &value)) return false;
            if ((int64_t)tag == DT_NULL) break;

            switch ((int64_t)tag) {
                case DT_RELA: rela_va = value + load_bias; break;
                case DT_RELASZ: rela_size = value; break;
                case DT_RELAENT: rela_ent = value; break;
                case DT_NEEDED:
                    return false;
                case DT_FLAGS:
                case DT_FLAGS_1:
                    break;
                default:
                    break;
            }
        }

        if (rela_size == 0) return true;
        if (rela_va == 0 || rela_ent != sizeof(elf_rela) || rela_size % sizeof(elf_rela) != 0 ||
            !va_in_loaded_range(rela_va, rela_size, image_start, image_end)) {
            return false;
        }

        uint64_t rela_count = rela_size / sizeof(elf_rela);
        for (uint64_t i = 0; i < rela_count; i++) {
            elf_rela rela;
            if (!read_rela_from_memory(rela_va + i * sizeof(elf_rela), pml4, &rela)) return false;

            uint32_t type = ELF64_R_TYPE(rela.info);
            uint64_t sym = ELF64_R_SYM(rela.info);
            if (type != R_X86_64_RELATIVE || sym != 0) {
                return false;
            }

            uint64_t target;
            uint64_t value;
            if (add_overflow(load_bias, rela.offset, &target) ||
                add_signed(load_bias, rela.addend, &value) ||
                !va_in_loaded_range(target, sizeof(uint64_t), image_start, image_end) ||
                !store_u64(target, pml4, value)) {
                return false;
            }
        }

        return true;
    }


    static bool read_rel32_from_memory(uint64_t user_va, uint64_t pml4, elf32_rel *out) {
        if (!out) return false;
        uint32_t offset = 0;
        uint32_t info = 0;
        if (!load_u32(user_va, pml4, &offset) || !load_u32(user_va + 4, pml4, &info)) return false;
        out->offset = offset;
        out->info = info;
        return true;
    }

    static bool apply_dynamic_relocations32(vfs::vfs_node *file,
                                            const elf32_header &header, uint64_t pml4,
                                            uint64_t load_bias, uint64_t image_start,
                                            uint64_t image_end) {
        elf32_program_header dynamic_ph{};
        bool saw_dynamic = false;
        for (uint16_t i = 0; i < header.ph_count; i++) {
            elf32_program_header ph;
            if (!read_phdr32(file, header, i, &ph)) return false;
            if (ph.type == PT_DYNAMIC) {
                if (saw_dynamic) return false;
                dynamic_ph = ph;
                saw_dynamic = true;
            }
        }

        if (!saw_dynamic) return true;
        if (dynamic_ph.filesz == 0 || dynamic_ph.filesz % sizeof(elf32_dynamic) != 0) return false;

        uint64_t dynamic_va;
        if (add_overflow(load_bias, dynamic_ph.vaddr, &dynamic_va) ||
            !va_in_loaded_range(dynamic_va, dynamic_ph.filesz, image_start, image_end)) {
            return false;
        }

        uint64_t rel_va = 0;
        uint64_t rel_size = 0;
        uint64_t rel_ent = 0;

        uint64_t count = dynamic_ph.filesz / sizeof(elf32_dynamic);
        for (uint64_t i = 0; i < count; i++) {
            uint32_t tag_raw = 0;
            uint32_t value = 0;
            uint64_t ent_va = dynamic_va + i * sizeof(elf32_dynamic);
            if (!load_u32(ent_va, pml4, &tag_raw) || !load_u32(ent_va + 4, pml4, &value)) return false;
            int32_t tag = (int32_t)tag_raw;
            if (tag == DT_NULL) break;

            switch (tag) {
                case DT_REL: rel_va = (uint64_t)value + load_bias; break;
                case DT_RELSZ: rel_size = value; break;
                case DT_RELENT: rel_ent = value; break;
                case DT_NEEDED:
                case DT_RELA:
                    return false;
                case DT_FLAGS:
                case DT_FLAGS_1:
                    break;
                default:
                    break;
            }
        }

        if (rel_size == 0) return true;
        if (rel_va == 0 || rel_ent != sizeof(elf32_rel) || rel_size % sizeof(elf32_rel) != 0 ||
            !va_in_loaded_range(rel_va, rel_size, image_start, image_end)) {
            return false;
        }

        uint64_t rel_count = rel_size / sizeof(elf32_rel);
        for (uint64_t i = 0; i < rel_count; i++) {
            elf32_rel rel;
            if (!read_rel32_from_memory(rel_va + i * sizeof(elf32_rel), pml4, &rel)) return false;

            uint32_t type = ELF32_R_TYPE(rel.info);
            uint32_t sym = ELF32_R_SYM(rel.info);
            if (type != R_386_RELATIVE || sym != 0) return false;

            uint64_t target;
            uint32_t old_value;
            if (add_overflow(load_bias, rel.offset, &target) ||
                !va_in_loaded_range(target, sizeof(uint32_t), image_start, image_end) ||
                !load_u32(target, pml4, &old_value)) {
                return false;
            }
            uint64_t value = load_bias + old_value;
            if (value > UINT32_MAX || !store_u32(target, pml4, (uint32_t)value)) return false;
        }

        return true;
    }

    static elf_info load32(vfs::vfs_node *file, const elf32_header &header) {
        if (header.bit_width != 1 || header.endianness != 1 || header.version != 1 ||
            header.version2 != 1 || header.machine != 3 ||
            header.ph_entry_size != sizeof(elf32_program_header) ||
            (header.type != ET_EXEC && header.type != ET_DYN)) {
            return fail();
        }
        if (header.ph_count == 0 || header.ph_count > 128 ||
            (uint64_t)header.phoff + (uint64_t)header.ph_count * header.ph_entry_size > file->size) {
            return fail();
        }

        uint64_t min_vaddr = UINT64_MAX;
        uint64_t max_vaddr = 0;
        elf_load_segment load_segments[MAX_LOAD_SEGMENTS]{};
        uint32_t load_segment_count = 0;
        bool saw_load = false;
        for (uint16_t i = 0; i < header.ph_count; i++) {
            elf32_program_header ph;
            if (!read_phdr32(file, header, i, &ph)) return fail();
            if (ph.type != PT_LOAD) continue;
            saw_load = true;
            if (ph.memsz < ph.filesz || !range_in_file(file, ph.offset, ph.filesz) ||
                (uint64_t)ph.vaddr + ph.memsz < ph.vaddr) {
                return fail();
            }
            if ((ph.flags & PF_W) && (ph.flags & PF_X)) return fail();
            if (load_segment_count == MAX_LOAD_SEGMENTS) return fail();

            uint64_t seg_start = align_down(ph.vaddr, pmm::PAGE_SIZE);
            uint64_t seg_end = align_up((uint64_t)ph.vaddr + ph.memsz, pmm::PAGE_SIZE);
            load_segments[load_segment_count].start = seg_start;
            load_segments[load_segment_count].end = seg_end;
            load_segments[load_segment_count].flags = ph.flags;
            load_segment_count++;
            if (seg_start < min_vaddr) min_vaddr = seg_start;
            if (seg_end > max_vaddr) max_vaddr = seg_end;
        }

        if (!saw_load || header.entry == 0 || max_vaddr <= min_vaddr) return fail();

        uint64_t load_bias = 0;
        if (header.type == ET_DYN) {
            uint64_t image_size = max_vaddr - min_vaddr;
            if (image_size > PIE32_BASE_SPAN) return fail();
            uint64_t base = PIE32_BASE_MIN +
                            random_page_offset((PIE32_BASE_SPAN - image_size) / pmm::PAGE_SIZE);
            load_bias = align_down(base, pmm::PAGE_SIZE) - min_vaddr;
        }

        uint64_t image_start = load_bias + min_vaddr;
        uint64_t image_end = load_bias + max_vaddr;
        if (image_start == 0 || image_end <= image_start || image_end >= USER32_TOP) return fail();

        uint64_t user_pml4 = vmm::create_user_address_space();
        if (!user_pml4) return fail();

        for (uint16_t i = 0; i < header.ph_count; i++) {
            elf32_program_header ph;
            if (!read_phdr32(file, header, i, &ph)) {
                vmm::destroy_user_address_space(user_pml4);
                return fail();
            }
            if (ph.type != PT_LOAD) continue;

            uint64_t mapped_vaddr = load_bias + ph.vaddr;
            uint64_t map_start = align_down(mapped_vaddr, pmm::PAGE_SIZE);
            uint64_t page_offset = mapped_vaddr & (pmm::PAGE_SIZE - 1);
            uint64_t map_size = align_up(page_offset + ph.memsz, pmm::PAGE_SIZE);
            if (map_size == 0 || map_start < image_start || map_start + map_size > image_end) {
                vmm::destroy_user_address_space(user_pml4);
                return fail();
            }

            uint64_t phys_base = pmm::alloc(map_size);
            if (!phys_base) {
                vmm::destroy_user_address_space(user_pml4);
                return fail();
            }

            vmm::PageFlags flags = vmm::PageFlags::User;
            if (ph.flags & PF_W) flags |= vmm::PageFlags::Write;
            if (!(ph.flags & PF_X)) flags |= vmm::PageFlags::NX;

            vmm::map_range(map_start, phys_base, map_size, flags, user_pml4);
            memset(p2v(phys_base), 0, map_size);

            void *dest = (void *)((uintptr_t)p2v(phys_base) + page_offset);
            uint64_t got = vfs::read(file, ph.offset, ph.filesz, dest);
            if (got != ph.filesz) {
                vmm::destroy_user_address_space(user_pml4);
                return fail();
            }
        }

        if (!apply_dynamic_relocations32(file, header, user_pml4, load_bias, image_start,
                                         image_end)) {
            vmm::destroy_user_address_space(user_pml4);
            return fail();
        }

        uint64_t entry = load_bias + header.entry;
        if (entry > UINT32_MAX || !va_in_loaded_range(entry, 1, image_start, image_end)) {
            vmm::destroy_user_address_space(user_pml4);
            return fail();
        }

        uint64_t heap_start = align_up(image_end, pmm::PAGE_SIZE) + pmm::PAGE_SIZE;
        if (heap_start <= image_end || heap_start >= USER32_TOP) {
            vmm::destroy_user_address_space(user_pml4);
            return fail();
        }

        elf_info info{};
        info.entry = entry;
        info.pml4 = user_pml4;
        info.heap_start = heap_start;
        info.load_base = image_start;
        info.load_end = image_end;
        info.segment_count = load_segment_count;
        for (uint32_t i = 0; i < load_segment_count; i++) {
            info.segments[i].start = load_bias + load_segments[i].start;
            info.segments[i].end = load_bias + load_segments[i].end;
            info.segments[i].flags = load_segments[i].flags;
        }
        info.pie = header.type == ET_DYN;
        info.is_32bit = true;
        return info;
    }

    elf_info load(const char *path) {
        vfs::vfs_node *file = vfs::open(path);
        if (!file) {
            return fail();
        }

        elf_header header;
        memset(&header, 0, sizeof(header));
        uint64_t got = vfs::read(file, 0, sizeof(elf_header), &header);
        if (got != sizeof(elf_header)) {
            return fail();
        }

        if (header.magic != ELF_MAGIC) {
            return fail();
        }
        if (header.bit_width == 1) {
            elf32_header header32;
            memset(&header32, 0, sizeof(header32));
            if (!read_exact(file, 0, sizeof(header32), &header32)) return fail();
            return load32(file, header32);
        }
        if (header.bit_width != 2 || header.endianness != 1 || header.version != 1 ||
            header.version2 != 1 || header.machine != 0x3E ||
            header.ph_entry_size != sizeof(elf_program_header) || header.type != ET_DYN) {
            return fail();
        }
        if (header.ph_count == 0 || header.ph_count > 128 ||
            header.phoff + (uint64_t)header.ph_count * header.ph_entry_size > file->size) {
            return fail();
        }

        uint64_t min_vaddr = UINT64_MAX;
        uint64_t max_vaddr = 0;
        elf_load_segment load_segments[MAX_LOAD_SEGMENTS]{};
        uint32_t load_segment_count = 0;
        bool saw_load = false;
        for (uint16_t i = 0; i < header.ph_count; i++) {
            elf_program_header ph;
            if (!read_phdr(file, header, i, &ph)) return fail();
            if (ph.type != PT_LOAD) continue;
            saw_load = true;
            if (ph.memsz < ph.filesz || !range_in_file(file, ph.offset, ph.filesz) ||
                ph.vaddr + ph.memsz < ph.vaddr) {
                return fail();
            }
            if ((ph.flags & PF_W) && (ph.flags & PF_X)) {
                return fail();
            }

            if (load_segment_count == MAX_LOAD_SEGMENTS) {
                return fail();
            }

            uint64_t seg_start = align_down(ph.vaddr, pmm::PAGE_SIZE);
            uint64_t seg_end = align_up(ph.vaddr + ph.memsz, pmm::PAGE_SIZE);
            load_segments[load_segment_count].start = seg_start;
            load_segments[load_segment_count].end = seg_end;
            load_segments[load_segment_count].flags = ph.flags;
            load_segment_count++;
            if (seg_start < min_vaddr) min_vaddr = seg_start;
            if (seg_end > max_vaddr) max_vaddr = seg_end;
        }

        if (!saw_load || header.entry == 0 || max_vaddr <= min_vaddr) {
            return fail();
        }

        uint64_t image_size = max_vaddr - min_vaddr;
        if (image_size > PIE_BASE_SPAN) {
            return fail();
        }
        uint64_t base = PIE_BASE_MIN +
                        random_page_offset((PIE_BASE_SPAN - image_size) / pmm::PAGE_SIZE);
        uint64_t load_bias = align_down(base, pmm::PAGE_SIZE) - min_vaddr;

        uint64_t image_start = load_bias + min_vaddr;
        uint64_t image_end = load_bias + max_vaddr;
        if (image_start == 0 || image_end <= image_start || image_end >= vmm::user_top()) {
            return fail();
        }

        uint64_t user_pml4 = vmm::create_user_address_space();
        if (!user_pml4) {
            return fail();
        }

        for (uint16_t i = 0; i < header.ph_count; i++) {
            elf_program_header ph;
            if (!read_phdr(file, header, i, &ph)) {
                vmm::destroy_user_address_space(user_pml4);
                return fail();
            }
            if (ph.type != PT_LOAD) continue;

            uint64_t mapped_vaddr = load_bias + ph.vaddr;
            uint64_t map_start = align_down(mapped_vaddr, pmm::PAGE_SIZE);
            uint64_t page_offset = mapped_vaddr & (pmm::PAGE_SIZE - 1);
            uint64_t map_size = align_up(page_offset + ph.memsz, pmm::PAGE_SIZE);
            if (map_size == 0 || map_start < image_start || map_start + map_size > image_end) {
                vmm::destroy_user_address_space(user_pml4);
                return fail();
            }

            uint64_t phys_base = pmm::alloc(map_size);
            if (!phys_base) {
                vmm::destroy_user_address_space(user_pml4);
                return fail();
            }

            vmm::PageFlags flags = vmm::PageFlags::User;
            if (ph.flags & PF_W) flags = flags | vmm::PageFlags::Write;
            if (!(ph.flags & PF_X)) flags = flags | vmm::PageFlags::NX;

            vmm::map_range(map_start, phys_base, map_size, flags, user_pml4);
            memset(p2v(phys_base), 0, map_size);

            void *dest = (void *)((uintptr_t)p2v(phys_base) + page_offset);
            got = vfs::read(file, ph.offset, ph.filesz, dest);
            if (got != ph.filesz) {
                vmm::destroy_user_address_space(user_pml4);
                return fail();
            }
        }

        if (!apply_dynamic_relocations(file, header, user_pml4, load_bias, image_start,
                                       image_end)) {
            vmm::destroy_user_address_space(user_pml4);
            return fail();
        }

        uint64_t entry = load_bias + header.entry;
        if (!va_in_loaded_range(entry, 1, image_start, image_end)) {
            vmm::destroy_user_address_space(user_pml4);
            return fail();
        }

        uint64_t heap_start = align_up(image_end, pmm::PAGE_SIZE) +
                              pmm::PAGE_SIZE + random_page_offset(BRK_RANDOM_PAGES);
        if (heap_start <= image_end || heap_start >= vmm::user_top()) {
            vmm::destroy_user_address_space(user_pml4);
            return fail();
        }

        
        elf_info info{};
        info.entry = entry;
        info.pml4 = user_pml4;
        info.heap_start = heap_start;
        info.load_base = image_start;
        info.load_end = image_end;
        info.segment_count = load_segment_count;
        for (uint32_t i = 0; i < load_segment_count; i++) {
            info.segments[i].start = load_bias + load_segments[i].start;
            info.segments[i].end = load_bias + load_segments[i].end;
            info.segments[i].flags = load_segments[i].flags;
        }
        info.pie = true;
        info.is_32bit = false;
        return info;
    }
}  // namespace elf
