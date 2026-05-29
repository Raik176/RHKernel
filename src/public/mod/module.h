#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*module_init_t)();
typedef int (*module_start_t)();
typedef void (*module_exit_t)();

struct module_metadata {
    const char *name;
    module_init_t init;
    module_start_t start;
    module_exit_t exit;
    const char **deps;
};

#ifdef __cplusplus
}
#endif

#define MODULE_INFO(name_str, init_func, start_func, exit_func, ...)                                  \
    static const char *__module_deps_##init_func[] = {__VA_ARGS__ __VA_OPT__(, )(const char *) 0}; \
    __attribute__((section(".module_info"), visibility("default"),                                 \
                   used)) struct module_metadata __module_metadata = {                             \
        .name = name_str,                                                                          \
        .init = (module_init_t)init_func,                                                          \
        .start = (module_start_t)start_func,                                                       \
        .exit = (module_exit_t)exit_func,                                                          \
        .deps = __module_deps_##init_func}
