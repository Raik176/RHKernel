#ifndef _SYS_SEGMENT_H
#define _SYS_SEGMENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int set_fs_base(void *base);
void *get_fs_base(void);
int set_gs_base(void *base);
void *get_gs_base(void);
int has_fsgsbase(void);

#ifdef __cplusplus
}
#endif

#endif
