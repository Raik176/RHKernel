#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>



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

struct string_list {
    char **items;
    size_t len;
    size_t cap;
};


static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static void write_fd(int fd, const char *s) { write(fd, s, strlen(s)); }
static void out(const char *s) { write_fd(STDOUT_FILENO, s); }
static void err(const char *s) { write_fd(STDERR_FILENO, s); }

static int is_hidden(const char *name) { return name && name[0] == '.'; }
static int is_dot_entry(const char *name) { return streq(name, ".") || streq(name, ".."); }

static const char *type_suffix(uint32_t type, int classify) {
    if (type == VFS_NODE_DIRECTORY) return "/";
    if (classify && (type == VFS_NODE_CHAR_DEVICE || type == VFS_NODE_BLOCK_DEVICE)) return "#";
    return "";
}

static void print_usage(void) {
    err("usage: ls [-a] [-1] [-F] [-R] [path...]\n");
}

static char *join_path(const char *base, const char *name) {
    size_t blen = strlen(base);
    size_t nlen = strlen(name);
    int need_slash = blen != 0 && base[blen - 1] != '/';
    if (blen > (size_t)-1 - nlen - (need_slash ? 2 : 1)) return nullptr;

    char *path = (char *)malloc(blen + (need_slash ? 1 : 0) + nlen + 1);
    if (!path) return nullptr;

    size_t pos = 0;
    for (size_t i = 0; i < blen; i++) path[pos++] = base[i];
    if (need_slash) path[pos++] = '/';
    for (size_t i = 0; i < nlen; i++) path[pos++] = name[i];
    path[pos] = 0;
    return path;
}

static void free_string_list(struct string_list *list) {
    if (!list) return;
    for (size_t i = 0; i < list->len; i++) free(list->items[i]);
    free(list->items);
    list->items = nullptr;
    list->len = 0;
    list->cap = 0;
}

static int push_string(struct string_list *list, char *s) {
    if (!list || !s) return -1;
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        if (new_cap < list->cap || new_cap > ((size_t)-1 / sizeof(char *))) return -1;
        char **items = (char **)realloc(list->items, new_cap * sizeof(char *));
        if (!items) return -1;
        list->items = items;
        list->cap = new_cap;
    }
    list->items[list->len++] = s;
    return 0;
}

static int read_dirent(const char *path, uint64_t index, struct dirent64 *de) {
    de->name = nullptr;
    de->name_capacity = 0;
    int64_t r = (int64_t)readdir(path, index, de);
    if (r == -1) return -1;

    uint64_t cap64 = de->name_len + 1;
    if (cap64 == 0 || cap64 > (uint64_t)(size_t)-1) return -1;

    char *name = (char *)malloc((size_t)cap64);
    if (!name) return -1;

    de->name = name;
    de->name_capacity = cap64;
    r = (int64_t)readdir(path, index, de);
    if (r < 0 || de->name_len >= de->name_capacity) {
        free(name);
        de->name = nullptr;
        return -1;
    }
    name[de->name_len] = 0;
    return 0;
}

static int list_one(const char *path, int show_all, int one_per_line, int classify, int print_header, int recursive) {
    struct dirent64 de;
    struct string_list dirs = {};
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

        int hidden = is_hidden(de.name);
        if (!show_all && hidden) {
            free(de.name);
            continue;
        }

        if (any && !one_per_line) out("  ");
        out(de.name);
        out(type_suffix(de.type, classify));
        if (one_per_line) out("\n");
        any = 1;

        if (recursive && de.type == VFS_NODE_DIRECTORY && !is_dot_entry(de.name)) {
            char *child = join_path(path, de.name);
            if (!child || push_string(&dirs, child) != 0) {
                free(child);
                err("ls: out of memory\n");
                rc = 1;
            }
        }
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

    if (recursive && idx != 0) {
        for (size_t i = 0; i < dirs.len; i++) {
            out("\n");
            if (list_one(dirs.items[i], show_all, one_per_line, classify, 1, 1) != 0) rc = 1;
        }
    }

    free_string_list(&dirs);
    return rc;
}

int main(int argc, char **argv) {
    int show_all = 0;
    int one_per_line = 0;
    int classify = 0;
    int recursive = 0;
    int first_path = 1;

    for (; first_path < argc; first_path++) {
        const char *a = argv[first_path];
        if (!a || a[0] != '-' || a[1] == '\0') break;
        if (streq(a, "--")) { first_path++; break; }
        for (int i = 1; a[i]; i++) {
            if (a[i] == 'a') show_all = 1;
            else if (a[i] == '1') one_per_line = 1;
            else if (a[i] == 'F') classify = 1;
            else if (a[i] == 'R') recursive = 1;
            else { print_usage(); return 1; }
        }
    }

    int path_count = argc - first_path;
    if (path_count <= 0) return list_one(".", show_all, one_per_line, classify, recursive, recursive);

    int rc = 0;
    for (int i = first_path; i < argc; i++) {
        if (i > first_path) out("\n");
        int header = recursive || path_count > 1;
        if (list_one(argv[i], show_all, one_per_line, classify, header, recursive) != 0) rc = 1;
    }
    return rc;
}
