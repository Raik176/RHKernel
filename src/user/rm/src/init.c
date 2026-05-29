#include <string.h>
#include <unistd.h>

static void es(const char *s) { write(STDERR_FILENO, s, strlen(s)); }

int main(int argc, char **argv) {
    if (argc < 2) { es("usage: rm <path>...\n"); return 1; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) != 0) { es("rm: failed: "); es(argv[i]); es("\n"); rc = 1; }
    }
    return rc;
}
