#include <string.h>
#include <unistd.h>

static void es(const char *s) { write(STDERR_FILENO, s, strlen(s)); }

int main(int argc, char **argv) {
    if (argc != 3) { es("usage: mv <old> <new>\n"); return 1; }
    if (rename(argv[1], argv[2]) != 0) { es("mv: rename failed\n"); return 1; }
    return 0;
}
