#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY 0x0
#define O_WRONLY 0x1
#define O_RDWR 0x2
#define O_ACCMODE 0x3
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_APPEND 0x400

#ifdef __cplusplus
extern "C" {
#endif

int open(const char *path, int flags, ...);
int create(const char *path);

#ifdef __cplusplus
}
#endif

#endif
