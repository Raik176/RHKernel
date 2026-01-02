#include "kbd_core.h"

#include "mod/device.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "string.h"
#include "symbol.h"

#define kbd_BUFFER_SIZE 256

struct kbd_device {
    char name[kbd_MAX_NAME];
    int index;
    uint32_t modifiers;

    char buffer[kbd_BUFFER_SIZE];
    size_t head;
    size_t tail;
};

static int kbd_count = 0;

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

static uint32_t kbd_read(void *priv, uint32_t off, uint32_t size, uint8_t *buf) {
    (void)off;

    struct kbd_device *kbd = (struct kbd_device *)priv;
    if (!kbd) return 0;

    size_t read_bytes = 0;
    while (kbd->tail != kbd->head && read_bytes < size) {
        buf[read_bytes++] = kbd->buffer[kbd->tail];
        kbd->tail = (kbd->tail + 1) % kbd_BUFFER_SIZE;
    }
    return (uint32_t)read_bytes;
}

static struct device_ops kbd_ops = {.read = kbd_read, .write = NULL};

struct kbd_device *kbd_register(const char *name) {
    struct kbd_device *kbd = (struct kbd_device *)kmalloc(sizeof(*kbd));
    memset(kbd, 0, sizeof(*kbd));

    kbd->index = kbd_count++;
    strncpy(kbd->name, name, kbd_MAX_NAME - 1);

    char path[64];
    snprintf(path, sizeof(path), "input/kbd%d", kbd->index);

    if (device_register(path, &kbd_ops, kbd) != 0) {
        kfree(kbd);
        return NULL;
    }

    return kbd;
}

void kbd_unregister(struct kbd_device *kbd) {
    char path[64];
    snprintf(path, sizeof(path), "input/kbd%d", kbd->index);
    device_unregister(path);
    kfree(kbd);
}

void kbd_handle_scancode(struct kbd_device *kbd, uint8_t sc, int pressed) {
    if (!kbd || sc >= 128) return;

    if (sc == 0x2A || sc == 0x36) {  // Left or Right Shift
        if (pressed)
            kbd->modifiers |= kbd_SHIFT;
        else
            kbd->modifiers &= ~kbd_SHIFT;
        return;
    }
    if (sc == 0x3A && pressed) {  // Caps Lock (on press only)
        kbd->modifiers ^= kbd_CAPS;
        return;
    }

    if (!pressed) return;

    int use_shift = (kbd->modifiers & kbd_SHIFT) ? 1 : 0;

    char c_normal = keymap[sc][0];
    char c_shifted = keymap[sc][1];

    char final_char = use_shift ? c_shifted : c_normal;

    if ((kbd->modifiers & kbd_CAPS) && (c_normal >= 'a' && c_normal <= 'z')) {
        final_char = use_shift ? c_normal : c_shifted;
    }

    if (!final_char) return;

    size_t next = (kbd->head + 1) % kbd_BUFFER_SIZE;
    if (next != kbd->tail) {
        kbd->buffer[kbd->head] = final_char;
        kbd->head = next;
    }
}

KEXPORT(kbd_register)
KEXPORT(kbd_unregister)
KEXPORT(kbd_handle_scancode)

static int kbd_init() { return 0; }
static void kbd_exit() {}
MODULE_INFO("kbd_core", kbd_init, kbd_exit);