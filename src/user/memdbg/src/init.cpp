#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>


static void out(int fd, const char *s) { write(fd, s, strlen(s)); }

static void copy(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void append(char *dst, size_t cap, const char *src) {
    size_t n = strlen(dst);
    size_t i = 0;
    while (src && src[i] && n + i + 1 < cap) { dst[n + i] = src[i]; i++; }
    dst[n + i] = 0;
}

static void utoa_dec(uint64_t v, char *buf, size_t cap) {
    if (!cap) return;
    char tmp[32];
    size_t n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < sizeof(tmp));
    size_t i = 0;
    while (n && i + 1 < cap) buf[i++] = tmp[--n];
    buf[i] = 0;
}

static int read_file(const char *path, char *buf, size_t cap, size_t *out_len) {
    if (!cap) return -1;
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


static uint64_t parse_u64(const char *s, uint64_t fallback) {
    uint64_t value = 0;
    bool any = false;
    while (*s >= '0' && *s <= '9') {
        any = true;
        uint64_t digit = (uint64_t)(*s - '0');
        if (value > (UINT64_MAX - digit) / 10) return fallback;
        value = value * 10 + digit;
        s++;
    }
    return any ? value : fallback;
}

static uint64_t read_scalar(const char *path, uint64_t fallback) {
    char buf[64];
    if (read_file(path, buf, sizeof(buf), nullptr) != 0) return fallback;
    return parse_u64(buf, fallback);
}

static void print_scalar(const char *label, const char *path, int required) {
    char buf[128];
    if (read_file(path, buf, sizeof(buf), nullptr) != 0) {
        if (required) { out(STDERR_FILENO, "memdbg: cannot read "); out(STDERR_FILENO, path); out(STDERR_FILENO, "\n"); }
        return;
    }
    out(STDOUT_FILENO, label);
    out(STDOUT_FILENO, ": ");
    out(STDOUT_FILENO, buf);
    if (!buf[0] || buf[strlen(buf) - 1] != '\n') out(STDOUT_FILENO, "\n");
}

static void print_order(uint64_t order) {
    char nbuf[32], blocks_path[128], bytes_path[128], blocks[64], bytes[64];
    utoa_dec(order, nbuf, sizeof(nbuf));

    copy(blocks_path, sizeof(blocks_path), "/sys/kernel/debug/mm/pmm/buddy/order");
    append(blocks_path, sizeof(blocks_path), nbuf);
    append(blocks_path, sizeof(blocks_path), "/blocks");

    copy(bytes_path, sizeof(bytes_path), "/sys/kernel/debug/mm/pmm/buddy/order");
    append(bytes_path, sizeof(bytes_path), nbuf);
    append(bytes_path, sizeof(bytes_path), "/bytes");

    if (read_file(blocks_path, blocks, sizeof(blocks), nullptr) != 0) return;
    if (read_file(bytes_path, bytes, sizeof(bytes), nullptr) != 0) return;

    out(STDOUT_FILENO, "  order"); out(STDOUT_FILENO, nbuf); out(STDOUT_FILENO, ": blocks=");
    out(STDOUT_FILENO, blocks);
    if (blocks[0] && blocks[strlen(blocks) - 1] == '\n') out(STDOUT_FILENO, "           bytes=");
    else out(STDOUT_FILENO, " bytes=");
    out(STDOUT_FILENO, bytes);
    if (!bytes[0] || bytes[strlen(bytes) - 1] != '\n') out(STDOUT_FILENO, "\n");
}

int main(int, char **) {
    out(STDOUT_FILENO, "memory:\n");
    print_scalar("  system_bytes", "/proc/mem/system", 1);
    print_scalar("  managed_bytes", "/proc/mem/managed", 1);
    print_scalar("  free_bytes", "/proc/mem/available", 1);
    print_scalar("  used_bytes", "/proc/mem/used", 1);
    print_scalar("  physical_limit_bytes", "/proc/mem/physical_limit", 1);

    out(STDOUT_FILENO, "vmm:\n");
    print_scalar("  direct_map_bytes", "/sys/kernel/mm/vmm/direct_map_bytes", 1);
    print_scalar("  page_table_bytes", "/sys/kernel/mm/vmm/page_table_bytes", 1);
    print_scalar("  phys_map_base", "/sys/kernel/mm/vmm/phys_map_base", 1);
    print_scalar("  paging_levels", "/sys/kernel/mm/vmm/paging_levels", 1);
    print_scalar("  virtual_address_bits", "/sys/kernel/mm/vmm/virtual_address_bits", 1);
    print_scalar("  page_1g_supported", "/sys/kernel/mm/vmm/page_1g_supported", 1);
    print_scalar("  demand_zero_supported", "/sys/kernel/mm/vmm/demand_zero_supported", 1);
    print_scalar("  guard_page_supported", "/sys/kernel/mm/vmm/guard_page_supported", 1);
    print_scalar("  pat_supported", "/sys/kernel/mm/vmm/pat_supported", 1);
    print_scalar("  wc_supported", "/sys/kernel/mm/vmm/write_combining_supported", 1);
    print_scalar("  nx_supported", "/sys/kernel/mm/vmm/nx_supported", 1);

    out(STDOUT_FILENO, "pmm:\n");
    print_scalar("  page_size", "/sys/kernel/mm/pmm/page_size", 1);
    print_scalar("  max_order", "/sys/kernel/mm/pmm/max_order", 1);
    print_scalar("  ap_trampoline_phys", "/sys/kernel/mm/pmm/ap_trampoline_phys", 1);
    print_scalar("  deferred_spans", "/sys/kernel/debug/mm/pmm/deferred_spans", 0);

    out(STDOUT_FILENO, "heap:\n");
    print_scalar("  slab_page_bytes", "/sys/kernel/mm/heap/slab_page_bytes", 0);
    print_scalar("  large_alloc_bytes", "/sys/kernel/mm/heap/large_alloc_bytes", 0);

    out(STDOUT_FILENO, "buddy_orders:\n");
    uint64_t max_order = read_scalar("/sys/kernel/mm/pmm/max_order", 14);
    for (uint64_t i = 0; i <= max_order; i++) print_order(i);
    return 0;
}
