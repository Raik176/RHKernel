#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>

struct stat {
    uint64_t inode;
    uint32_t type;
    uint64_t size;
};

#ifdef __cplusplus
extern "C" {
#endif

int stat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int lstat(const char *path, struct stat *st);

#ifdef __cplusplus
}
#endif

#endif
