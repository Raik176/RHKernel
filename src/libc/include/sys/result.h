#ifndef _SYS_RESULT_H
#define _SYS_RESULT_H

#define LIBC_RESULT_OK 0
#define LIBC_RESULT_PERM (-1)
#define LIBC_RESULT_NOENT (-2)
#define LIBC_RESULT_SRCH (-3)
#define LIBC_RESULT_INTR (-4)
#define LIBC_RESULT_IO (-5)
#define LIBC_RESULT_BADF (-9)
#define LIBC_RESULT_NOMEM (-12)
#define LIBC_RESULT_ACCES (-13)
#define LIBC_RESULT_FAULT (-14)
#define LIBC_RESULT_BUSY (-16)
#define LIBC_RESULT_EXIST (-17)
#define LIBC_RESULT_NODEV (-19)
#define LIBC_RESULT_NOTDIR (-20)
#define LIBC_RESULT_ISDIR (-21)
#define LIBC_RESULT_INVAL (-22)
#define LIBC_RESULT_NFILE (-23)
#define LIBC_RESULT_MFILE (-24)
#define LIBC_RESULT_NOSPC (-28)
#define LIBC_RESULT_NAMETOOLONG (-36)
#define LIBC_RESULT_NOSYS (-38)
#define LIBC_RESULT_RANGE (-34)
#define LIBC_RESULT_OVERFLOW (-75)

#ifdef __cplusplus
extern "C" {
#endif

static inline int libc_result_ok(int result) { return result >= 0; }
static inline int libc_result_error(int result) { return result < 0; }
static inline int libc_result_code(int result) { return result < 0 ? -result : 0; }

#ifdef __cplusplus
}
#endif

#endif
