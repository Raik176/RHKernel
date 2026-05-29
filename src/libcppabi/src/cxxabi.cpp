#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" void *__libc_get_thread_data(void);
extern "C" void __libc_set_thread_data(void *);

typedef intptr_t sptr;
typedef unsigned _Unwind_Word __attribute__((mode(__word__)));
typedef unsigned _Unwind_Ptr __attribute__((mode(__pointer__)));

enum _Unwind_Reason_Code {
    _URC_NO_REASON = 0,
    _URC_FOREIGN_EXCEPTION_CAUGHT = 1,
    _URC_FATAL_PHASE2_ERROR = 2,
    _URC_FATAL_PHASE1_ERROR = 3,
    _URC_NORMAL_STOP = 4,
    _URC_END_OF_STACK = 5,
    _URC_HANDLER_FOUND = 6,
    _URC_INSTALL_CONTEXT = 7,
    _URC_CONTINUE_UNWIND = 8
};

enum _Unwind_Action {
    _UA_SEARCH_PHASE = 1,
    _UA_CLEANUP_PHASE = 2,
    _UA_HANDLER_FRAME = 4,
    _UA_FORCE_UNWIND = 8,
    _UA_END_OF_STACK = 16
};

struct _Unwind_Exception;
typedef void (*_Unwind_Exception_Cleanup_Fn)(_Unwind_Reason_Code, _Unwind_Exception *);

struct _Unwind_Exception {
    uint64_t exception_class;
    _Unwind_Exception_Cleanup_Fn exception_cleanup;
    uint64_t private_1;
    uint64_t private_2;
};

struct _Unwind_Context;

extern "C" _Unwind_Reason_Code _Unwind_RaiseException(_Unwind_Exception *);
extern "C" void _Unwind_Resume(_Unwind_Exception *);
extern "C" void _Unwind_Resume_or_Rethrow(_Unwind_Exception *);
extern "C" void _Unwind_DeleteException(_Unwind_Exception *);
extern "C" _Unwind_Word _Unwind_GetGR(_Unwind_Context *, int);
extern "C" void _Unwind_SetGR(_Unwind_Context *, int, _Unwind_Word);
extern "C" _Unwind_Ptr _Unwind_GetIP(_Unwind_Context *);
extern "C" void _Unwind_SetIP(_Unwind_Context *, _Unwind_Ptr);
extern "C" _Unwind_Ptr _Unwind_GetRegionStart(_Unwind_Context *);
extern "C" _Unwind_Ptr _Unwind_GetLanguageSpecificData(_Unwind_Context *);
extern "C" _Unwind_Ptr _Unwind_GetTextRelBase(_Unwind_Context *);
extern "C" _Unwind_Ptr _Unwind_GetDataRelBase(_Unwind_Context *);

namespace std {
class type_info {
public:
    virtual ~type_info();
    const char *name() const { return __name; }
    bool operator==(const type_info &rhs) const;
    bool operator!=(const type_info &rhs) const { return !(*this == rhs); }
protected:
    explicit type_info(const char *name) : __name(name) {}
private:
    const char *__name;
    type_info(const type_info &);
    type_info &operator=(const type_info &);
};

type_info::~type_info() {}

bool type_info::operator==(const type_info &rhs) const {
    return this == &rhs || __name == rhs.__name || strcmp(__name, rhs.__name) == 0;
}
}

namespace __cxxabiv1 {
class __class_type_info : public std::type_info {
public:
    explicit __class_type_info(const char *name) : std::type_info(name) {}
    virtual ~__class_type_info();
};

class __si_class_type_info : public __class_type_info {
public:
    explicit __si_class_type_info(const char *name, const __class_type_info *base)
        : __class_type_info(name), __base_type(base) {}
    virtual ~__si_class_type_info();
    const __class_type_info *__base_type;
};

struct __base_class_type_info {
    const __class_type_info *__base_type;
    long __offset_flags;
    enum { __virtual_mask = 0x1, __public_mask = 0x2, __offset_shift = 8 };
};

class __vmi_class_type_info : public __class_type_info {
public:
    explicit __vmi_class_type_info(const char *name, unsigned int flags, unsigned int base_count)
        : __class_type_info(name), __flags(flags), __base_count(base_count) {}
    virtual ~__vmi_class_type_info();
    unsigned int __flags;
    unsigned int __base_count;
    __base_class_type_info __base_info[1];
};

class __fundamental_type_info : public std::type_info { public: explicit __fundamental_type_info(const char *n) : type_info(n) {} virtual ~__fundamental_type_info(); };
class __array_type_info : public std::type_info { public: explicit __array_type_info(const char *n) : type_info(n) {} virtual ~__array_type_info(); };
class __function_type_info : public std::type_info { public: explicit __function_type_info(const char *n) : type_info(n) {} virtual ~__function_type_info(); };
class __enum_type_info : public std::type_info { public: explicit __enum_type_info(const char *n) : type_info(n) {} virtual ~__enum_type_info(); };
class __pbase_type_info : public std::type_info {
public:
    explicit __pbase_type_info(const char *n, unsigned int flags, const std::type_info *pointee)
        : type_info(n), __flags(flags), __pointee(pointee) {}
    virtual ~__pbase_type_info();
    unsigned int __flags;
    const std::type_info *__pointee;
};
class __pointer_type_info : public __pbase_type_info { public: explicit __pointer_type_info(const char *n, unsigned int f, const std::type_info *p) : __pbase_type_info(n, f, p) {} virtual ~__pointer_type_info(); };
class __pointer_to_member_type_info : public __pbase_type_info { public: explicit __pointer_to_member_type_info(const char *n, unsigned int f, const std::type_info *p, const __class_type_info *c) : __pbase_type_info(n, f, p), __context(c) {} virtual ~__pointer_to_member_type_info(); const __class_type_info *__context; };

__class_type_info::~__class_type_info() {}
__si_class_type_info::~__si_class_type_info() {}
__vmi_class_type_info::~__vmi_class_type_info() {}
__fundamental_type_info::~__fundamental_type_info() {}
__array_type_info::~__array_type_info() {}
__function_type_info::~__function_type_info() {}
__enum_type_info::~__enum_type_info() {}
__pbase_type_info::~__pbase_type_info() {}
__pointer_type_info::~__pointer_type_info() {}
__pointer_to_member_type_info::~__pointer_to_member_type_info() {}
}

extern "C" { void *__dso_handle = &__dso_handle; }

static const uint64_t exception_class = 0x53594d432b2b0000ULL;

typedef void (*dtor_fn)(void *);
struct atexit_entry { dtor_fn fn; void *arg; void *dso; };
static atexit_entry *atexit_entries;
static size_t atexit_count;
static size_t atexit_capacity;

extern "C" int __cxa_atexit(dtor_fn fn, void *arg, void *dso) {
    if (!fn) return -1;
    if (atexit_count == atexit_capacity) {
        size_t next = atexit_capacity ? atexit_capacity * 2 : 32;
        atexit_entry *entries = (atexit_entry *)malloc(next * sizeof(*entries));
        if (!entries) return -1;
        if (atexit_entries) memcpy(entries, atexit_entries, atexit_count * sizeof(*entries));
        atexit_entries = entries;
        atexit_capacity = next;
    }
    atexit_entries[atexit_count++] = {fn, arg, dso};
    return 0;
}

extern "C" void __cxa_finalize(void *dso) {
    for (size_t i = atexit_count; i > 0; i--) {
        atexit_entry *entry = &atexit_entries[i - 1];
        if (!entry->fn) continue;
        if (dso && entry->dso != dso) continue;
        dtor_fn fn = entry->fn;
        void *arg = entry->arg;
        entry->fn = 0;
        fn(arg);
    }
}

extern "C" int __cxa_guard_acquire(uint64_t *guard) {
    return ((*guard & 1) == 0) && __sync_bool_compare_and_swap(guard, 0, 2);
}

extern "C" void __cxa_guard_release(uint64_t *guard) {
    __sync_synchronize();
    *guard = 1;
}

extern "C" void __cxa_guard_abort(uint64_t *guard) { *guard = 0; }

static void write_error(const char *msg) {
    size_t len = 0;
    while (msg[len]) len++;
    write(STDERR_FILENO, msg, len);
}

static void print_abort(const char *msg) {
    write_error(msg);
    abort();
}

extern "C" void __cxa_pure_virtual() { print_abort("libcppabi: pure virtual call\n"); }
extern "C" void __cxa_deleted_virtual() { print_abort("libcppabi: deleted virtual call\n"); }

extern "C" void *_ZTVN10__cxxabiv117__class_type_infoE[];
extern "C" void *_ZTVN10__cxxabiv120__si_class_type_infoE[];
extern "C" void *_ZTVN10__cxxabiv121__vmi_class_type_infoE[];

static bool same_type(const std::type_info *a, const std::type_info *b) {
    return a && b && (*a == *b);
}

static bool is_si_type(const std::type_info *type) {
    return type && *(void *const *)type == &_ZTVN10__cxxabiv120__si_class_type_infoE[2];
}

static bool is_vmi_type(const std::type_info *type) {
    return type && *(void *const *)type == &_ZTVN10__cxxabiv121__vmi_class_type_infoE[2];
}

struct base_match {
    char *first;
    unsigned count;
    bool public_path;
};

static char *base_ptr(char *obj, void **vptr, const __cxxabiv1::__base_class_type_info *base) {
    long flags = base->__offset_flags & 0xff;
    long offset = base->__offset_flags >> __cxxabiv1::__base_class_type_info::__offset_shift;
    if (flags & __cxxabiv1::__base_class_type_info::__virtual_mask) {
        ptrdiff_t virtual_offset = *(ptrdiff_t *)((char *)vptr + offset);
        return obj + virtual_offset;
    }
    return obj + offset;
}

static void walk_bases(const std::type_info *cur, char *ptr, const std::type_info *wanted,
                       bool public_path, base_match *out) {
    if (same_type(cur, wanted)) {
        if (!out->first) out->first = ptr;
        out->count++;
        if (public_path) out->public_path = true;
    }

    if (is_si_type(cur)) {
        const __cxxabiv1::__si_class_type_info *si = (const __cxxabiv1::__si_class_type_info *)cur;
        walk_bases(si->__base_type, ptr, wanted, public_path, out);
        return;
    }

    if (!is_vmi_type(cur)) return;

    const __cxxabiv1::__vmi_class_type_info *vmi = (const __cxxabiv1::__vmi_class_type_info *)cur;
    void **vptr = *(void ***)ptr;
    for (unsigned i = 0; i < vmi->__base_count; i++) {
        const __cxxabiv1::__base_class_type_info *base = &vmi->__base_info[i];
        bool is_public = (base->__offset_flags & __cxxabiv1::__base_class_type_info::__public_mask) != 0;
        walk_bases(base->__base_type, base_ptr(ptr, vptr, base), wanted, public_path && is_public, out);
    }
}

static bool has_public_base_at(const std::type_info *cur, char *ptr, const std::type_info *wanted,
                               char *wanted_ptr, bool public_path) {
    if (same_type(cur, wanted) && ptr == wanted_ptr) return public_path;

    if (is_si_type(cur)) {
        const __cxxabiv1::__si_class_type_info *si = (const __cxxabiv1::__si_class_type_info *)cur;
        return has_public_base_at(si->__base_type, ptr, wanted, wanted_ptr, public_path);
    }

    if (!is_vmi_type(cur)) return false;

    const __cxxabiv1::__vmi_class_type_info *vmi = (const __cxxabiv1::__vmi_class_type_info *)cur;
    void **vptr = *(void ***)ptr;
    for (unsigned i = 0; i < vmi->__base_count; i++) {
        const __cxxabiv1::__base_class_type_info *base = &vmi->__base_info[i];
        bool is_public = (base->__offset_flags & __cxxabiv1::__base_class_type_info::__public_mask) != 0;
        if (has_public_base_at(base->__base_type, base_ptr(ptr, vptr, base), wanted, wanted_ptr,
                               public_path && is_public)) return true;
    }
    return false;
}

static void *cast_from_complete(void *complete, const std::type_info *dynamic_type,
                                const std::type_info *src, const std::type_info *dst,
                                const void *sub) {
    if (same_type(dynamic_type, dst)) return complete;

    if (src && sub && !has_public_base_at(dynamic_type, (char *)complete, src, (char *)sub, true)) return 0;

    base_match result = {0, 0, false};
    walk_bases(dynamic_type, (char *)complete, dst, true, &result);
    if (result.count != 1 || !result.public_path) return 0;
    return result.first;
}

extern "C" void *__dynamic_cast(const void *sub, const __cxxabiv1::__class_type_info *src,
                                const __cxxabiv1::__class_type_info *dst, sptr src2dst_offset) {
    if (!sub || !dst) return 0;
    if (src2dst_offset >= 0) {
        void *fast = (char *)sub + src2dst_offset;
        (void)fast;
    }
    void **vptr = *(void ***)sub;
    ptrdiff_t offset_to_top = ((ptrdiff_t *)vptr)[-2];
    void *complete = (char *)sub + offset_to_top;
    const std::type_info *dynamic_type = (const std::type_info *)vptr[-1];
    return cast_from_complete(complete, dynamic_type, src, dst, sub);
}

enum {
    DW_EH_PE_omit = 0xff, DW_EH_PE_absptr = 0x00, DW_EH_PE_uleb128 = 0x01,
    DW_EH_PE_udata2 = 0x02, DW_EH_PE_udata4 = 0x03, DW_EH_PE_udata8 = 0x04,
    DW_EH_PE_sleb128 = 0x09, DW_EH_PE_sdata2 = 0x0a, DW_EH_PE_sdata4 = 0x0b,
    DW_EH_PE_sdata8 = 0x0c, DW_EH_PE_pcrel = 0x10, DW_EH_PE_textrel = 0x20,
    DW_EH_PE_datarel = 0x30, DW_EH_PE_funcrel = 0x40, DW_EH_PE_aligned = 0x50,
    DW_EH_PE_indirect = 0x80
};

static uint64_t read_uleb(const uint8_t **p) {
    uint64_t result = 0;
    unsigned shift = 0;
    for (;;) {
        uint8_t b = *(*p)++;
        result |= (uint64_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) return result;
        shift += 7;
    }
}

static int64_t read_sleb(const uint8_t **p) {
    int64_t result = 0;
    unsigned shift = 0;
    uint8_t b;
    do {
        b = *(*p)++;
        result |= (int64_t)(b & 0x7f) << shift;
        shift += 7;
    } while (b & 0x80);
    if ((b & 0x40) && shift < 64) result |= -((int64_t)1 << shift);
    return result;
}

static uintptr_t read_native(const uint8_t **p, size_t size, bool sign) {
    uintptr_t result = 0;
    for (size_t i = 0; i < size; i++) result |= (uintptr_t)(*(*p)++) << (i * 8);
    if (sign && size < sizeof(uintptr_t)) {
        uintptr_t bit = (uintptr_t)1 << (size * 8 - 1);
        if (result & bit) result |= ~(bit - 1);
    }
    return result;
}

static size_t encoded_size(uint8_t encoding) {
    switch (encoding & 0x0f) {
    case DW_EH_PE_udata2: case DW_EH_PE_sdata2: return 2;
    case DW_EH_PE_udata4: case DW_EH_PE_sdata4: return 4;
    case DW_EH_PE_udata8: case DW_EH_PE_sdata8: return 8;
    case DW_EH_PE_absptr: return sizeof(uintptr_t);
    default: return 0;
    }
}

static uintptr_t read_encoded(const uint8_t **p, uint8_t encoding, _Unwind_Context *ctx) {
    if (encoding == DW_EH_PE_omit) return 0;
    if ((encoding & 0x70) == DW_EH_PE_aligned) *p = (const uint8_t *)(((uintptr_t)*p + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1));
    uintptr_t start = (uintptr_t)*p;
    uintptr_t result;
    switch (encoding & 0x0f) {
    case DW_EH_PE_absptr: result = read_native(p, sizeof(uintptr_t), false); break;
    case DW_EH_PE_uleb128: result = read_uleb(p); break;
    case DW_EH_PE_udata2: result = read_native(p, 2, false); break;
    case DW_EH_PE_udata4: result = read_native(p, 4, false); break;
    case DW_EH_PE_udata8: result = read_native(p, 8, false); break;
    case DW_EH_PE_sleb128: result = (uintptr_t)read_sleb(p); break;
    case DW_EH_PE_sdata2: result = read_native(p, 2, true); break;
    case DW_EH_PE_sdata4: result = read_native(p, 4, true); break;
    case DW_EH_PE_sdata8: result = read_native(p, 8, true); break;
    default: abort();
    }
    switch (encoding & 0x70) {
    case 0: break;
    case DW_EH_PE_pcrel: result += start; break;
    case DW_EH_PE_textrel: result += _Unwind_GetTextRelBase(ctx); break;
    case DW_EH_PE_datarel: result += _Unwind_GetDataRelBase(ctx); break;
    case DW_EH_PE_funcrel: result += _Unwind_GetRegionStart(ctx); break;
    default: abort();
    }
    if (encoding & DW_EH_PE_indirect) result = *(uintptr_t *)result;
    return result;
}

struct caught_exception;

struct cxa_exception {
    std::type_info *exception_type;
    void (*destructor)(void *);
    void *unexpected_handler;
    void *terminate_handler;
    cxa_exception *next_exception;
    int handler_count;
    int handler_switch_value;
    const uint8_t *action_record;
    const uint8_t *language_specific_data;
    void *catch_temp;
    void *adjusted_ptr;
    caught_exception *catch_node;
    _Unwind_Exception unwind;
};

struct caught_exception {
    cxa_exception *native;
    _Unwind_Exception *foreign;
    caught_exception *next;
    bool heap;
};

struct abi_thread_state { caught_exception *caught; };
static abi_thread_state fallback_state;

static void default_terminate() {
    print_abort("libcppabi: terminate\n");
}

static void default_unexpected() {
    print_abort("libcppabi: unexpected\n");
}

static void (*terminate_handler)() = default_terminate;
static void (*unexpected_handler)() = default_unexpected;

static abi_thread_state *thread_state() {
    void *p = __libc_get_thread_data();
    if (p) return (abi_thread_state *)p;
    abi_thread_state *state = (abi_thread_state *)malloc(sizeof(*state));
    if (!state) return &fallback_state;
    memset(state, 0, sizeof(*state));
    __libc_set_thread_data(state);
    return __libc_get_thread_data() ? state : &fallback_state;
}

static bool is_native(_Unwind_Exception *unwind) {
    return unwind && unwind->exception_class == exception_class;
}

static cxa_exception *exception_from_unwind(_Unwind_Exception *unwind) {
    return (cxa_exception *)((char *)unwind - offsetof(cxa_exception, unwind));
}

static void exception_cleanup(_Unwind_Reason_Code, _Unwind_Exception *unwind) {
    cxa_exception *exc = exception_from_unwind(unwind);
    if (exc->destructor) exc->destructor(exc + 1);
    if (exc->catch_node) free(exc->catch_node);
    free(exc);
}

extern "C" void *__cxa_allocate_exception(size_t thrown_size) {
    cxa_exception *exc = (cxa_exception *)malloc(sizeof(cxa_exception) + thrown_size);
    if (!exc) terminate_handler();
    memset(exc, 0, sizeof(*exc));
    return exc + 1;
}

extern "C" void __cxa_free_exception(void *thrown) {
    if (!thrown) return;
    free((cxa_exception *)thrown - 1);
}

extern "C" void __cxa_throw(void *thrown, std::type_info *type, void (*destructor)(void *)) {
    cxa_exception *exc = (cxa_exception *)thrown - 1;
    exc->exception_type = type;
    exc->destructor = destructor;
    exc->unwind.exception_class = exception_class;
    exc->unwind.exception_cleanup = exception_cleanup;
    _Unwind_RaiseException(&exc->unwind);
    terminate_handler();
}

extern "C" void *__cxa_begin_catch(void *unwind_arg) {
    _Unwind_Exception *unwind = (_Unwind_Exception *)unwind_arg;
    abi_thread_state *st = thread_state();
    if (is_native(unwind)) {
        cxa_exception *exc = exception_from_unwind(unwind);
        if (!exc->catch_node) {
            exc->catch_node = (caught_exception *)malloc(sizeof(*exc->catch_node));
            if (!exc->catch_node) terminate_handler();
            memset(exc->catch_node, 0, sizeof(*exc->catch_node));
        }
        caught_exception *node = exc->catch_node;
        if (node->native != exc) {
            node->native = exc;
            node->foreign = 0;
            node->heap = false;
            node->next = st->caught;
            st->caught = node;
        }
        exc->handler_count++;
        return exc->adjusted_ptr ? exc->adjusted_ptr : exc + 1;
    }
    caught_exception *node = (caught_exception *)malloc(sizeof(*node));
    if (!node) terminate_handler();
    memset(node, 0, sizeof(*node));
    node->foreign = unwind;
    node->heap = true;
    node->next = st->caught;
    st->caught = node;
    return unwind;
}

extern "C" void __cxa_end_catch() {
    abi_thread_state *st = thread_state();
    caught_exception *node = st->caught;
    if (!node) terminate_handler();
    st->caught = node->next;
    if (node->native) {
        cxa_exception *exc = node->native;
        node->next = 0;
        if (--exc->handler_count <= 0) {
            node->native = 0;
            _Unwind_DeleteException(&exc->unwind);
        }
        return;
    }
    if (node->foreign) _Unwind_DeleteException(node->foreign);
    if (node->heap) free(node);
}

extern "C" void __cxa_rethrow() {
    caught_exception *node = thread_state()->caught;
    if (!node) terminate_handler();
    if (node->native) _Unwind_Resume_or_Rethrow(&node->native->unwind);
    if (node->foreign) _Unwind_Resume_or_Rethrow(node->foreign);
    terminate_handler();
}

extern "C" void *__cxa_get_exception_ptr(void *unwind_arg) {
    _Unwind_Exception *unwind = (_Unwind_Exception *)unwind_arg;
    if (!is_native(unwind)) return unwind;
    cxa_exception *exc = exception_from_unwind(unwind);
    return exc->adjusted_ptr ? exc->adjusted_ptr : exc + 1;
}

extern "C" void *__cxa_current_primary_exception() {
    caught_exception *node = thread_state()->caught;
    return node && node->native ? node->native + 1 : 0;
}

extern "C" void __cxa_increment_exception_refcount(void *) {}
extern "C" void __cxa_decrement_exception_refcount(void *) {}
extern "C" void *__cxa_begin_cleanup(void *p) { return p; }
extern "C" void __cxa_end_cleanup() { terminate_handler(); }
extern "C" void __cxa_call_unexpected(void *) { unexpected_handler(); terminate_handler(); }
extern "C" void _ZSt9terminatev() { terminate_handler(); }
extern "C" void _ZSt10unexpectedv() { unexpected_handler(); terminate_handler(); }

static bool type_matches(const std::type_info *catch_type, const cxa_exception *exc, void **adjusted) {
    if (!catch_type) { *adjusted = (void *)(exc + 1); return true; }
    if (!exc || !exc->exception_type) return false;
    if (*catch_type == *exc->exception_type) { *adjusted = (void *)(exc + 1); return true; }
    void *p = cast_from_complete((void *)(exc + 1), exc->exception_type, 0, catch_type, 0);
    if (!p) return false;
    *adjusted = p;
    return true;
}

struct lsda_result { uintptr_t landing_pad; int selector; const uint8_t *action; };

static const std::type_info *get_ttype(const uint8_t *class_info, uint8_t encoding, int filter, _Unwind_Context *ctx) {
    if (filter <= 0 || encoding == DW_EH_PE_omit) return 0;
    size_t size = encoded_size(encoding);
    if (!size) abort();
    const uint8_t *p = class_info - filter * size;
    return (const std::type_info *)read_encoded(&p, encoding, ctx);
}

static bool exception_spec_allows(const uint8_t *class_info, uint8_t encoding, int64_t filter,
                                  const cxa_exception *exc, _Unwind_Context *ctx) {
    if (!exc || !class_info || encoding == DW_EH_PE_omit) return true;
    const uint8_t *p = class_info + filter;
    uint64_t count = read_uleb(&p);
    for (uint64_t i = 0; i < count; i++) {
        int index = (int)read_uleb(&p);
        const std::type_info *type = get_ttype(class_info, encoding, index, ctx);
        void *adjusted = 0;
        if (type_matches(type, exc, &adjusted)) return true;
    }
    return false;
}

static bool find_lsda(_Unwind_Context *ctx, const cxa_exception *exc, _Unwind_Exception *unwind,
                      _Unwind_Action actions, lsda_result *out) {
    const uint8_t *lsda = (const uint8_t *)_Unwind_GetLanguageSpecificData(ctx);
    if (!lsda) return false;

    const uint8_t *p = lsda;
    uintptr_t region = _Unwind_GetRegionStart(ctx);
    uintptr_t ip = _Unwind_GetIP(ctx) - 1;

    uint8_t lpstart_encoding = *p++;
    uintptr_t lpstart = region;
    if (lpstart_encoding != DW_EH_PE_omit) lpstart = read_encoded(&p, lpstart_encoding, ctx);

    uint8_t ttype_encoding = *p++;
    const uint8_t *class_info = 0;
    if (ttype_encoding != DW_EH_PE_omit) {
        uint64_t offset = read_uleb(&p);
        class_info = p + offset;
    }

    uint8_t callsite_encoding = *p++;
    uint64_t callsite_len = read_uleb(&p);
    const uint8_t *callsite_end = p + callsite_len;
    const uint8_t *action_table = callsite_end;

    while (p < callsite_end) {
        uintptr_t start = read_encoded(&p, callsite_encoding, ctx);
        uintptr_t len = read_encoded(&p, callsite_encoding, ctx);
        uintptr_t landing = read_encoded(&p, callsite_encoding, ctx);
        uint64_t action_offset = read_uleb(&p);

        if (ip < region + start || ip >= region + start + len) continue;
        if (!landing) return false;

        out->landing_pad = lpstart + landing;
        out->selector = 0;
        out->action = 0;

        if (actions & _UA_FORCE_UNWIND) return (actions & _UA_CLEANUP_PHASE) != 0 && action_offset == 0;
        if (!is_native(unwind)) return (actions & _UA_CLEANUP_PHASE) != 0 && action_offset == 0;
        if (action_offset == 0) return (actions & _UA_CLEANUP_PHASE) != 0;

        const uint8_t *action = action_table + action_offset - 1;
        for (;;) {
            const uint8_t *this_action = action;
            int64_t filter = read_sleb(&action);
            int64_t next = read_sleb(&action);

            if (filter > 0) {
                const std::type_info *catch_type = get_ttype(class_info, ttype_encoding, (int)filter, ctx);
                void *adjusted = 0;
                if (type_matches(catch_type, exc, &adjusted)) {
                    ((cxa_exception *)exc)->adjusted_ptr = adjusted;
                    out->selector = (int)filter;
                    out->action = this_action;
                    return true;
                }
            } else if (filter == 0) {
                ((cxa_exception *)exc)->adjusted_ptr = (void *)(exc + 1);
                out->selector = 0;
                out->action = this_action;
                return true;
            } else if (!exception_spec_allows(class_info, ttype_encoding, filter, exc, ctx)) {
                ((cxa_exception *)exc)->adjusted_ptr = (void *)(exc + 1);
                out->selector = (int)filter;
                out->action = this_action;
                return true;
            }

            if (next == 0) break;
            action = this_action + next;
        }
        return false;
    }
    return false;
}

extern "C" _Unwind_Reason_Code __gxx_personality_v0(int version, _Unwind_Action actions,
                                                      uint64_t cls, _Unwind_Exception *unwind,
                                                      _Unwind_Context *ctx) {
    if (version != 1) return _URC_FATAL_PHASE1_ERROR;
    bool native = cls == exception_class;
    cxa_exception *exc = native ? exception_from_unwind(unwind) : 0;
    lsda_result result;
    if (!find_lsda(ctx, exc, unwind, actions, &result)) return _URC_CONTINUE_UNWIND;
    if (actions & _UA_SEARCH_PHASE) return result.selector != 0 ? _URC_HANDLER_FOUND : _URC_CONTINUE_UNWIND;
    if (native) {
        exc->handler_switch_value = result.selector;
        exc->action_record = result.action;
        exc->language_specific_data = (const uint8_t *)_Unwind_GetLanguageSpecificData(ctx);
        if (!exc->adjusted_ptr) exc->adjusted_ptr = exc + 1;
    }
    _Unwind_SetGR(ctx, 0, (_Unwind_Word)unwind);
    _Unwind_SetGR(ctx, 1, (_Unwind_Word)result.selector);
    _Unwind_SetIP(ctx, result.landing_pad);
    return _URC_INSTALL_CONTEXT;
}
