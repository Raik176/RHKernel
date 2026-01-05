#include <stdlib.h>

extern int main(int argc, char **argv);
extern void __libc_init_array(void);

void _init(void) {}
void _fini(void) {}

void _start(int argc, char **argv) {
    __libc_init_array();

    exit(main(argc, argv));

    __builtin_unreachable();
}
