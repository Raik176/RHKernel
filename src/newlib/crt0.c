#include <stdlib.h>

extern int main(int argc, char **argv);

extern void __libc_init_array(void) __attribute__((weak));
extern void __libc_fini_array(void) __attribute__((weak));

void _init(void) {}
void _fini(void) {}

void _start(int argc, char **argv) {
    if (__libc_init_array) {
        __libc_init_array();
    }

    int status = main(argc, argv);

    if (__libc_fini_array) {
        __libc_fini_array();
    }

    exit(status);

    __builtin_unreachable();
}