#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#ifdef __cplusplus
extern "C" {
#endif

void _exit(int status) __attribute__((noreturn));
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);
int dup2(int oldfd, int newfd);
pid_t fork(void);
int exec(const char *path, char *const argv[]);
pid_t wait(int *status);
pid_t getpid(void);
int sleep(unsigned ticks);
int sched_yield(void);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char *path);
int rename(const char *old_path, const char *new_path);
int chdir(const char *path);
char *getcwd(char *buf, size_t size);
int isatty(int fd);
int fsctl(int op, void *args);
int readdir(const char *path, uint64_t index, void *dirent);

#ifdef __cplusplus
}
#endif

#endif
