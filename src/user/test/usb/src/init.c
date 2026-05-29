#include <fcntl.h>
#include <unistd.h>
#include <usb.h>

static int read_any(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[256];
    (void)read(fd, buf, sizeof(buf));
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (sizeof(struct usb_user_device) < 32) return 2;
    if (read_any("/dev/usb/devices") != 0) return 1;
    if (read_any("/dev/usb/hcds") != 0) return 3;
    return 0;
}
