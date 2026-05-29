#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#ifdef __cplusplus
extern "C" {
#endif

int mount(const char *source, const char *target, const char *fstype, const char *flags);
int unmount(const char *target);

#ifdef __cplusplus
}
#endif

#endif
