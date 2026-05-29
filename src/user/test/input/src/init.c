#include <libc.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <input.h>

static void die(const char *msg) {
    write(1, msg, strlen(msg));
    exit(1);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int fd = open("/dev/input/events", O_RDONLY);
    if (fd < 0) die("open /dev/input/events failed\n");

    if ((sizeof(struct input_event) % sizeof(uint32_t)) != 0) die("input_event layout invalid\n");
    if (INPUT_KEY_RELEASED != 0 || INPUT_KEY_PRESSED != 1 || INPUT_KEY_REPEATED != 2) die("input key values invalid\n");
    close(fd);
    write(1, "input test ok\n", 14);
    return 0;
}
