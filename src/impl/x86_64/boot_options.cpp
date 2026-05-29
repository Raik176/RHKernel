#include "boot_options.h"

#include "multiboot2.h"
#include "string.h"
#include "util.h"

namespace boot_options {
    static options g_options;

    static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    static bool set_error(const char *s) {
        g_options.valid = false;
        strncpy(g_options.error, s, sizeof(g_options.error) - 1);
        g_options.error[sizeof(g_options.error) - 1] = 0;
        return false;
    }

    static bool is_hex(char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    static char lower_hex(char c) {
        if (c >= 'A' && c <= 'F') return (char)(c - 'A' + 'a');
        return c;
    }

    static bool valid_gpt_uuid(const char *s, uint64_t len) {
        if (len != 36) return false;
        for (uint64_t i = 0; i < len; i++) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (s[i] != '-') return false;
            } else if (!is_hex(s[i])) {
                return false;
            }
        }
        return true;
    }

    static bool valid_mbr_disk_uuid(const char *s, uint64_t len) {
        if (len != 8) return false;
        for (uint64_t i = 0; i < len; i++) if (!is_hex(s[i])) return false;
        return true;
    }

    static bool valid_mbr_part_uuid(const char *s, uint64_t len) {
        if (len != 11 || s[8] != '-') return false;
        for (uint64_t i = 0; i < 8; i++) if (!is_hex(s[i])) return false;
        return is_hex(s[9]) && is_hex(s[10]);
    }

    static bool copy_token(char *dst, uint64_t dst_size, const char *src, uint64_t len) {
        if (!dst || dst_size == 0 || len >= dst_size) return false;
        memcpy(dst, src, len);
        dst[len] = 0;
        return true;
    }

    static bool copy_disk_uuid(char *dst, const char *src, uint64_t len) {
        if (!valid_gpt_uuid(src, len) && !valid_mbr_disk_uuid(src, len)) return false;
        for (uint64_t i = 0; i < len; i++) dst[i] = lower_hex(src[i]);
        dst[len] = 0;
        return true;
    }

    static bool copy_part_uuid(char *dst, const char *src, uint64_t len) {
        if (!valid_gpt_uuid(src, len) && !valid_mbr_part_uuid(src, len)) return false;
        for (uint64_t i = 0; i < len; i++) dst[i] = lower_hex(src[i]);
        dst[len] = 0;
        return true;
    }

    static bool set_root(const char *value, uint64_t len) {
        if (g_options.root != root_kind::none) return set_error("duplicate root option");
        if (len > 5 && strncmp(value, "disk:", 5) == 0) {
            if (!copy_disk_uuid(g_options.root_uuid, value + 5, len - 5)) return set_error("bad root disk uuid");
            g_options.root = root_kind::disk;
            return true;
        }
        if (len > 5 && strncmp(value, "part:", 5) == 0) {
            if (!copy_part_uuid(g_options.root_uuid, value + 5, len - 5)) return set_error("bad root partition uuid");
            g_options.root = root_kind::part;
            return true;
        }
        return set_error("bad root option");
    }

    static bool valid_fs_name(const char *s, uint64_t len) {
        if (len == 0 || len >= sizeof(g_options.rootfs)) return false;
        for (uint64_t i = 0; i < len; i++) {
            char c = s[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!ok) return false;
        }
        return true;
    }

    static bool set_rootfs(const char *value, uint64_t len) {
        if (g_options.rootfs[0]) return set_error("duplicate rootfs option");
        if (!valid_fs_name(value, len)) return set_error("bad rootfs option");
        return copy_token(g_options.rootfs, sizeof(g_options.rootfs), value, len);
    }

    static bool set_rootmode(const char *value, uint64_t len) {
        if (g_options.rootmode[0]) return set_error("duplicate rootmode option");
        if (len == 1 && value[0] == 'r') return copy_token(g_options.rootmode, sizeof(g_options.rootmode), "r", 1);
        if (len == 2 && value[0] == 'r' && value[1] == 'w') return copy_token(g_options.rootmode, sizeof(g_options.rootmode), "rw", 2);
        return set_error("bad rootmode option");
    }

    static bool parse_option(const char *tok, uint64_t len) {
        if (len == 0) return true;
        if (len >= 5 && strncmp(tok, "root=", 5) == 0) return set_root(tok + 5, len - 5);
        if (len >= 7 && strncmp(tok, "rootfs=", 7) == 0) return set_rootfs(tok + 7, len - 7);
        if (len >= 9 && strncmp(tok, "rootmode=", 9) == 0) return set_rootmode(tok + 9, len - 9);
        bool has_equal = false;
        for (uint64_t i = 0; i < len; i++) if (tok[i] == '=') { has_equal = true; break; }
        if (!has_equal && tok[0] == '/') return true;
        return set_error("unknown boot option");
    }

    static void parse_cmdline(const char *cmdline) {
        const char *p = cmdline;
        while (*p) {
            while (is_space(*p)) p++;
            const char *tok = p;
            while (*p && !is_space(*p)) p++;
            if (!parse_option(tok, (uint64_t)(p - tok))) return;
        }
    }

    void init(uint64_t mb_phys_addr) {
        memset(&g_options, 0, sizeof(g_options));
        g_options.valid = true;
        uint8_t *tags_start = (uint8_t *)p2v(mb_phys_addr);
        for (multiboot_tag *tag = (multiboot_tag *)(tags_start + 8);
             tag->type != MULTIBOOT_TAG_TYPE_END;
             tag = (multiboot_tag *)((uint8_t *)tag + ((tag->size + 7) & ~7))) {
            if (tag->type == MULTIBOOT_TAG_TYPE_CMDLINE) {
                parse_cmdline((const char *)tag + sizeof(multiboot_tag));
                if (g_options.valid && !g_options.rootmode[0]) strcpy(g_options.rootmode, "r");
                return;
            }
        }
        if (g_options.valid && !g_options.rootmode[0]) strcpy(g_options.rootmode, "r");
    }

    const options &get() { return g_options; }
}
