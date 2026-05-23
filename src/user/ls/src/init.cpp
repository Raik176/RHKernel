#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#define SYSCALL_WRITE 0
#define SYSCALL_READDIR 19

#define STDOUT 1
#define STDERR 2

#define VFS_NODE_FILE 1u
#define VFS_NODE_DIRECTORY 2u
#define VFS_NODE_CHAR_DEVICE 3u
#define VFS_NODE_BLOCK_DEVICE 4u

struct dirent64 {
    uint32_t inode;
    uint32_t type;
    uint64_t name_len;
    char *name;
    uint64_t name_capacity;
};

static inline uint64_t syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t ret;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3)
                 : "rcx", "r11", "memory");
    return ret;
}

static size_t slen(const char *s) { size_t n = 0; while (s && s[n]) n++; return n; }
static int streq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void write_fd(int fd, const char *s) { syscall3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s)); }
static void out(const char *s) { write_fd(STDOUT, s); }
static void err(const char *s) { write_fd(STDERR, s); }

static int is_hidden(const char *name) { return name && name[0] == '.'; }

static const char *type_suffix(uint32_t type, int classify) {
    if (!classify) return "";
    if (type == VFS_NODE_DIRECTORY) return "/";
    if (type == VFS_NODE_CHAR_DEVICE || type == VFS_NODE_BLOCK_DEVICE) return "#";
    return "";
}

static void print_usage(void) {
    err("usage: ls [-a] [-1] [-F] [path...]\n");
}

static int read_dirent(const char *path, uint64_t index, struct dirent64 *de) {
    de->name = nullptr;
    de->name_capacity = 0;
    int64_t r = (int64_t)syscall3(SYSCALL_READDIR, (uintptr_t)path, index, (uintptr_t)de);
    if (r == -1) return -1;

    uint64_t cap = de->name_len + 1;
    char *name = (char *)malloc(cap ? cap : 1);
    if (!name) return -1;

    de->name = name;
    de->name_capacity = cap;
    r = (int64_t)syscall3(SYSCALL_READDIR, (uintptr_t)path, index, (uintptr_t)de);
    if (r < 0) {
        free(name);
        de->name = nullptr;
        return -1;
    }
    return 0;
}

static int list_one(const char *path, int show_all, int one_per_line, int classify, int print_header) {
    struct dirent64 de;
    uint64_t idx = 0;
    int any = 0;
    int rc = 0;

    if (print_header) {
        out(path);
        out(":\n");
    }

    for (;;) {
        if (read_dirent(path, idx, &de) != 0) break;
        idx++;

        if (!show_all && is_hidden(de.name)) {
            free(de.name);
            continue;
        }

        if (any && !one_per_line) out("  ");
        out(de.name);
        out(type_suffix(de.type, classify));
        if (one_per_line) out("\n");
        any = 1;
        free(de.name);
    }

    if (idx == 0) {
        err("ls: cannot access directory: ");
        err(path);
        err("\n");
        rc = 1;
    } else if (any && !one_per_line) {
        out("\n");
    }

    return rc;
}

int main(int argc, char **argv) {
    int show_all = 0;
    int one_per_line = 0;
    int classify = 0;
    int first_path = 1;

    for (; first_path < argc; first_path++) {
        const char *a = argv[first_path];
        if (!a || a[0] != '-' || a[1] == '\0') break;
        if (streq(a, "--")) { first_path++; break; }
        for (int i = 1; a[i]; i++) {
            if (a[i] == 'a') show_all = 1;
            else if (a[i] == '1') one_per_line = 1;
            else if (a[i] == 'F') classify = 1;
            else { print_usage(); return 1; }
        }
    }

    int path_count = argc - first_path;
    if (path_count <= 0) return list_one(".", show_all, one_per_line, classify, 0);

    int rc = 0;
    for (int i = first_path; i < argc; i++) {
        if (i > first_path) out("\n");
        if (list_one(argv[i], show_all, one_per_line, classify, path_count > 1) != 0) rc = 1;
    }
    return rc;
}
