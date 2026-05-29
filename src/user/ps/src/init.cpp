#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>


struct dirent { uint32_t inode; uint32_t type; uint64_t name_len;
    char *name;
    uint64_t name_capacity; };

static void out(int fd, const char *s) { write(fd, s, strlen(s)); }

static void copy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void append(char *dst, size_t cap, const char *src) {
    size_t n = strlen(dst);
    if (n >= cap) return;
    size_t i = 0;
    while (src && src[i] && n + i + 1 < cap) { dst[n + i] = src[i]; i++; }
    dst[n + i] = 0;
}

static int read_all(const char *path, char *buf, size_t cap, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t len = 0;
    while (len + 1 < cap) {
        ssize_t n = read(fd, buf + len, cap - 1 - len);
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = 0;
    if (out_len) *out_len = len;
    return 0;
}

static const char *field(const char *buf, const char *key, char *outv, size_t cap) {
    size_t key_len = strlen(key);
    for (size_t i = 0; buf[i]; i++) {
        size_t j = 0;
        while (j < key_len && buf[i + j] == key[j]) j++;
        if (j != key_len) continue;
        const char *p = buf + i + key_len;
        size_t n = 0;
        while (p[n] && p[n] != '\n' && n + 1 < cap) { outv[n] = p[n]; n++; }
        outv[n] = 0;
        return outv;
    }
    copy(outv, cap, "?");
    return outv;
}

static void print_task(const char *pid) {
    char path[320];
    char buf[2048];
    char ppid[32], cpu[32], state[32], prio[32], quantum[32], type[32];
    copy(path, sizeof(path), "/sys/kernel/debug/scheduler/tasks/");
    append(path, sizeof(path), pid);
    append(path, sizeof(path), "/status");
    if (read_all(path, buf, sizeof(buf), nullptr) != 0) return;
    out(STDOUT_FILENO, pid); out(STDOUT_FILENO, " ");
    out(STDOUT_FILENO, field(buf, "ppid: ", ppid, sizeof(ppid))); out(STDOUT_FILENO, " ");
    out(STDOUT_FILENO, field(buf, "cpu: ", cpu, sizeof(cpu))); out(STDOUT_FILENO, " ");
    out(STDOUT_FILENO, field(buf, "state: ", state, sizeof(state))); out(STDOUT_FILENO, " ");
    out(STDOUT_FILENO, field(buf, "priority: ", prio, sizeof(prio))); out(STDOUT_FILENO, " ");
    out(STDOUT_FILENO, field(buf, "quantum: ", quantum, sizeof(quantum))); out(STDOUT_FILENO, " ");
    out(STDOUT_FILENO, field(buf, "type: ", type, sizeof(type))); out(STDOUT_FILENO, "\n");
}

int main(int, char **) {
    out(STDOUT_FILENO, "pid ppid cpu state priority quantum type\n");
    for (uint64_t i = 0;; i++) {
        dirent de;
        char namebuf[64];
        de.name = namebuf;
        de.name_capacity = sizeof(namebuf);
        int64_t r = (int64_t)readdir("/sys/kernel/debug/scheduler/tasks", i, &de);
        if (r < 0) break;
        print_task(de.name);
    }
    return 0;
}
