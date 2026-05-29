#include <stdlib.h>

typedef void (*init_func)(void);

extern int main(int argc, char **argv);
extern init_func __preinit_array_start[];
extern init_func __preinit_array_end[];
extern init_func __init_array_start[];
extern init_func __init_array_end[];
extern init_func __fini_array_start[];
extern init_func __fini_array_end[];
extern void __cxa_finalize(void *);
extern void __libc_init_main_thread(void);
extern char __eh_frame_start[];
extern char __eh_frame_end[];
extern void __register_frame(const void *);

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
    __libc_init_main_thread();
    if (&__eh_frame_start[0] != &__eh_frame_end[0]) __register_frame(__eh_frame_start);
    run_forward(__preinit_array_start, __preinit_array_end);
    run_forward(__init_array_start, __init_array_end);
    int status = main(argc, argv);
    run_backward(__fini_array_start, __fini_array_end);
    __cxa_finalize(0);
    exit(status);
}
