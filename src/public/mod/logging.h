#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum log_level { LOG_INFO, LOG_WARN, LOG_ERR };

void klog(enum log_level lvl, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
