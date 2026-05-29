#ifndef _SYS_FSCTL_H
#define _SYS_FSCTL_H

#ifdef __cplusplus
extern "C" {
#endif

#define FS_CTL_MOUNT 1
#define FS_CTL_UNMOUNT 2
#define FS_CTL_PROBE 3

#define FS_PROBE_NO 0
#define FS_PROBE_YES 1
#define FS_PROBE_ERR -1
#define FS_PROBE_UNSUPPORTED -2

struct fsctl_args {
    const char *source;
    const char *target;
    const char *fstype;
    const char *flags;
};

#ifdef __cplusplus
}
#endif

#endif
