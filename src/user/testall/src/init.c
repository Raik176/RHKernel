#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#define SYSCALL_WRITE 0
#define SYSCALL_WAIT 7
#define SYSCALL_FORK 10
#define SYSCALL_EXEC 11
#define SYSCALL_READDIR 19

#define STDOUT 1
#define STDERR 2

struct dirent64 {
    uint32_t inode;
    uint32_t type;
    uint64_t name_len;
    char *name;
    uint64_t name_capacity;
};

static uint64_t sc0(uint64_t n) {
    uint64_t r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(0ULL), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory");
    return r;
}

static uint64_t sc1(uint64_t n, uint64_t a) {
    uint64_t r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(0ULL), "d"(0ULL) : "rcx", "r11", "memory");
    return r;
}

static uint64_t sc3(uint64_t n, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t r;
    asm volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

static size_t slen(const char *s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int cmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void wr(int fd, const char *s) {
    sc3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)s, slen(s));
}

static void out(const char *s) { wr(STDOUT, s); }
static void err(const char *s) { wr(STDERR, s); }

static void dec(uint64_t v) {
    char b[32];
    int n = 0;
    if (!v) {
        b[n++] = '0';
    } else {
        char t[32];
        int m = 0;
        while (v) {
            t[m++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (m) b[n++] = t[--m];
    }
    sc3(SYSCALL_WRITE, STDOUT, (uintptr_t)b, (uint64_t)n);
}

static char *dup_s(const char *s) {
    size_t len = slen(s);
    char *outp = (char *)malloc(len + 1);
    if (!outp) return 0;
    for (size_t i = 0; i <= len; i++) outp[i] = s[i];
    return outp;
}

static char *join_path(const char *dir, const char *name) {
    size_t dlen = slen(dir);
    size_t nlen = slen(name);
    int slash = dlen && dir[dlen - 1] != '/';
    char *path = (char *)malloc(dlen + (size_t)slash + nlen + 1);
    if (!path) return 0;

    size_t p = 0;
    for (size_t i = 0; i < dlen; i++) path[p++] = dir[i];
    if (slash) path[p++] = '/';
    for (size_t i = 0; i < nlen; i++) path[p++] = name[i];
    path[p] = 0;
    return path;
}

static int read_dirent_name(const char *dir, uint64_t index, struct dirent64 *de) {
    de->name = 0;
    de->name_capacity = 0;
    int64_t r = (int64_t)sc3(SYSCALL_READDIR, (uintptr_t)dir, index, (uintptr_t)de);
    if (r == -1) return -1;

    de->name = (char *)malloc(de->name_len + 1);
    if (!de->name) return -1;
    de->name_capacity = de->name_len + 1;

    r = (int64_t)sc3(SYSCALL_READDIR, (uintptr_t)dir, index, (uintptr_t)de);
    if (r < 0) {
        free(de->name);
        de->name = 0;
        return -1;
    }

    return 0;
}

static int add_name(char ***names, size_t *count, size_t *cap, const char *name) {
    if (*count == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 16;
        char **new_names = (char **)realloc(*names, new_cap * sizeof(char *));
        if (!new_names) return -1;
        *names = new_names;
        *cap = new_cap;
    }

    (*names)[*count] = dup_s(name);
    if (!(*names)[*count]) return -1;
    (*count)++;
    return 0;
}

static void sort_names(char **names, size_t count) {
    for (size_t i = 1; i < count; i++) {
        char *tmp = names[i];
        size_t j = i;
        while (j && cmp(names[j - 1], tmp) > 0) {
            names[j] = names[j - 1];
            j--;
        }
        names[j] = tmp;
    }
}

static int load_tests(const char *dir, char ***out_names, size_t *out_count) {
    char **names = 0;
    size_t count = 0;
    size_t cap = 0;

    for (uint64_t i = 0;; i++) {
        struct dirent64 de;
        if (read_dirent_name(dir, i, &de) != 0) break;

        if (de.name[0] != '.' && add_name(&names, &count, &cap, de.name) != 0) {
            free(de.name);
            goto fail;
        }

        free(de.name);
    }

    sort_names(names, count);
    *out_names = names;
    *out_count = count;
    return 0;

fail:
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    return -1;
}

static int run_one(const char *dir, const char *name) {
    char *path = join_path(dir, name);
    if (!path) {
        err("testall: out of memory\n");
        return 1;
    }

    out("[TEST] ");
    out(path);
    out("\n");

    uint64_t pid = sc0(SYSCALL_FORK);
    if (pid == 0) {
        char *argv[] = {path, 0};
        sc3(SYSCALL_EXEC, (uintptr_t)path, (uintptr_t)argv, 0);
        err("testall: exec failed: ");
        err(path);
        err("\n");
        sc1(6, 127);
    }

    if ((int64_t)pid < 0) {
        out("[FAIL] fork failed: ");
        out(path);
        out("\n");
        free(path);
        return 1;
    }

    int status = -1;
    int64_t waited = (int64_t)sc1(SYSCALL_WAIT, (uintptr_t)&status);
    if (waited != (int64_t)pid) {
        out("[FAIL] wait failed: ");
        out(path);
        out("\n");
        free(path);
        return 1;
    }

    if (status != 0) {
        out("[FAIL] ");
        out(path);
        out(" exited ");
        dec((uint64_t)status);
        out("\n");
        free(path);
        return 1;
    }

    out("[ OK ] ");
    out(path);
    out("\n");
    free(path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2 || (argc == 2 && streq(argv[1], "--help"))) {
        err("usage: testall [test-directory]\n");
        return argc > 2 ? 1 : 0;
    }

    const char *dir = argc == 2 ? argv[1] : "test";
    char **names = 0;
    size_t total = 0;

    if (load_tests(dir, &names, &total) != 0) {
        err("testall: failed to load tests from ");
        err(dir);
        err("\n");
        return 1;
    }

    if (!total) {
        err("testall: no tests found in ");
        err(dir);
        err("\n");
        free(names);
        return 1;
    }

    out("testall: running ");
    dec((uint64_t)total);
    out(" test(s) from ");
    out(dir);
    out("\n");

    int failed = 0;
    for (size_t i = 0; i < total; i++) failed += run_one(dir, names[i]);

    out("testall: ");
    dec((uint64_t)(total - (size_t)failed));
    out(" passed, ");
    dec((uint64_t)failed);
    out(" failed\n");

    for (size_t i = 0; i < total; i++) free(names[i]);
    free(names);
    return failed ? 1 : 0;
}
