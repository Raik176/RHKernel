#ifndef _LIBC_H
#define _LIBC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum syscall_number {
    SYSCALL_WRITE = 0,
    SYSCALL_OPEN,
    SYSCALL_READ,
    SYSCALL_CLOSE,
    SYSCALL_YIELD,
    SYSCALL_SLEEP,
    SYSCALL_EXIT,
    SYSCALL_WAIT,
    SYSCALL_DUP2,
    SYSCALL_CLONE,
    SYSCALL_FORK,
    SYSCALL_EXEC,
    SYSCALL_GETPID,
    SYSCALL_MMAP,
    SYSCALL_MUNMAP,
    SYSCALL_BRK,
    SYSCALL_CREATE,
    SYSCALL_UNLINK,
    SYSCALL_RENAME,
    SYSCALL_READDIR,
    SYSCALL_CHDIR,
    SYSCALL_GETCWD,
    SYSCALL_FSCTL,
    SYSCALL_PKEY_MPROTECT,
    SYSCALL_SEEK,
    SYSCALL_STAT,
    SYSCALL_FSTAT,
    SYSCALL_SET_FS_BASE,
    SYSCALL_GET_FS_BASE,
    SYSCALL_SET_GS_BASE,
    SYSCALL_GET_GS_BASE
};

uint64_t __syscall0(uint64_t num);
uint64_t __syscall1(uint64_t num, uint64_t a1);
uint64_t __syscall2(uint64_t num, uint64_t a1, uint64_t a2);
uint64_t __syscall3(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t __syscall4(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
uint64_t __syscall6(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
void __libc_init_main_thread(void);
void *__libc_get_thread_data(void);
void __libc_set_thread_data(void *data);

#ifdef __cplusplus
}
#endif

#endif
