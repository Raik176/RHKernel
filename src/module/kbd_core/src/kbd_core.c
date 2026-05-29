#include "kbd_core.h"

#include "input.h"
#include "mod/device.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "mod/scheduler.h"
#include "smp/lock.h"
#include "string.h"
#include "symbol.h"

#define KBD_EVENT_BUFFER_SIZE 512u
#define KBD_SC_EXTENDED INPUT_KEY_EXTENDED
#define KBD_KEY_RELEASE 0x80u
#define KBD_REPEAT_DELAY_TICKS 500u
#define KBD_REPEAT_PERIOD_TICKS 33u
#define KBD_KEY_STATE_BITS 256u

struct kbd_event_ring {
    struct input_event data[KBD_EVENT_BUFFER_SIZE];
    uint64_t first;
    uint64_t next;
};

struct kbd_device {
    char name[kbd_MAX_NAME];
    int index;
    uint32_t modifiers;
    int e0;
    int online;
    uint32_t repeat_code;
    uint32_t repeat_text;
    uint64_t repeat_next_tick;
    int repeat_active;
    uint32_t down[(KBD_KEY_STATE_BITS + 31u) / 32u];
    spinlock_t lock;
    struct kernel_wait_queue wait;
    struct kbd_event_ring events;
};

struct kbd_reader {
    struct kbd_device *kbd;
    struct kbd_event_ring *ring;
    spinlock_t *lock;
    struct kernel_wait_queue *wait;
    uint64_t next;
    int aggregate;
};

static int kbd_count = 0;
static spinlock_t kbd_global_lock;
static struct kernel_wait_queue kbd_global_wait;
static struct kbd_event_ring kbd_all_events;
static struct kbd_device *kbd_devices[64];

static void kbd_generate_due_repeats(struct kbd_device *only);
static int kbd_repeat_active(struct kbd_device *only);

static int key_slot(uint32_t code) {
    uint32_t base = code & 0x7fu;
    if (base >= 128u) return -1;
    return (code & KBD_SC_EXTENDED) ? (int)(128u + base) : (int)base;
}

static int key_is_down(struct kbd_device *kbd, uint32_t code) {
    int slot = key_slot(code);
    if (slot < 0) return 0;
    return (kbd->down[(uint32_t)slot / 32u] & (1u << ((uint32_t)slot % 32u))) != 0;
}

static void key_set_down(struct kbd_device *kbd, uint32_t code, int down) {
    int slot = key_slot(code);
    if (slot < 0) return;
    uint32_t bit = 1u << ((uint32_t)slot % 32u);
    if (down) kbd->down[(uint32_t)slot / 32u] |= bit;
    else kbd->down[(uint32_t)slot / 32u] &= ~bit;
}

static const char keymap[128][2] = {
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
    [0x39] = {' ', ' '},  [0x1C] = {'\n', '\n'}, [0x0E] = {'\b', '\b'}, [0x0F] = {'\t', '\t'},
};

static void ring_push(struct kbd_event_ring *ring, const struct input_event *ev) {
    struct input_event out = *ev;
    out.sequence = ring->next;
    ring->data[ring->next % KBD_EVENT_BUFFER_SIZE] = out;
    ring->next++;
    if (ring->next - ring->first > KBD_EVENT_BUFFER_SIZE) ring->first = ring->next - KBD_EVENT_BUFFER_SIZE;
}

static int ring_pop_one(struct kbd_reader *reader, struct input_event *out) {
    if (reader->next < reader->ring->first) {
        uint64_t dropped = reader->ring->first - reader->next;
        memset(out, 0, sizeof(*out));
        out->sequence = reader->ring->first;
        out->type = INPUT_EVENT_SYNC;
        out->device = reader->aggregate ? INPUT_STREAM_AGGREGATE : (uint32_t)reader->kbd->index;
        out->code = INPUT_SYNC_DROPPED;
        out->value = dropped > UINT32_MAX ? UINT32_MAX : (uint32_t)dropped;
        reader->next = reader->ring->first;
        return 1;
    }
    if (reader->next >= reader->ring->next) return 0;
    *out = reader->ring->data[reader->next % KBD_EVENT_BUFFER_SIZE];
    reader->next++;
    return 1;
}

static uint64_t reader_read(struct kbd_reader *reader, uint64_t size, uint8_t *buf) {
    if (!reader || !buf || size < sizeof(struct input_event) || (size % sizeof(struct input_event)) != 0) return 0;

    for (;;) {
        kbd_generate_due_repeats(reader->aggregate ? NULL : reader->kbd);

        uint64_t flags;
        spinlock_acquire(reader->lock, &flags);
        uint64_t seq = kernel_wait_queue_seq(reader->wait);
        uint64_t n = 0;
        uint64_t max = size / sizeof(struct input_event);
        while (n < max && ring_pop_one(reader, (struct input_event *)(buf + n * sizeof(struct input_event)))) n++;
        int online = reader->aggregate || !reader->kbd || reader->kbd->online;
        spinlock_release(reader->lock, flags);
        if (n != 0) return n * sizeof(struct input_event);
        if (!online) return 0;
        if (kbd_repeat_active(reader->aggregate ? NULL : reader->kbd)) {
            if (kernel_sleep_ticks(1) != 0) return 0;
        } else if (kernel_wait_queue_wait_changed(reader->wait, seq) != 0) {
            return 0;
        }
    }
}

static int event_open(void *priv, void **ctx) {
    if (!ctx) return -1;
    struct kbd_reader *reader = (struct kbd_reader *)kmalloc(sizeof(*reader));
    if (!reader) return -1;
    memset(reader, 0, sizeof(*reader));

    struct kbd_device *kbd = (struct kbd_device *)priv;
    uint64_t flags;
    if (kbd) {
        spinlock_acquire(&kbd->lock, &flags);
        if (!kbd->online) {
            spinlock_release(&kbd->lock, flags);
            kfree(reader);
            return -1;
        }
        reader->kbd = kbd;
        reader->ring = &kbd->events;
        reader->lock = &kbd->lock;
        reader->wait = &kbd->wait;
        reader->next = kbd->events.next;
        reader->aggregate = 0;
        spinlock_release(&kbd->lock, flags);
    } else {
        spinlock_acquire(&kbd_global_lock, &flags);
        reader->ring = &kbd_all_events;
        reader->lock = &kbd_global_lock;
        reader->wait = &kbd_global_wait;
        reader->next = kbd_all_events.next;
        reader->aggregate = 1;
        spinlock_release(&kbd_global_lock, flags);
    }

    *ctx = reader;
    return 0;
}

static void event_close(void *priv, void *ctx) {
    (void)priv;
    if (ctx) kfree(ctx);
}

static uint64_t kbd_event_read(void *priv, void *ctx, uint64_t off, uint64_t size, uint8_t *buf) {
    (void)priv;
    (void)off;
    return reader_read((struct kbd_reader *)ctx, size, buf);
}

static struct device_ops kbd_event_ops = {
    .read = NULL,
    .write = NULL,
    .mmap = NULL,
    .open = event_open,
    .close = event_close,
    .read_file = kbd_event_read,
    .write_file = NULL,
};

static char translated_char(uint32_t code, uint32_t modifiers) {
    code &= ~KBD_SC_EXTENDED;
    if (code >= 128) return 0;
    char normal = keymap[code][0];
    char shifted = keymap[code][1];
    if (!normal) return 0;
    int shift = (modifiers & INPUT_MOD_SHIFT) != 0;
    char c = shift ? shifted : normal;
    if ((modifiers & INPUT_MOD_CAPS) && normal >= 'a' && normal <= 'z') c = shift ? normal : shifted;
    return c;
}

static void update_modifier(struct kbd_device *kbd, uint32_t code, int pressed) {
    uint32_t bit = 0;
    if (code == 0x2A || code == 0x36) bit = INPUT_MOD_SHIFT;
    if (code == 0x1D || code == (KBD_SC_EXTENDED | 0x1D)) bit = INPUT_MOD_CTRL;
    if (code == 0x38 || code == (KBD_SC_EXTENDED | 0x38)) bit = INPUT_MOD_ALT;
    if (bit) {
        if (pressed) kbd->modifiers |= bit;
        else kbd->modifiers &= ~bit;
    }
    if (code == 0x3A && pressed) kbd->modifiers ^= INPUT_MOD_CAPS;
}

static void push_global_event(const struct input_event *ev) {
    uint64_t flags;
    spinlock_acquire(&kbd_global_lock, &flags);
    ring_push(&kbd_all_events, ev);
    spinlock_release(&kbd_global_lock, flags);
    kernel_wait_queue_wake_all(&kbd_global_wait);
}

static void push_device_event(struct kbd_device *kbd, const struct input_event *ev) {
    uint64_t flags;
    spinlock_acquire(&kbd->lock, &flags);
    ring_push(&kbd->events, ev);
    spinlock_release(&kbd->lock, flags);
    kernel_wait_queue_wake_all(&kbd->wait);
}

static void kbd_push_key_event(struct kbd_device *kbd, uint32_t code, uint32_t value, uint32_t text) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_EVENT_KEY;
    ev.device = (uint32_t)kbd->index;
    ev.code = code;
    ev.value = value;
    ev.modifiers = kbd->modifiers;
    ev.text = text;
    ring_push(&kbd->events, &ev);
    push_global_event(&ev);
}

void kbd_handle_key(struct kbd_device *kbd, uint32_t code, int pressed, uint32_t text) {
    if (!kbd || (code & ~INPUT_KEY_MAX_CODE)) return;
    pressed = pressed ? 1 : 0;

    uint64_t flags;
    spinlock_acquire(&kbd->lock, &flags);
    if (!kbd->online) {
        spinlock_release(&kbd->lock, flags);
        return;
    }

    int already_down = key_is_down(kbd, code);
    if (pressed && already_down) {
        spinlock_release(&kbd->lock, flags);
        return;
    }
    if (!pressed && !already_down) {
        spinlock_release(&kbd->lock, flags);
        return;
    }

    uint32_t value = pressed ? INPUT_KEY_PRESSED : INPUT_KEY_RELEASED;
    uint32_t out_text = 0;
    if (pressed) out_text = text ? text : (uint32_t)(uint8_t)translated_char(code, kbd->modifiers);

    update_modifier(kbd, code, pressed);
    key_set_down(kbd, code, pressed);

    if (pressed && out_text) {
        kbd->repeat_code = code;
        kbd->repeat_text = out_text;
        kbd->repeat_next_tick = kernel_monotonic_ticks() + KBD_REPEAT_DELAY_TICKS;
        kbd->repeat_active = 1;
    } else if (!pressed && kbd->repeat_active && kbd->repeat_code == code) {
        kbd->repeat_active = 0;
        kbd->repeat_code = 0;
        kbd->repeat_text = 0;
        kbd->repeat_next_tick = 0;
    }

    kbd_push_key_event(kbd, code, value, pressed ? out_text : 0);
    spinlock_release(&kbd->lock, flags);
    kernel_wait_queue_wake_all(&kbd->wait);
}

void kbd_handle_scancode(struct kbd_device *kbd, uint8_t scancode, int ignored_pressed) {
    (void)ignored_pressed;
    if (!kbd) return;

    uint64_t flags;
    spinlock_acquire(&kbd->lock, &flags);
    if (!kbd->online) {
        spinlock_release(&kbd->lock, flags);
        return;
    }
    if (scancode == 0xE0) {
        kbd->e0 = 1;
        spinlock_release(&kbd->lock, flags);
        return;
    }
    if (scancode == 0xE1) {
        kbd->e0 = 0;
        spinlock_release(&kbd->lock, flags);
        return;
    }
    int e0 = kbd->e0;
    kbd->e0 = 0;
    spinlock_release(&kbd->lock, flags);

    int pressed = (scancode & KBD_KEY_RELEASE) == 0;
    uint32_t code = scancode & 0x7F;
    if (e0) code |= KBD_SC_EXTENDED;
    kbd_handle_key(kbd, code, pressed, 0);
}

static void kbd_generate_one_repeat(struct kbd_device *kbd, uint64_t now) {
    if (!kbd) return;

    uint64_t flags;
    spinlock_acquire(&kbd->lock, &flags);
    if (!kbd->online || !kbd->repeat_active || kbd->repeat_next_tick > now) {
        spinlock_release(&kbd->lock, flags);
        return;
    }

    uint32_t code = kbd->repeat_code;
    uint32_t text = kbd->repeat_text;
    if (key_is_down(kbd, code)) {
        kbd_push_key_event(kbd, code, INPUT_KEY_REPEATED, text);
        kbd->repeat_next_tick = now + KBD_REPEAT_PERIOD_TICKS;
    } else {
        kbd->repeat_active = 0;
        kbd->repeat_code = 0;
        kbd->repeat_text = 0;
        kbd->repeat_next_tick = 0;
    }
    spinlock_release(&kbd->lock, flags);
    kernel_wait_queue_wake_all(&kbd->wait);
}

static void kbd_generate_due_repeats(struct kbd_device *only) {
    uint64_t now = kernel_monotonic_ticks();
    if (only) {
        kbd_generate_one_repeat(only, now);
        return;
    }

    struct kbd_device *local[64];
    uint32_t count = 0;
    uint64_t flags;
    spinlock_acquire(&kbd_global_lock, &flags);
    for (uint32_t i = 0; i < sizeof(kbd_devices) / sizeof(kbd_devices[0]); i++) {
        if (kbd_devices[i]) local[count++] = kbd_devices[i];
    }
    spinlock_release(&kbd_global_lock, flags);

    for (uint32_t i = 0; i < count; i++) kbd_generate_one_repeat(local[i], now);
}

static int kbd_one_repeat_active(struct kbd_device *kbd) {
    if (!kbd) return 0;
    uint64_t flags;
    spinlock_acquire(&kbd->lock, &flags);
    int active = kbd->online && kbd->repeat_active;
    spinlock_release(&kbd->lock, flags);
    return active;
}

static int kbd_repeat_active(struct kbd_device *only) {
    if (only) return kbd_one_repeat_active(only);

    struct kbd_device *local[64];
    uint32_t count = 0;
    uint64_t flags;
    spinlock_acquire(&kbd_global_lock, &flags);
    for (uint32_t i = 0; i < sizeof(kbd_devices) / sizeof(kbd_devices[0]); i++) {
        if (kbd_devices[i]) local[count++] = kbd_devices[i];
    }
    spinlock_release(&kbd_global_lock, flags);

    for (uint32_t i = 0; i < count; i++) {
        if (kbd_one_repeat_active(local[i])) return 1;
    }
    return 0;
}

uint32_t kbd_get_modifiers(struct kbd_device *kbd) {
    if (!kbd) return 0;
    uint64_t flags;
    spinlock_acquire(&kbd->lock, &flags);
    uint32_t modifiers = kbd->modifiers;
    spinlock_release(&kbd->lock, flags);
    return modifiers;
}

static void emit_device_event(struct kbd_device *kbd, uint32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = INPUT_EVENT_DEVICE;
    ev.device = (uint32_t)kbd->index;
    ev.code = INPUT_DEVICE_KEYBOARD;
    ev.value = value;
    ev.modifiers = kbd->modifiers;
    push_device_event(kbd, &ev);
    push_global_event(&ev);
}

struct kbd_device *kbd_register(const char *name) {
    struct kbd_device *kbd = (struct kbd_device *)kmalloc(sizeof(*kbd));
    if (!kbd) return NULL;
    memset(kbd, 0, sizeof(*kbd));
    spinlock_init(&kbd->lock);
    kernel_wait_queue_init(&kbd->wait);
    kbd->online = 1;

    kbd->index = __atomic_fetch_add(&kbd_count, 1, __ATOMIC_RELAXED);
    if ((unsigned)kbd->index >= sizeof(kbd_devices) / sizeof(kbd_devices[0])) goto fail;
    strncpy(kbd->name, name ? name : "keyboard", kbd_MAX_NAME - 1);

    char path[64];
    snprintf(path, sizeof(path), "input/kbd%d/events", kbd->index);
    if (devfs_register(path, &kbd_event_ops, kbd) != 0) goto fail;

    uint64_t flags;
    spinlock_acquire(&kbd_global_lock, &flags);
    kbd_devices[kbd->index] = kbd;
    spinlock_release(&kbd_global_lock, flags);

    emit_device_event(kbd, INPUT_DEVICE_ADDED);
    return kbd;

fail:
    kfree(kbd);
    return NULL;
}

void kbd_unregister(struct kbd_device *kbd) {
    if (!kbd) return;

    uint64_t flags;
    spinlock_acquire(&kbd->lock, &flags);
    if (!kbd->online) {
        spinlock_release(&kbd->lock, flags);
        return;
    }
    kbd->online = 0;
    spinlock_release(&kbd->lock, flags);

    emit_device_event(kbd, INPUT_DEVICE_REMOVED);

    char path[64];
    snprintf(path, sizeof(path), "input/kbd%d/events", kbd->index);
    devfs_unregister(path);

    spinlock_acquire(&kbd_global_lock, &flags);
    if ((unsigned)kbd->index < sizeof(kbd_devices) / sizeof(kbd_devices[0])) kbd_devices[kbd->index] = NULL;
    spinlock_release(&kbd_global_lock, flags);

    kernel_wait_queue_wake_all(&kbd->wait);
}

KEXPORT(kbd_register)
KEXPORT(kbd_unregister)
KEXPORT(kbd_handle_scancode)
KEXPORT(kbd_handle_key)
KEXPORT(kbd_get_modifiers)

static int kbd_init() {
    spinlock_init(&kbd_global_lock);
    kernel_wait_queue_init(&kbd_global_wait);
    if (devfs_register("input/events", &kbd_event_ops, NULL) != 0) return -1;
    return 0;
}
MODULE_INFO("kbd_core", kbd_init, 0, NULL);
