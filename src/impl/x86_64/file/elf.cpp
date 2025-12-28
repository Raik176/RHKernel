#include "file/elf.h"

#include "file/vfs.h"
#include "memory/heap.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "string.h"

//TODO: correct endianness
namespace elf {
    elf_info load(const char* path) {
        vfs::vfs_node* file = vfs::open(path);
        if (!file) { return {0, 0}; }

        elf_header header;
        vfs::read(file, 0, sizeof(elf_header), &header);

        if (header.magic != ELF_MAGIC) { return {0, 0}; }

        // Create a new address space for this process
        uint64_t user_pml4 = vmm::create_user_address_space();

        for (int i = 0; i < header.ph_count; i++) {
            elf_program_header ph;
            vfs::read(file, header.phoff + (i * header.ph_entry_size), sizeof(ph), &ph);

            if (ph.type == PT_LOAD) {
                // Determine VMM flags
                vmm::PageFlags flags = vmm::PageFlags::User;
                if (ph.flags & PF_W) flags = flags | vmm::PageFlags::Write;
                if (!(ph.flags & PF_X)) flags = flags | vmm::PageFlags::NX;

                // Allocate physical memory for the segment
                // We align the size to page boundaries
                uint64_t page_count = (ph.memsz + (ph.vaddr & 0xFFF) + 0xFFF) / 0x1000;
                uint64_t phys_base = pmm::alloc(page_count * 0x1000);

                // Map the segment into the NEW user address space
                vmm::map_range(ph.vaddr & ~0xFFFULL, phys_base, page_count * 0x1000, flags,
                               user_pml4);

                // Zero out the allocated memory and copy data from file
                // Note: We use p2v to write directly to the physical frames
                memset(p2v(phys_base), 0, page_count * 0x1000);

                // Read from VFS into the offset in physical memory
                // ph.vaddr & 0xFFF handles segments not starting on page boundaries
                void* dest = (void*)((uintptr_t)p2v(phys_base) + (ph.vaddr & 0xFFF));
                vfs::read(file, ph.offset, ph.filesz, dest);
            }
        }

        return {header.entry, user_pml4};
    }
}  // namespace elf