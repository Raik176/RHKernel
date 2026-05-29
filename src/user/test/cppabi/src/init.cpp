#include <stdint.h>
#include <string.h>
#include <sys/segment.h>
#include <unistd.h>

static void out(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }
static void err(const char *s) { write(STDERR_FILENO, s, strlen(s)); }

static int fail(const char *s) {
    err("cppabi: ");
    err(s);
    err("\n");
    return 1;
}

static int global_ctor_count;
static int global_dtor_count;

struct global_probe {
    global_probe() { global_ctor_count++; }
    ~global_probe() { global_dtor_count++; }
};

static global_probe global;

struct base {
    virtual ~base() {}
    virtual int value() const { return 7; }
};

struct left : virtual base {
    int l;
    left() : l(11) {}
    int value() const override { return 17; }
};

struct right : virtual base {
    int r;
    right() : r(13) {}
    int value() const override { return 19; }
};

struct child : left, right {
    int c;
    child() : c(23) {}
    int value() const override { return 42; }
};

struct other : base {
    int value() const override { return 5; }
};

struct local_probe {
    int value;
    local_probe() : value(99) {}
};

static int local_static_value() {
    static local_probe probe;
    return probe.value;
}

static int cleanup_count;
struct cleanup_probe {
    ~cleanup_probe() { cleanup_count++; }
};

static int test_exception() {
    try {
        cleanup_probe probe;
        (void)probe;
        throw 1234;
    } catch (int v) {
        if (v != 1234 || cleanup_count != 1) return 0;
    }

    try {
        throw child();
    } catch (base &b) {
        if (b.value() != 42) return 0;
    }

    try {
        throw child();
    } catch (...) {
        return 1;
    }

    return 0;
}

int main() {
    if (global_ctor_count != 1) return fail("global constructor did not run");
    if (global_dtor_count != 0) return fail("global destructor ran early");

    void *fs = get_fs_base();
    if (!fs) return fail("fs base is unset");
    if (set_fs_base(fs) != 0 || get_fs_base() != fs) return fail("fs base roundtrip failed");

    base *b = new child;
    child *c = dynamic_cast<child *>(b);
    left *l = dynamic_cast<left *>(b);
    right *r = dynamic_cast<right *>(b);
    other *o = dynamic_cast<other *>(b);
    if (!c || !l || !r || o || c->value() != 42) return fail("dynamic_cast failed");
    if (dynamic_cast<base *>(l) != b || dynamic_cast<base *>(r) != b) return fail("virtual base cast failed");
    delete b;

    if (local_static_value() != 99) return fail("local static initialization failed");
    if (!test_exception()) return fail("exception catch failed");

    out("cppabi: ok\n");
    return 0;
}
