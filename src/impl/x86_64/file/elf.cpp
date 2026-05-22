#include "file/elf.h"

#include "console.h"
#include "file/vfs.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "string.h"

// TODO: correct endianness
namespace elf {
    elf_info load(const char *path) {
        vfs::vfs_node *file = vfs::open(path);
        if (!file) {
            console::printf("[ELF] open failed: %s\n", path ? path : "<null>");
            return {0, 0, 0};
        }

        elf_header header;
        memset(&header, 0, sizeof(header));
        uint64_t got = vfs::read(file, 0, sizeof(elf_header), &header);
        if (got != sizeof(elf_header)) {
            console::printf("[ELF] short header read for %s: got=%d expected=%d file_size=%d\n",
                            path, got, (uint64_t)sizeof(elf_header), file->size);
            return {0, 0, 0};
        }

        if (header.magic != ELF_MAGIC) {
            console::printf("[ELF] bad magic for %s: magic=%x file_size=%d\n", path,
                            (uint64_t)header.magic, file->size);
            return {0, 0, 0};
        }
        if (header.bit_width != 2 || header.machine != 0x3E ||
            header.ph_entry_size != sizeof(elf_program_header)) {
            console::printf("[ELF] unsupported ELF %s: class=%d machine=%x phentsize=%d\n", path,
                            (uint64_t)header.bit_width, (uint64_t)header.machine,
                            (uint64_t)header.ph_entry_size);
            return {0, 0, 0};
        }
        if (header.ph_count == 0 ||
            header.phoff + (uint64_t)header.ph_count * header.ph_entry_size > file->size) {
            console::printf("[ELF] invalid phdr table for %s: phoff=%d count=%d entsz=%d size=%d\n",
                            path, header.phoff, (uint64_t)header.ph_count,
                            (uint64_t)header.ph_entry_size, file->size);
            return {0, 0, 0};
        }

        uint64_t user_pml4 = vmm::create_user_address_space();
        if (!user_pml4) {
            console::printf("[ELF] could not create address space for %s\n", path);
            return {0, 0, 0};
        }

        uint64_t max_user_addr = 0;
        bool saw_load = false;

        for (int i = 0; i < header.ph_count; i++) {
            elf_program_header ph;
            memset(&ph, 0, sizeof(ph));
            got = vfs::read(file, header.phoff + (i * header.ph_entry_size), sizeof(ph), &ph);
            if (got != sizeof(ph)) {
                console::printf("[ELF] short phdr read for %s index=%d got=%d\n", path,
                                (uint64_t)i, got);
                vmm::destroy_user_address_space(user_pml4);
                return {0, 0, 0};
            }

            if (ph.type != PT_LOAD) continue;
            saw_load = true;

            if (ph.memsz < ph.filesz || ph.vaddr + ph.memsz < ph.vaddr ||
                ph.offset + ph.filesz > file->size) {
                console::printf("[ELF] invalid LOAD for %s index=%d off=%d filesz=%d memsz=%d vaddr=%p size=%d\n",
                                path, (uint64_t)i, ph.offset, ph.filesz, ph.memsz, ph.vaddr,
                                file->size);
                vmm::destroy_user_address_space(user_pml4);
                return {0, 0, 0};
            }

            vmm::PageFlags flags = vmm::PageFlags::User;
            if (ph.flags & PF_W) flags = flags | vmm::PageFlags::Write;
            if (!(ph.flags & PF_X)) flags = flags | vmm::PageFlags::NX;

            uint64_t page_count = (ph.memsz + (ph.vaddr & 0xFFF) + 0xFFF) / 0x1000;
            if (page_count == 0) continue;

            uint64_t phys_base = pmm::alloc(page_count * 0x1000);
            if (!phys_base) {
                console::printf("[ELF] OOM loading %s segment=%d pages=%d\n", path, (uint64_t)i,
                                page_count);
                vmm::destroy_user_address_space(user_pml4);
                return {0, 0, 0};
            }

            vmm::map_range(ph.vaddr & ~0xFFFULL, phys_base, page_count * 0x1000, flags,
                           user_pml4);
            memset(p2v(phys_base), 0, page_count * 0x1000);

            void *dest = (void *)((uintptr_t)p2v(phys_base) + (ph.vaddr & 0xFFF));
            got = vfs::read(file, ph.offset, ph.filesz, dest);
            if (got != ph.filesz) {
                console::printf("[ELF] short segment read for %s index=%d got=%d expected=%d\n",
                                path, (uint64_t)i, got, ph.filesz);
                vmm::destroy_user_address_space(user_pml4);
                return {0, 0, 0};
            }

            uint64_t seg_end = ph.vaddr + ph.memsz;
            if (seg_end > max_user_addr) max_user_addr = seg_end;
        }

        if (!saw_load || header.entry == 0) {
            console::printf("[ELF] no loadable image for %s: saw_load=%d entry=%p\n", path,
                            (uint64_t)saw_load, header.entry);
            vmm::destroy_user_address_space(user_pml4);
            return {0, 0, 0};
        }

        return {header.entry, user_pml4, align_up(max_user_addr, pmm::PAGE_SIZE)};
    }
}  // namespace elf