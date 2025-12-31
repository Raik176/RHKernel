#include "kbd_core.h"

#include "mod/device.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "string.h"
#include "symbol.h"

#define KDB_BUFFER_SIZE 256

struct kdb_device {
    char name[KDB_MAX_NAME];
    int index;
    uint32_t modifiers;

    char buffer[KDB_BUFFER_SIZE];
    size_t head;
    size_t tail;
};

static int kdb_count = 0;

static const char keymap[128][2] = {
    // [Scancode] = { Normal, Shifted }
    [0x02] = {'1', '!'},  [0x03] = {'2', '@'},   [0x04] = {'3', '#'},  [0x05] = {'4', '$'},
    [0x06] = {'5', '%'},  [0x07] = {'6', '^'},   [0x08] = {'7', '&'},  [0x09] = {'8', '*'},
    [0x0A] = {'9', '('},  [0x0B] = {'0', ')'},   [0x0C] = {'-', '_'},  [0x0D] = {'=', '+'},

    [0x10] = {'q', 'Q'},  [0x11] = {'w', 'W'},   [0x12] = {'e', 'E'},  [0x13] = {'r', 'R'},
    [0x14] = {'t', 'T'},  [0x15] = {'y', 'Y'},   [0x16] = {'u', 'U'},  [0x17] = {'i', 'I'},
    [0x18] = {'o', 'O'},  [0x19] = {'p', 'P'},   [0x1A] = {'[', '{'},  [0x1B] = {']', '}'},

    [0x1E] = {'a', 'A'},  [0x1F] = {'s', 'S'},   [0x20] = {'d', 'D'},  [0x21] = {'f', 'F'},
    [0x22] = {'g', 'G'},  [0x23] = {'h', 'H'},   [0x24] = {'j', 'J'},  [0x25] = {'k', 'K'},
    [0x26] = {'l', 'L'},  [0x27] = {';', ':'},   [0x28] = {'\'', '"'}, [0x29] = {'`', '~'},

    [0x2B] = {'\\', '|'}, [0x2C] = {'z', 'Z'},   [0x2D] = {'x', 'X'},  [0x2E] = {'c', 'C'},
    [0x2F] = {'v', 'V'},  [0x30] = {'b', 'B'},   [0x31] = {'n', 'N'},  [0x32] = {'m', 'M'},
    [0x33] = {',', '<'},  [0x34] = {'.', '>'},   [0x35] = {'/', '?'},

    [0x39] = {' ', ' '},  [0x1C] = {'\n', '\n'}, [0x0E] = {'\b', '\b'}};

static char apply_modifiers(struct kdb_device *kdb, char c) {
    if (!c) return 0;
    if ((kdb->modifiers & KDB_SHIFT) || (kdb->modifiers & KDB_CAPS)) {
        if (c >= 'a' && c <= 'z') c -= 32;
    }
    return c;
}

// Updated signature: receives 'priv'
static uint32_t kdb_read(void *priv, uint32_t off, uint32_t size, uint8_t *buf) {
    struct kdb_device *kdb = (struct kdb_device *)priv;
    if (!kdb) return 0;

    size_t read_bytes = 0;
    while (kdb->tail != kdb->head && read_bytes < size) {
        buf[read_bytes++] = kdb->buffer[kdb->tail];
        kdb->tail = (kdb->tail + 1) % KDB_BUFFER_SIZE;
    }
    return (uint32_t)read_bytes;
}

static struct device_ops kdb_ops = {.read = kdb_read, .write = NULL};

struct kdb_device *kdb_register(const char *name) {
    struct kdb_device *kdb = (struct kdb_device *)kmalloc(sizeof(*kdb));
    memset(kdb, 0, sizeof(*kdb));

    kdb->index = kdb_count++;
    strncpy(kdb->name, name, KDB_MAX_NAME - 1);

    char path[64];
    snprintf(path, sizeof(path), "input/kdb%d", kdb->index);

    // Pass 'kdb' as the third argument (priv)
    if (device_register(path, &kdb_ops, kdb) != 0) {
        kfree(kdb);
        return NULL;
    }

    klog(LOG_INFO, "Keyboard registered: /dev/%s\n", path);
    return kdb;
}

void kdb_unregister(struct kdb_device *kdb) {
    char path[64];
    snprintf(path, sizeof(path), "input/kdb%d", kdb->index);
    device_unregister(path);
    kfree(kdb);
}

void kdb_handle_scancode(struct kdb_device *kdb, uint8_t sc, int pressed) {
    if (!kdb || sc >= 128) return;

    // 1. Update Modifiers (Shift, Caps)
    if (sc == 0x2A || sc == 0x36) {  // Left or Right Shift
        if (pressed)
            kdb->modifiers |= KDB_SHIFT;
        else
            kdb->modifiers &= ~KDB_SHIFT;
        return;
    }
    if (sc == 0x3A && pressed) {  // Caps Lock (on press only)
        kdb->modifiers ^= KDB_CAPS;
        return;
    }

    if (!pressed) return;

    int use_shift = (kdb->modifiers & KDB_SHIFT) ? 1 : 0;

    char c_normal = keymap[sc][0];
    char c_shifted = keymap[sc][1];

    char final_char = use_shift ? c_shifted : c_normal;

    if ((kdb->modifiers & KDB_CAPS) && (c_normal >= 'a' && c_normal <= 'z')) {
        final_char = use_shift ? c_normal : c_shifted;
    }

    if (!final_char) return;

    size_t next = (kdb->head + 1) % KDB_BUFFER_SIZE;
    if (next != kdb->tail) {
        kdb->buffer[kdb->head] = final_char;
        kdb->head = next;
    }
}

KEXPORT(kdb_register)
KEXPORT(kdb_unregister)
KEXPORT(kdb_handle_scancode)

static int kdb_init() { return 0; }
static void kdb_exit() {}
MODULE_INFO("kdb_core", kdb_init, kdb_exit);