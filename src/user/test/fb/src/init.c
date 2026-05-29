#include <libc.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stddef.h>
#include <display.h>

static void die(const char *msg) {
    write(1, msg, strlen(msg));
    exit(1);
}

static void send_cmd(int fd, uint32_t cmd, uint32_t flags, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    struct fb_command c = {cmd, flags, x, y, w, h};
    if (write(fd, &c, sizeof(c)) != (int)sizeof(c)) die("fb command failed\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) die("open /dev/fb0 failed\n");

    struct fb_mode mode;
    if (read(fd, &mode, sizeof(mode)) != (int)sizeof(mode)) die("fb mode read failed\n");
    if (!mode.width || !mode.height || !mode.pitch || !mode.framebuffer_bytes) die("fb mode invalid\n");

    send_cmd(fd, FB_CMD_ACQUIRE, 0, 0, 0, 0, 0);
    void *fb = mmap(0, mode.framebuffer_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) die("fb mmap failed\n");

    send_cmd(fd, FB_CMD_DIRTY, 0, 0, 0, mode.width, mode.height);
    send_cmd(fd, FB_CMD_PRESENT, FB_PRESENT_SYNC, 0, 0, 0, 0);
    munmap(fb, mode.framebuffer_bytes);
    send_cmd(fd, FB_CMD_RELEASE, 0, 0, 0, 0, 0);
    close(fd);
    write(1, "fb test ok\n", 11);
    return 0;
}
