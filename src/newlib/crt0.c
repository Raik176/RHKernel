#include <stdlib.h>
#include <stddef.h>

extern int main(int argc, char **argv);

typedef void (*init_func)(void);

extern init_func __preinit_array_start[];
extern init_func __preinit_array_end[];
extern init_func __init_array_start[];
extern init_func __init_array_end[];
extern init_func __fini_array_start[];
extern init_func __fini_array_end[];

void _init(void) {}
void _fini(void) {}

static void run_forward(init_func *start, init_func *end) {
    for (init_func *fn = start; fn < end; fn++) {
        if (*fn) (*fn)();
    }
}

static void run_backward(init_func *start, init_func *end) {
    for (init_func *fn = end; fn > start;) {
        fn--;
        if (*fn) (*fn)();
    }
}

void _start(int argc, char **argv) {
    run_forward(__preinit_array_start, __preinit_array_end);
    run_forward(__init_array_start, __init_array_end);

    int status = main(argc, argv);

    run_backward(__fini_array_start, __fini_array_end);
    exit(status);

    __builtin_unreachable();
}
