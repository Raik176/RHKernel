#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void wr(int fd, const char *s) { write(fd, s, strlen(s)); }

static int intcmp(const void *a, const void *b) {
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

static int fail(const char *s) {
    wr(STDERR_FILENO, "libc: ");
    wr(STDERR_FILENO, s);
    wr(STDERR_FILENO, "\n");
    return 1;
}

static int check_memory_primitives(void) {
    unsigned char src[257];
    unsigned char dst[257];
    unsigned char ref[257];

    for (size_t i = 0; i < sizeof(src); i++) src[i] = (unsigned char)(i * 37u + 11u);

    for (size_t doff = 0; doff < 16; doff++) {
        for (size_t soff = 0; soff < 16; soff++) {
            for (size_t len = 0; len < 129; len++) {
                memset(dst, 0xa5, sizeof(dst));
                memset(ref, 0xa5, sizeof(ref));
                memcpy(dst + doff, src + soff, len);
                for (size_t i = 0; i < len; i++) ref[doff + i] = src[soff + i];
                if (memcmp(dst, ref, sizeof(dst)) != 0) return -1;
            }
        }
    }

    for (size_t off = 0; off < 16; off++) {
        for (size_t len = 0; len < 129; len++) {
            memset(dst, 0, sizeof(dst));
            memset(dst + off, 0x5c, len);
            for (size_t i = 0; i < sizeof(dst); i++) {
                unsigned char want = (i >= off && i < off + len) ? 0x5c : 0;
                if (dst[i] != want) return -2;
            }
        }
    }

    for (size_t dst_off = 0; dst_off < 64; dst_off++) {
        for (size_t src_off = 0; src_off < 64; src_off++) {
            for (size_t len = 0; len < 97; len++) {
                for (size_t i = 0; i < sizeof(dst); i++) dst[i] = ref[i] = (unsigned char)(i + 3u);
                memmove(dst + dst_off, dst + src_off, len);
                if (dst_off < src_off) {
                    for (size_t i = 0; i < len; i++) ref[dst_off + i] = (unsigned char)(src_off + i + 3u);
                } else {
                    for (size_t i = len; i; i--) ref[dst_off + i - 1] = (unsigned char)(src_off + i - 1 + 3u);
                }
                if (memcmp(dst, ref, sizeof(dst)) != 0) return -3;
            }
        }
    }

    unsigned char big[2048];
    unsigned char big_ref[2048];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = big_ref[i] = (unsigned char)(i * 19u + 7u);
    memcpy(big + 17, big_ref + 301, 1300);
    for (size_t i = 0; i < 1300; i++) big_ref[17 + i] = (unsigned char)((301 + i) * 19u + 7u);
    if (memcmp(big, big_ref, sizeof(big)) != 0) return -4;

    memset(big + 3, 0xe7, 1700);
    for (size_t i = 0; i < sizeof(big); i++) {
        unsigned char want = (i >= 3 && i < 1703) ? 0xe7 : big_ref[i];
        if (big[i] != want) return -5;
        big[i] = big_ref[i] = (unsigned char)(i * 23u + 5u);
    }

    memmove(big + 349, big + 17, 1300);
    for (size_t i = 1300; i; i--) big_ref[349 + i - 1] = (unsigned char)((17 + i - 1) * 23u + 5u);
    if (memcmp(big, big_ref, sizeof(big)) != 0) return -6;

    memmove(big + 17, big + 349, 1300);
    for (size_t i = 0; i < 1300; i++) big_ref[17 + i] = big_ref[349 + i];
    if (memcmp(big, big_ref, sizeof(big)) != 0) return -7;

    return 0;
}

static int check_string_primitives(void) {
    char text[] = "0123456789abcdef0123456789abcdef";
    if (strlen(text) != 32) return -1;
    if (strnlen(text, 7) != 7 || strnlen(text, 40) != 32) return -2;
    if (memchr(text, 'a', sizeof(text)) != text + 10) return -3;
    if (memchr(text, 0, sizeof(text)) != text + 32) return -4;
    if (memchr(text, 'z', sizeof(text)) != 0) return -5;
    if (memcmp("\xff", "\x7f", 1) <= 0) return -6;
    if (strchr(text, 0) != text + 32) return -7;
    if (strrchr(text, '0') != text + 16) return -8;
    if (strstr(text, "abcdef0") != text + 10) return -9;
    if (strstr(text, "fed") != 0) return -10;
    if (strspn("abc123", "cba") != 3) return -11;
    if (strcspn("abc123", "31") != 3) return -12;
    char pbrk_src[] = "abcdef";
    if (strpbrk(pbrk_src, "xdy") != pbrk_src + 3) return -13;

    char long_text[1536];
    for (size_t i = 0; i < sizeof(long_text) - 1; i++) long_text[i] = 'a';
    long_text[777] = 'z';
    long_text[1021] = 'q';
    long_text[sizeof(long_text) - 1] = 0;
    if (strlen(long_text) != sizeof(long_text) - 1) return -14;
    if (strnlen(long_text, 333) != 333) return -15;
    if (strnlen(long_text, sizeof(long_text) + 10) != sizeof(long_text) - 1) return -16;
    if (memchr(long_text, 'z', sizeof(long_text)) != long_text + 777) return -17;
    if (memchr(long_text, 0, sizeof(long_text)) != long_text + sizeof(long_text) - 1) return -18;
    if (strchr(long_text, 'q') != long_text + 1021) return -19;
    if (strstr(long_text + 700, "azaa") != long_text + 776) return -20;
    return 0;
}


static int check_unlink_open_lifetime(void) {
    const char *path = "/tmp-open-unlink-lifetime";
    unlink(path);

    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    if (write(fd, "abcdef", 6) != 6) return -2;
    if (unlink(path) != 0) return -3;

    int gone = open(path, O_RDONLY);
    if (gone >= 0) {
        close(gone);
        return -4;
    }

    if (lseek(fd, 0, SEEK_SET) != 0) return -5;
    char buf[8];
    memset(buf, 0, sizeof(buf));
    if (read(fd, buf, 6) != 6) return -6;
    if (memcmp(buf, "abcdef", 6) != 0) return -7;
    if (write(fd, "gh", 2) != 2) return -8;
    close(fd);
    return 0;
}

int main(void) {
    int missing = open("/definitely-missing-libc-test-file", O_RDONLY);
    if (missing >= 0) return fail("open missing file did not return a negative error");

    int search_fd = open("/task/self/launch/search", O_RDWR);
    if (search_fd < 0) return fail("open O_RDWR rejected");
    close(search_fd);

    if (malloc(0) != 0) return fail("malloc(0) returned storage");
    if (calloc(0, 8) != 0 || calloc(8, 0) != 0) return fail("calloc zero-size returned storage");

    if (!isalpha('A') || !isdigit('7') || !isspace('\n')) return fail("ctype classification failed");
    if (tolower('Z') != 'z' || toupper('q') != 'Q') return fail("ctype conversion failed");

    char fmt[64];
    if (snprintf(fmt, sizeof(fmt), "%s:%d:%x:%p", "v", -12, 0x2a, (void *)0x1234) <= 0) return fail("snprintf failed");
    if (!strstr(fmt, "v:-12:2a:0x1234")) return fail("snprintf output bad");

    char tokbuf[] = "alpha,beta,gamma";
    char *save = 0;
    if (strcmp(strtok_r(tokbuf, ",", &save), "alpha") != 0) return fail("strtok first failed");
    if (strcmp(strtok_r(0, ",", &save), "beta") != 0) return fail("strtok second failed");

    int values[] = {5, 1, 4, 3, 2};
    qsort(values, 5, sizeof(values[0]), intcmp);
    for (int i = 0; i < 5; i++) if (values[i] != i + 1) return fail("qsort failed");
    int key = 4;
    int *found = (int *)bsearch(&key, values, 5, sizeof(values[0]), intcmp);
    if (!found || *found != 4) return fail("bsearch failed");

    char a[32];
    memset(a, 'x', sizeof(a));
    memcpy(a, "abc", 4);
    if (strcmp(a, "abc") != 0) return fail("string copy failed");
    memmove(a + 1, a, 4);
    if (strcmp(a, "aabc") != 0) return fail("memmove overlap failed");
    if (check_memory_primitives() != 0) return fail("memory primitive edge case failed");
    if (check_string_primitives() != 0) return fail("string primitive edge case failed");
    if (check_unlink_open_lifetime() != 0) return fail("unlink/open file lifetime failed");

    char *p = (char *)malloc(4096);
    if (!p) return fail("malloc failed");
    for (size_t i = 0; i < 4096; i++) p[i] = (char)(i & 255);

    char *q = (char *)realloc(p, 9000);
    if (!q) return fail("realloc grow failed");
    for (size_t i = 0; i < 4096; i++) if ((unsigned char)q[i] != (i & 255)) return fail("realloc preserved bad data");
    free(q);

    char *z = (char *)calloc(128, 8);
    if (!z) return fail("calloc failed");
    for (size_t i = 0; i < 1024; i++) if (z[i] != 0) return fail("calloc not zeroed");
    free(z);

    wr(STDOUT_FILENO, "libc: ok\n");
    return 0;
}
