#include <libc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/segment.h>

struct libc_thread_data {
    void *self;
    void *cxxabi;
};

static struct libc_thread_data main_thread;

int has_fsgsbase(void) {
    return 0;
}

int set_fs_base(void *base) {
    return (int)(int64_t)__syscall1(SYSCALL_SET_FS_BASE, (uintptr_t)base);
}

void *get_fs_base(void) {
    uint64_t ret = __syscall0(SYSCALL_GET_FS_BASE);
    if ((int64_t)ret < 0) return 0;
    return (void *)(uintptr_t)ret;
}

int set_gs_base(void *base) {
    return (int)(int64_t)__syscall1(SYSCALL_SET_GS_BASE, (uintptr_t)base);
}

void *get_gs_base(void) {
    uint64_t ret = __syscall0(SYSCALL_GET_GS_BASE);
    if ((int64_t)ret < 0) return 0;
    return (void *)(uintptr_t)ret;
}

void *__libc_get_thread_data(void) {
    struct libc_thread_data *data = (struct libc_thread_data *)get_fs_base();
    if (!data || data->self != data) return 0;
    return data->cxxabi;
}

void __libc_set_thread_data(void *data) {
    struct libc_thread_data *tls = (struct libc_thread_data *)get_fs_base();
    if (!tls || tls->self != tls) return;
    tls->cxxabi = data;
}

void __libc_init_main_thread(void) {
    memset(&main_thread, 0, sizeof(main_thread));
    main_thread.self = &main_thread;
    if (set_fs_base(&main_thread) != 0) abort();
}
