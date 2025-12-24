#include "file/initramfs.h"

#include "console.h"
#include "file/vfs.h"
#include "memory/heap.h"
#include "string.h"
#include "util.h"

namespace initramfs {

    /**
     * @brief Helper to convert a hex string from CPIO header to uint32_t.
     */
    static uint32_t hex_to_int(const char* s, int len) {
        uint32_t res = 0;
        for (int i = 0; i < len; i++) {
            res <<= 4;
            if (s[i] >= '0' && s[i] <= '9')
                res += (s[i] - '0');
            else if (s[i] >= 'A' && s[i] <= 'F')
                res += (s[i] - 'A' + 10);
            else if (s[i] >= 'a' && s[i] <= 'f')
                res += (s[i] - 'a' + 10);
        }
        return res;
    }

    void init(uint64_t mb_phys_addr) {
        vfs::vfs_node* root = vfs::get_root();
        uint8_t* current_ptr = nullptr;

        uint8_t* tags_start = (uint8_t*)p2v(mb_phys_addr);
        for (multiboot_tag* tag = (multiboot_tag*)(tags_start + 8);
             tag->type != MULTIBOOT_TAG_TYPE_END;
             tag = (multiboot_tag*)((uint8_t*)tag + ((tag->size + 7) & ~7))) {
            if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
                multiboot_tag_module* mod = (multiboot_tag_module*)tag;
                if (strcmp(mod->cmdline, "initramfs") == 0) {
                    current_ptr = (uint8_t*)p2v(mod->mod_start);
                    break;
                }
            }
        }

        if (!current_ptr) return;

        while (true) {
            struct cpio_newc_header* header = (struct cpio_newc_header*)current_ptr;
            if (memcmp(header->magic, "070701", 6) != 0) break;

            uint32_t namesize = hex_to_int(header->namesize, 8);
            uint32_t filesize = hex_to_int(header->filesize, 8);
            uint32_t mode = hex_to_int(header->mode, 8);
            char* full_path = (char*)(current_ptr + sizeof(struct cpio_newc_header));

            if (strcmp(full_path, "TRAILER!!!") == 0 && filesize == 0) break;

            if (strcmp(full_path, ".") != 0) {
                vfs::vfs_node* parent = root;
                char* path_copy = (char*)heap::kmalloc(namesize);
                strcpy(path_copy, full_path);

                char* segment = path_copy;
                char* next_slash = nullptr;

                while ((next_slash = strchr(segment, '/'))) {
                    *next_slash = '\0';
                    if (*segment != '\0') {
                        vfs::vfs_node* found = vfs::finddir(parent, segment);
                        if (!found) {
                            found = (vfs::vfs_node*)heap::kmalloc(sizeof(vfs::vfs_node));
                            memset(found, 0, sizeof(vfs::vfs_node));
                            found->name = (char*)heap::kmalloc(strlen(segment) + 1);
                            strcpy(found->name, segment);
                            found->type = vfs::VfsType::VFS_DIRECTORY;

                            found->next = parent->child;
                            parent->child = found;
                        }
                        parent = found;
                    }
                    segment = next_slash + 1;
                }

                if (*segment != '\0') {
                    vfs::vfs_node* node = vfs::finddir(parent, segment);

                    if (!node) {
                        node = (vfs::vfs_node*)heap::kmalloc(sizeof(vfs::vfs_node));
                        memset(node, 0, sizeof(vfs::vfs_node));
                        node->name = (char*)heap::kmalloc(strlen(segment) + 1);
                        strcpy(node->name, segment);
                        node->next = parent->child;
                        parent->child = node;
                    }

                    node->size = filesize;
                    node->inode = hex_to_int(header->ino, 8);
                    node->ptr =
                        (uintptr_t)(current_ptr + align_up(sizeof(cpio_newc_header) + namesize, 4));

                    if ((mode & 0170000) == 0040000) {
                        node->type = vfs::VfsType::VFS_DIRECTORY;
                    } else {
                        node->type = vfs::VfsType::VFS_FILE;
                    }
                }
                heap::kfree(path_copy);
            }

            current_ptr += align_up(sizeof(cpio_newc_header) + namesize, 4) + align_up(filesize, 4);
        }
    }
}  // namespace initramfs