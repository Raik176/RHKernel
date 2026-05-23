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

#if defined(__GNUC__)
#define MODULE_NOIPA __attribute__((noipa))
#else
#define MODULE_NOIPA
#endif

extern "C" {
extern uint8_t _text_start[];
extern uint8_t _text_end[];
extern uint8_t _bss_end[];
extern uint8_t _kernel_phys_start[];
extern uint8_t _kernel_phys_end[];
}

namespace module_loader {
    static vmm::VirtualRangeAllocator *module_v_alloc = nullptr;
    static LoadedModule *module_list = nullptr;
    static vfs::vfs_node *proc_modules_dir = nullptr;

    static bool add_overflow(uint64_t a, uint64_t b, uint64_t *out) {
        *out = a + b;
        return *out < a;
    }

    static bool mul_overflow(uint64_t a, uint64_t b, uint64_t *out) {
        if (a != 0 && b > UINT64_MAX / a) return true;
        *out = a * b;
        return false;
    }

    static bool align_up_checked(uint64_t value, uint64_t align, uint64_t *out) {
        uint64_t mask = align - 1;
        if (add_overflow(value, mask, out)) return false;
        *out &= ~mask;
        return true;
    }

    static bool range_contains(uintptr_t start, uintptr_t end, uintptr_t addr, uint64_t size) {
        uint64_t addr_end;
        if (add_overflow(addr, size, &addr_end)) return false;
        return addr >= start && addr_end <= end;
    }

    static bool range_in_file(vfs::vfs_node *file, uint64_t off, uint64_t size) {
        uint64_t end;
        if (add_overflow(off, size, &end)) return false;
        return end <= file->size;
    }

    static bool read_exact(vfs::vfs_node *file, uint64_t off, uint64_t size, void *out) {
        if (!range_in_file(file, off, size)) return false;
        return vfs::read(file, off, size, out) == size;
    }

    static bool read_section(vfs::vfs_node *file, const elf::elf_header &header, uint16_t index,
                             elf::elf_section_header *out) {
        if (index >= header.sh_count) return false;
        uint64_t add;
        uint64_t off;
        if (mul_overflow(index, header.sh_entry_size, &add) || add_overflow(header.shoff, add, &off)) return false;
        return read_exact(file, off, sizeof(*out), out);
    }

    static bool read_symbol(vfs::vfs_node *file, const elf::elf_section_header &symtab,
                            uint64_t index, elf::elf_symbol *out) {
        uint64_t add;
        uint64_t off;
        if (mul_overflow(index, sizeof(*out), &add) || add_overflow(symtab.offset, add, &off)) return false;
        return read_exact(file, off, sizeof(*out), out);
    }

    static bool read_rela(vfs::vfs_node *file, const elf::elf_section_header &section,
                          uint64_t index, elf::elf_rela *out) {
        uint64_t add;
        uint64_t off;
        if (mul_overflow(index, sizeof(*out), &add) || add_overflow(section.offset, add, &off)) return false;
        return read_exact(file, off, sizeof(*out), out);
    }

    static bool read_cstr(vfs::vfs_node *file, const elf::elf_section_header &strtab,
                          uint32_t off, char *out, uint64_t out_size) {
        if (!out || out_size == 0 || off >= strtab.size) return false;
        uint64_t file_off;
        if (add_overflow(strtab.offset, off, &file_off)) return false;
        uint64_t max = strtab.size - off;
        for (uint64_t i = 0; i < max && i + 1 < out_size; i++) {
            if (!read_exact(file, file_off + i, 1, out + i)) return false;
            if (out[i] == 0) return true;
        }
        out[out_size - 1] = 0;
        return false;
    }

    static void zero_bytes(void *ptr, uint64_t size) {
        volatile uint8_t *p = (volatile uint8_t *)ptr;
        while (size--) *p++ = 0;
    }

    static bool relocation_fits(uint64_t off, uint64_t width, uint64_t section_size) {
        if (off > section_size) return false;
        return width <= section_size - off;
    }

    static bool add_signed(uintptr_t base, int64_t addend, uintptr_t *out) {
        if (addend >= 0) {
            uint64_t u = (uint64_t)addend;
            if (base > UINT64_MAX - u) return false;
            *out = base + u;
            return true;
        }
        uint64_t u = (uint64_t)(-addend);
        if (base < u) return false;
        *out = base - u;
        return true;
    }

    static bool cstr_in_range(const char *s, uintptr_t start, uintptr_t end) {
        uintptr_t p = (uintptr_t)s;
        if (p < start || p >= end) return false;
        while (p < end) {
            if (*(const char *)p == 0) return true;
            p++;
        }
        return false;
    }

    static bool valid_component(const char *s) {
        if (!s || !*s || strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return false;
        for (const char *p = s; *p; p++) if (*p == '/') return false;
        return true;
    }

    static bool ranges_overlap(uintptr_t a0, uintptr_t a1, uintptr_t b0, uintptr_t b1) {
        if (a0 == a1 || b0 == b1) return false;
        return a0 < b1 && b0 < a1;
    }

    static bool address_in_module(const LoadedModule *m, uintptr_t addr, uint64_t size = 1) {
        return m && range_contains(m->base, m->end, addr, size);
    }

    static bool address_executable_in_sections(ModuleSection *sections, uint64_t count, uintptr_t addr) {
        for (uint64_t i = 0; i < count; i++) {
            if (!(sections[i].flags & SHF_EXECINSTR)) continue;
            if (range_contains(sections[i].start, sections[i].end, addr, 1)) return true;
        }
        return false;
    }

    bool address_in_kernel(uintptr_t addr) {
        return range_contains((uintptr_t)_text_start, (uintptr_t)_bss_end, addr, 1);
    }

    bool address_in_kernel_text(uintptr_t addr) {
        return range_contains((uintptr_t)_text_start, (uintptr_t)_text_end, addr, 1);
    }

    const LoadedModule *find_module_containing(uintptr_t addr, uint64_t size) {
        for (LoadedModule *m = module_list; m; m = m->next) {
            if (address_in_module(m, addr, size)) return m;
        }
        return nullptr;
    }

    bool address_in_module_text(const LoadedModule *m, uintptr_t addr) {
        if (!m || !m->sections) return false;
        for (uint64_t i = 0; i < m->section_count; i++) {
            if (!(m->sections[i].flags & SHF_EXECINSTR)) continue;
            if (range_contains(m->sections[i].start, m->sections[i].end, addr, 1)) return true;
        }
        return false;
    }

    bool address_in_kernel_or_module(uintptr_t addr, uint64_t size) {
        if (range_contains((uintptr_t)_text_start, (uintptr_t)_bss_end, addr, size)) return true;
        return find_module_containing(addr, size) != nullptr;
    }

    uint64_t kernel_image_size() { return (uint64_t)(_kernel_phys_end - _kernel_phys_start); }

    uint64_t module_total_mapped_size() {
        uint64_t total = 0;
        for (LoadedModule *m = module_list; m; m = m->next) total += m->mapped_size;
        return total;
    }

    const LoadedModule *first_module() { return module_list; }

    const LoadedModule *find_module(const char *name) {
        for (LoadedModule *m = module_list; m; m = m->next) {
            if (strcmp(m->name, name) == 0) return m;
        }
        return nullptr;
    }

    bool is_module_loaded(const char *name) { return find_module(name) != nullptr; }

    void list_modules() {
        for (LoadedModule *m = module_list; m; m = m->next) {
            console::printf("%s base=%p size=%d\n", m->name, m->base, m->mapped_size);
        }
    }

    bool load_by_name(const char *name) {
        if (!valid_component(name)) return false;
        char path[160];
        snprintf(path, sizeof(path), "/lib/modules/%s.ko", name);
        bool before = is_module_loaded(name);
        load_module(path);
        return before || is_module_loaded(name);
    }

    static void append(char *buf, uint64_t cap, uint64_t *pos, const char *s) {
        while (*s && *pos + 1 < cap) buf[(*pos)++] = *s++;
        if (*pos < cap) buf[*pos] = 0;
    }

    static void appendf(char *buf, uint64_t cap, uint64_t *pos, const char *fmt, ...) {
        if (!buf || !pos || *pos >= cap) return;
        char tmp[384];
        va_list args;
        va_start(args, fmt);
        int len = vsnprintf(tmp, sizeof(tmp), fmt, args);
        va_end(args);
        if (len > 0) append(buf, cap, pos, tmp);
    }

    static uint64_t proc_copy(char *buf, uint64_t len, uint64_t offset, uint64_t size, uint8_t *out) {
        if (offset >= len) return 0;
        if (size > len - offset) size = len - offset;
        memcpy(out, buf + offset, size);
        return size;
    }

    static uint64_t proc_module_info_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
        LoadedModule *m = (LoadedModule *)node->ptr;
        if (!m) return 0;
        char buf[1024];
        uint64_t len = 0;
        buf[0] = 0;
        appendf(buf, sizeof(buf), &len, "name: %s\npath: %s\nbase: %p\nend: %p\nimage_size: %d\nmapped_size: %d\nfile_size: %d\nexec_start: %p\nexec_end: %p\nsections: %d\nsymbols: %d\n",
                m->name, m->path, m->base, m->end, m->image_size, m->mapped_size, m->file_size,
                m->exec_start, m->exec_end, m->section_count, m->symbol_count);
        return proc_copy(buf, len, offset, size, buffer);
    }

    static uint64_t proc_module_maps_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
        LoadedModule *m = (LoadedModule *)node->ptr;
        if (!m) return 0;
        char buf[4096];
        uint64_t len = 0;
        buf[0] = 0;
        append(buf, sizeof(buf), &len, "start end flags name\n");
        for (uint64_t i = 0; i < m->section_count; i++) {
            const char *r = "r";
            const char *w = (m->sections[i].flags & SHF_WRITE) ? "w" : "-";
            const char *x = (m->sections[i].flags & SHF_EXECINSTR) ? "x" : "-";
            appendf(buf, sizeof(buf), &len, "%p %p %s%s%s %s\n", m->sections[i].start,
                    m->sections[i].end, r, w, x, m->sections[i].name);
        }
        return proc_copy(buf, len, offset, size, buffer);
    }

    static uint64_t proc_module_symbols_read(vfs::vfs_node *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
        LoadedModule *m = (LoadedModule *)node->ptr;
        if (!m) return 0;
        char buf[8192];
        uint64_t len = 0;
        buf[0] = 0;
        append(buf, sizeof(buf), &len, "address name\n");
        for (uint64_t i = 0; i < m->symbol_count; i++) {
            appendf(buf, sizeof(buf), &len, "%p %s\n", m->symbols[i].addr, m->symbols[i].name);
        }
        return proc_copy(buf, len, offset, size, buffer);
    }

    static vfs::vfs_node *proc_child(vfs::vfs_node *parent, const char *name, vfs::VfsType type) {
        vfs::vfs_node *node = vfs::finddir(parent, name);
        if (node) return node;
        return vfs::create_node(name, type, parent);
    }

    static void publish_proc_root() {
        vfs::vfs_node *proc = vfs::open("/proc");
        if (!proc || proc->type != vfs::VfsType::VFS_DIRECTORY) return;
        proc_modules_dir = proc_child(proc, "modules", vfs::VfsType::VFS_DIRECTORY);
        if (!proc_modules_dir) return;
    }

    static void publish_proc_module(LoadedModule *m) {
        if (!m || !valid_component(m->name)) return;
        if (!proc_modules_dir) publish_proc_root();
        if (!proc_modules_dir) return;
        vfs::vfs_node *dir = proc_child(proc_modules_dir, m->name, vfs::VfsType::VFS_DIRECTORY);
        if (!dir) return;
        struct Entry { const char *name; uint64_t (*read)(vfs::vfs_node *, uint64_t, uint64_t, uint8_t *); };
        Entry entries[] = {{"info", proc_module_info_read}, {"maps", proc_module_maps_read}, {"symbols", proc_module_symbols_read}};
        for (uint64_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
            vfs::vfs_node *n = proc_child(dir, entries[i].name, vfs::VfsType::VFS_CHAR_DEVICE);
            if (n) { n->ptr = (uintptr_t)m; n->read = entries[i].read; }
        }
    }

    void init() {
        module_v_alloc = new vmm::VirtualRangeAllocator(MODULE_BASE, MODULE_SIZE);
        publish_proc_root();
    }

    MODULE_NOIPA void load_module(const char *path) {
        vfs::vfs_node *file = vfs::open(path);
        if (!file) {
            console::printf("[MODULE] Could not open %s\n", path);
            return;
        }

        elf::elf_header header;
        if (!read_exact(file, 0, sizeof(header), &header) || header.magic != ELF_MAGIC) {
            console::printf("[MODULE] %s is not a valid ELF file\n", path);
            return;
        }

        uint64_t sh_table_size;
        if (header.sh_entry_size != sizeof(elf::elf_section_header) || header.sh_count == 0 ||
            header.sh_count > 256 || mul_overflow(header.sh_count, header.sh_entry_size, &sh_table_size) ||
            !range_in_file(file, header.shoff, sh_table_size)) {
            console::printf("[MODULE] %s has an invalid section table\n", path);
            return;
        }

        uint64_t total_size = 0;
        uint64_t alloc_count = 0;
        for (uint16_t i = 0; i < header.sh_count; i++) {
            elf::elf_section_header sh;
            if (!read_section(file, header, i, &sh)) {
                console::printf("[MODULE] Failed to read section header in %s\n", path);
                return;
            }
            if (!(sh.flags & SHF_ALLOC)) continue;
            if (!align_up_checked(total_size, pmm::PAGE_SIZE, &total_size) ||
                add_overflow(total_size, sh.size, &total_size)) {
                console::printf("[MODULE] %s is too large\n", path);
                return;
            }
            alloc_count++;
        }

        if (total_size == 0 || alloc_count == 0) {
            console::printf("[MODULE] %s has no loadable sections\n", path);
            return;
        }

        uint64_t rounded_size;
        if (!align_up_checked(total_size, pmm::PAGE_SIZE, &rounded_size)) {
            console::printf("[MODULE] %s is too large\n", path);
            return;
        }
        size_t page_count = rounded_size / pmm::PAGE_SIZE;
        uint64_t phys_base = pmm::alloc(page_count * pmm::PAGE_SIZE);
        if (!phys_base) {
            console::printf("[MODULE] Out of physical memory for %s\n", path);
            return;
        }

        uintptr_t virt_base = module_v_alloc->allocate(rounded_size);
        if (!virt_base) {
            console::printf("[MODULE] Out of virtual address space for %s\n", path);
            pmm::free(phys_base, page_count * pmm::PAGE_SIZE);
            return;
        }
        uintptr_t virt_end = virt_base + rounded_size;

        vmm::map_range(virt_base, phys_base, rounded_size, vmm::PageFlags::Write);
        zero_bytes((void *)virt_base, rounded_size);

        module_metadata *meta = nullptr;
        uint8_t *page_writable = nullptr;
        uint8_t *page_executable = nullptr;
        uintptr_t *section_addresses = (uintptr_t *)heap::kmalloc(sizeof(uintptr_t) * header.sh_count);
        uint64_t *section_sizes = (uint64_t *)heap::kmalloc(sizeof(uint64_t) * header.sh_count);
        ModuleSection *sections = (ModuleSection *)heap::kmalloc(sizeof(ModuleSection) * alloc_count);
        elf::elf_section_header sh_strtab;
        uint64_t section_count = 0;
        uintptr_t current_offset = 0;
        uintptr_t exec_start = UINT64_MAX;
        uintptr_t exec_end = 0;
        kernel_symbol *pending_symbols = nullptr;
        uint64_t pending_symbol_count = 0;
        LoadedModule *loaded = nullptr;
        kernel_symbol *symbols = nullptr;
        if (!section_addresses || !section_sizes || !sections) {
            console::printf("[MODULE] Out of heap memory for %s\n", path);
            goto fail_unmapped;
        }

        for (uint16_t i = 0; i < header.sh_count; i++) {
            section_addresses[i] = 0;
            section_sizes[i] = 0;
        }

        if (header.shstrndx >= header.sh_count || !read_section(file, header, header.shstrndx, &sh_strtab)) goto fail;

        for (uint16_t i = 0; i < header.sh_count; i++) {
            elf::elf_section_header sh;
            if (!read_section(file, header, i, &sh)) goto fail;
            if (!(sh.flags & SHF_ALLOC)) continue;

            uint64_t aligned;
            if (!align_up_checked(current_offset, pmm::PAGE_SIZE, &aligned)) goto fail;
            current_offset = aligned;
            uint64_t section_virt_u;
            uint64_t section_end_u;
            if (add_overflow(virt_base, current_offset, &section_virt_u) ||
                add_overflow(section_virt_u, sh.size, &section_end_u) || section_end_u > virt_end) {
                console::printf("[MODULE] Section address overflow in %s\n", path);
                goto fail;
            }
            uintptr_t section_virt = section_virt_u;
            uintptr_t section_end = section_end_u;

            for (uint64_t j = 0; j < section_count; j++) {
                if (ranges_overlap(section_virt, section_end, sections[j].start, sections[j].end)) {
                    console::printf("[MODULE] Overlapping alloc sections in %s\n", path);
                    goto fail;
                }
            }

            char sec_name[64];
            if (!read_cstr(file, sh_strtab, sh.name, sec_name, sizeof(sec_name))) goto fail;
            sections[section_count].name = strdup(sec_name);
            sections[section_count].start = section_virt;
            sections[section_count].end = section_end;
            sections[section_count].flags = sh.flags;
            if (!sections[section_count].name) goto fail;
            section_count++;

            section_addresses[i] = section_virt;
            section_sizes[i] = sh.size;

            if (sh.type == SHT_NOBITS) {
                zero_bytes((void *)section_virt, sh.size);
            } else {
                if (!range_in_file(file, sh.offset, sh.size) ||
                    vfs::read(file, sh.offset, sh.size, (void *)section_virt) != sh.size) {
                    console::printf("[MODULE] Failed to read section in %s\n", path);
                    goto fail;
                }
            }

            if (add_overflow(current_offset, sh.size, &current_offset)) goto fail;
        }

        for (uint16_t i = 0; i < header.sh_count; i++) {
            elf::elf_section_header sh;
            if (!read_section(file, header, i, &sh)) goto fail;
            if (sh.type != SHT_RELA) continue;
            if (sh.info >= header.sh_count || sh.link >= header.sh_count) {
                console::printf("[MODULE] Invalid relocation metadata in %s\n", path);
                goto fail;
            }

            uintptr_t target_section = section_addresses[sh.info];
            if (!target_section) continue;
            if (sh.size % sizeof(elf::elf_rela) != 0) {
                console::printf("[MODULE] Invalid relocation size in %s\n", path);
                goto fail;
            }

            elf::elf_section_header symtab_sh;
            elf::elf_section_header strtab_sh;
            if (!read_section(file, header, sh.link, &symtab_sh) ||
                symtab_sh.link >= header.sh_count ||
                !read_section(file, header, symtab_sh.link, &strtab_sh)) {
                console::printf("[MODULE] Invalid symbol table in %s\n", path);
                goto fail;
            }

            uint64_t count = sh.size / sizeof(elf::elf_rela);
            for (uint64_t j = 0; j < count; j++) {
                elf::elf_rela rela;
                if (!read_rela(file, sh, j, &rela)) goto fail;

                uint64_t sym_index = ELF64_R_SYM(rela.info);
                uint64_t sym_end;
                if (add_overflow(sym_index, 1, &sym_end) || mul_overflow(sym_end, sizeof(elf::elf_symbol), &sym_end) ||
                    sym_end > symtab_sh.size) {
                    console::printf("[MODULE] Invalid symbol index in %s\n", path);
                    goto fail;
                }

                elf::elf_symbol sym;
                if (!read_symbol(file, symtab_sh, sym_index, &sym)) goto fail;

                uintptr_t S = 0;
                if (sym.shndx == SHN_UNDEF) {
                    char name[256];
                    if (!read_cstr(file, strtab_sh, sym.name, name, sizeof(name))) goto fail;
                    S = ksym::resolve_symbol(name);
                    if (!S) {
                        console::printf("[MODULE] Failed to resolve: %s\n", name);
                        goto fail;
                    }
                } else if (sym.shndx == SHN_ABS) {
                    S = sym.value;
                } else {
                    if (sym.shndx >= header.sh_count || !section_addresses[sym.shndx]) {
                        console::printf("[MODULE] Invalid symbol section in %s\n", path);
                        goto fail;
                    }
                    uint64_t saddr;
                    if (add_overflow(section_addresses[sym.shndx], sym.value, &saddr) ||
                        !range_contains(section_addresses[sym.shndx], section_addresses[sym.shndx] + section_sizes[sym.shndx], saddr, sym.size ? sym.size : 1)) {
                        console::printf("[MODULE] Symbol address overflow in %s\n", path);
                        goto fail;
                    }
                    S = saddr;
                }

                if (!relocation_fits(rela.offset, 4, section_sizes[sh.info])) goto fail;
                uintptr_t P = target_section + rela.offset;
                uint32_t type = ELF64_R_TYPE(rela.info);

                switch (type) {
                    case R_X86_64_64: {
                        if (!relocation_fits(rela.offset, 8, section_sizes[sh.info])) goto fail;
                        uintptr_t v;
                        if (!add_signed(S, rela.addend, &v)) goto fail;
                        *(uint64_t *)P = v;
                        break;
                    }
                    case R_X86_64_32: {
                        uintptr_t v;
                        if (!add_signed(S, rela.addend, &v) || v > UINT32_MAX) goto fail;
                        *(uint32_t *)P = (uint32_t)v;
                        break;
                    }
                    case R_X86_64_32S: {
                        uintptr_t vu;
                        if (!add_signed(S, rela.addend, &vu)) goto fail;
                        int64_t v = (int64_t)vu;
                        if (v < INT32_MIN || v > INT32_MAX) goto fail;
                        *(int32_t *)P = (int32_t)v;
                        break;
                    }
                    case R_X86_64_PC32:
                    case R_X86_64_PLT32: {
                        __int128 rel = (__int128)S + (__int128)rela.addend - (__int128)P;
                        if (rel < INT32_MIN || rel > INT32_MAX) {
                            console::printf("[MODULE] Relocation out of range in %s\n", path);
                            goto fail;
                        }
                        *(uint32_t *)P = (uint32_t)(int32_t)rel;
                        break;
                    }
                    default:
                        console::printf("[MODULE] Unsupported relocation type: %d in %s\n", type, path);
                        goto fail;
                }
            }
        }

        page_writable = (uint8_t *)heap::kmalloc(page_count);
        page_executable = (uint8_t *)heap::kmalloc(page_count);
        if (!page_writable || !page_executable) goto fail;
        memset(page_writable, 0, page_count);
        memset(page_executable, 0, page_count);

        for (uint16_t i = 0; i < header.sh_count; i++) {
            elf::elf_section_header sh;
            if (!read_section(file, header, i, &sh)) goto fail_perm;
            if (!(sh.flags & SHF_ALLOC)) continue;

            char name[64];
            if (!read_cstr(file, sh_strtab, sh.name, name, sizeof(name))) goto fail_perm;

            if (strcmp(name, ".module_info") == 0) {
                if (sh.size < sizeof(module_metadata) || !range_contains(virt_base, virt_end, section_addresses[i], sizeof(module_metadata))) {
                    console::printf("[MODULE] Invalid .module_info in %s\n", path);
                    goto fail_perm;
                }
                meta = (module_metadata *)section_addresses[i];
            }
            if (strcmp(name, ".ksymtab") == 0) {
                if (sh.size % sizeof(kernel_symbol) != 0) {
                    console::printf("[MODULE] Invalid .ksymtab size in %s\n", path);
                    goto fail_perm;
                }
                pending_symbols = (kernel_symbol *)section_addresses[i];
                pending_symbol_count = sh.size / sizeof(kernel_symbol);
            }

            if (sh.size == 0) continue;
            uintptr_t section_virt = section_addresses[i];
            uint64_t first_page = (section_virt - virt_base) / pmm::PAGE_SIZE;
            uint64_t last_page = (section_virt + sh.size - 1 - virt_base) / pmm::PAGE_SIZE;
            if (last_page >= page_count) goto fail_perm;

            if (sh.flags & SHF_EXECINSTR) {
                if (section_virt < exec_start) exec_start = section_virt;
                if (section_virt + sh.size > exec_end) exec_end = section_virt + sh.size;
            }

            for (uint64_t page = first_page; page <= last_page; page++) {
                if (sh.flags & SHF_WRITE) page_writable[page] = 1;
                if (sh.flags & SHF_EXECINSTR) page_executable[page] = 1;
            }
        }

        if (!meta || !range_contains(virt_base, virt_end, (uintptr_t)meta, sizeof(*meta))) {
            console::printf("[MODULE] No valid metadata in %s\n", path);
            goto fail_perm;
        }
        if (!cstr_in_range(meta->name, virt_base, virt_end) || !valid_component(meta->name)) {
            console::printf("[MODULE] Invalid module name in %s\n", path);
            goto fail_perm;
        }
        if (is_module_loaded(meta->name)) {
            console::printf("[MODULE] Duplicate module name %s\n", meta->name);
            goto fail_perm;
        }
        if (meta->init && !address_executable_in_sections(sections, section_count, (uintptr_t)meta->init)) {
            console::printf("[MODULE] Init pointer is not executable in %s\n", path);
            goto fail_perm;
        }

        for (uint64_t j = 0; j < pending_symbol_count; j++) {
            kernel_symbol *sym = &pending_symbols[j];
            if (!cstr_in_range(sym->name, virt_base, virt_end) || !sym->name[0]) {
                console::printf("[MODULE] Invalid exported symbol name in %s\n", path);
                goto fail_perm;
            }
            if (!range_contains(virt_base, virt_end, (uintptr_t)sym, sizeof(*sym))) goto fail_perm;
            if (!address_in_kernel(sym->addr) && !range_contains(virt_base, virt_end, sym->addr, 1)) {
                console::printf("[MODULE] Exported symbol %s points outside module/kernel in %s\n", sym->name, path);
                goto fail_perm;
            }
            if (ksym::resolve_symbol(sym->name) != 0) {
                console::printf("[MODULE] Duplicate exported symbol %s in %s\n", sym->name, path);
                goto fail_perm;
            }
            for (uint64_t k = j + 1; k < pending_symbol_count; k++) {
                if (cstr_in_range(pending_symbols[k].name, virt_base, virt_end) &&
                    strcmp(sym->name, pending_symbols[k].name) == 0) {
                    console::printf("[MODULE] Duplicate exported symbol %s in %s\n", sym->name, path);
                    goto fail_perm;
                }
            }
        }

        for (uint64_t page = 0; page < page_count; page++) {
            uintptr_t page_virt = virt_base + page * pmm::PAGE_SIZE;
            uint64_t page_phys = phys_base + page * pmm::PAGE_SIZE;
            if (page_writable[page] && page_executable[page]) {
                console::printf("[MODULE] W+X page rejected in %s page=%d\n", path, page);
                goto fail_perm;
            }

            vmm::PageFlags flags = vmm::PageFlags::Global;
            if (page_writable[page]) flags |= vmm::PageFlags::Write;
            if (!page_executable[page]) flags |= vmm::PageFlags::NX;
            vmm::map_page(page_virt, page_phys, flags, vmm::PageSize::Size4K);
        }

        loaded = (LoadedModule *)heap::kmalloc(sizeof(LoadedModule));
        if (!loaded) goto fail_perm;
        memset(loaded, 0, sizeof(*loaded));
        if (pending_symbol_count) {
            symbols = (kernel_symbol *)heap::kmalloc(sizeof(kernel_symbol) * pending_symbol_count);
            if (!symbols) { heap::kfree(loaded); goto fail_perm; }
        }

        loaded->name = strdup(meta->name);
        loaded->path = strdup(path);
        loaded->base = virt_base;
        loaded->end = virt_end;
        loaded->exec_start = exec_start == UINT64_MAX ? 0 : exec_start;
        loaded->exec_end = exec_end;
        loaded->image_size = total_size;
        loaded->mapped_size = rounded_size;
        loaded->file_size = file->size;
        loaded->section_count = section_count;
        loaded->symbol_count = pending_symbol_count;
        loaded->sections = sections;
        loaded->symbols = symbols;
        if (!loaded->name || !loaded->path) goto fail_loaded;

        for (uint64_t j = 0; j < pending_symbol_count; j++) {
            loaded->symbols[j].addr = pending_symbols[j].addr;
            loaded->symbols[j].name = strdup(pending_symbols[j].name);
            if (!loaded->symbols[j].name) goto fail_loaded;
        }

        for (uint64_t j = 0; j < pending_symbol_count; j++) {
            if (!ksym::export_symbol(loaded->symbols[j].name, loaded->symbols[j].addr)) {
                console::printf("[MODULE] Duplicate exported symbol %s in %s\n", loaded->symbols[j].name, path);
                goto fail_loaded;
            }
        }

        loaded->next = module_list;
        module_list = loaded;
        publish_proc_module(loaded);

        heap::kfree(page_writable);
        heap::kfree(page_executable);

        if (meta->init) {
            console::printf("[MODULE] Initializing %s...\n", meta->name);
            int res = meta->init();
            if (res != 0) {
                console::printf("[MODULE] Failed to load module %s, returned non zero exit code in init: %d.\n",
                                meta->name, res);
            }
        } else {
            console::printf("[MODULE] No init function in %s\n", path);
        }

        heap::kfree(section_sizes);
        heap::kfree(section_addresses);
        return;

    fail_loaded:
        if (loaded) {
            if (loaded->name) heap::kfree((void *)loaded->name);
            if (loaded->path) heap::kfree((void *)loaded->path);
            if (loaded->symbols) {
                for (uint64_t j = 0; j < pending_symbol_count; j++) if (loaded->symbols[j].name) heap::kfree((void *)loaded->symbols[j].name);
                heap::kfree(loaded->symbols);
            }
            heap::kfree(loaded);
        }
    fail_perm:
        if (page_writable) heap::kfree(page_writable);
        if (page_executable) heap::kfree(page_executable);
    fail:
        if (sections) {
            for (uint64_t i = 0; i < section_count; i++) if (sections[i].name) heap::kfree((void *)sections[i].name);
            heap::kfree(sections);
        }
        if (section_sizes) heap::kfree(section_sizes);
        if (section_addresses) heap::kfree(section_addresses);
    fail_unmapped:
        vmm::unmap_range(virt_base, rounded_size);
        module_v_alloc->free(virt_base);
        pmm::free(phys_base, page_count * pmm::PAGE_SIZE);
        console::printf("[MODULE] Failed to load %s\n", path);
    }

}  // namespace module_loader
