#include <fcntl.h>
#include <libc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/fsctl.h>
#include <sys/mount.h>
#include <sys/result.h>
#include <sys/stat.h>
#include <unistd.h>


static int ret_int(uint64_t r) { return (int)(int64_t)r; }

static ssize_t ret_ssize(uint64_t r) { return (ssize_t)(int64_t)r; }

void _exit(int status) {
    __syscall1(SYSCALL_EXIT, (uint64_t)status);
    __builtin_unreachable();
}

void exit(int status) {
    _exit(status);
}

void abort(void) {
    _exit(127);
}

ssize_t write(int fd, const void *buf, size_t count) {
    return ret_ssize(__syscall3(SYSCALL_WRITE, (uint64_t)fd, (uintptr_t)buf, count));
}

ssize_t read(int fd, void *buf, size_t count) {
    return ret_ssize(__syscall3(SYSCALL_READ, (uint64_t)fd, (uintptr_t)buf, count));
}

int close(int fd) { return ret_int(__syscall1(SYSCALL_CLOSE, (uint64_t)fd)); }

int open(const char *path, int flags, ...) {
    const int access = flags & O_ACCMODE;
    if (access != O_RDONLY && access != O_WRONLY && access != O_RDWR) return LIBC_RESULT_INVAL;
    if (flags & ~(O_ACCMODE | O_CREAT | O_TRUNC)) return LIBC_RESULT_INVAL;

    int kernel_flags = flags & (O_CREAT | O_TRUNC);
    return ret_int(__syscall3(SYSCALL_OPEN, (uintptr_t)path, (uint64_t)kernel_flags, 0));
}

int create(const char *path) { return ret_int(__syscall1(SYSCALL_CREATE, (uintptr_t)path)); }
int unlink(const char *path) { return ret_int(__syscall1(SYSCALL_UNLINK, (uintptr_t)path)); }
int rename(const char *old_path, const char *new_path) { return ret_int(__syscall2(SYSCALL_RENAME, (uintptr_t)old_path, (uintptr_t)new_path)); }
int chdir(const char *path) { return ret_int(__syscall1(SYSCALL_CHDIR, (uintptr_t)path)); }
int dup2(int oldfd, int newfd) { return ret_int(__syscall2(SYSCALL_DUP2, (uint64_t)oldfd, (uint64_t)newfd)); }
pid_t fork(void) { return (pid_t)ret_int(__syscall0(SYSCALL_FORK)); }
int exec(const char *path, char *const argv[]) { return ret_int(__syscall2(SYSCALL_EXEC, (uintptr_t)path, (uintptr_t)argv)); }
pid_t wait(int *status) { return (pid_t)ret_int(__syscall1(SYSCALL_WAIT, (uintptr_t)status)); }
pid_t getpid(void) { return (pid_t)ret_int(__syscall0(SYSCALL_GETPID)); }
int sleep(unsigned ticks) { return ret_int(__syscall1(SYSCALL_SLEEP, ticks)); }
int sched_yield(void) { return ret_int(__syscall0(SYSCALL_YIELD)); }
off_t lseek(int fd, off_t offset, int whence) { return (off_t)ret_ssize(__syscall3(SYSCALL_SEEK, (uint64_t)fd, (uint64_t)offset, (uint64_t)whence)); }
int stat(const char *path, struct stat *st) { return ret_int(__syscall2(SYSCALL_STAT, (uintptr_t)path, (uintptr_t)st)); }
int fstat(int fd, struct stat *st) { return ret_int(__syscall2(SYSCALL_FSTAT, (uint64_t)fd, (uintptr_t)st)); }
int lstat(const char *path, struct stat *st) { return stat(path, st); }
int fsctl(int op, void *args) { return ret_int(__syscall2(SYSCALL_FSCTL, (uint64_t)op, (uintptr_t)args)); }

int mount(const char *source, const char *target, const char *fstype, const char *flags) {
    if (!source || !target) return LIBC_RESULT_INVAL;
    struct fsctl_args args = { source, target, fstype, flags };
    return fsctl(FS_CTL_MOUNT, &args);
}

int unmount(const char *target) {
    if (!target) return LIBC_RESULT_INVAL;
    struct fsctl_args args = { 0, target, 0, 0 };
    return fsctl(FS_CTL_UNMOUNT, &args);
}
int readdir(const char *path, uint64_t index, void *dirent) { return ret_int(__syscall3(SYSCALL_READDIR, (uintptr_t)path, index, (uintptr_t)dirent)); }

char *getcwd(char *buf, size_t size) {
    if (!buf || size == 0) return 0;
    if ((int64_t)__syscall2(SYSCALL_GETCWD, (uintptr_t)buf, size) < 0) return 0;
    return buf;
}

int isatty(int fd) { return fd >= 0 && fd <= 2; }

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (length == 0) return MAP_FAILED;
    uint64_t r = __syscall6(SYSCALL_MMAP, (uintptr_t)addr, length, (uint64_t)prot, (uint64_t)flags, (uint64_t)fd, (uint64_t)offset);
    if ((int64_t)r < 0) return MAP_FAILED;
    return (void *)(uintptr_t)r;
}

int munmap(void *addr, size_t length) {
    return ret_int(__syscall2(SYSCALL_MUNMAP, (uintptr_t)addr, length));
}
