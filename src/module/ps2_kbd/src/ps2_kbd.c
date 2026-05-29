#include "kbd_core.h"
#include "mod/heap.h"
#include "mod/interrupt.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "portio.h"
#include "string.h"

#define PS2_DATA 0x60
#define PS2_STATUS 0x64

struct ps2_keyboard {
    struct kbd_device *kbd;
    int released;
};

static struct ps2_keyboard *kbd;

static enum irq_return ps2_irq(void *priv) {
    struct ps2_keyboard *ps2 = (struct ps2_keyboard *)priv;
    uint8_t sc = inb(PS2_DATA);

    kbd_handle_scancode(ps2->kbd, sc, 0);

    return IRQ_HANDLED;
}

static int ps2_init() {
    kbd = kmalloc(sizeof(*kbd));
    memset(kbd, 0, sizeof(*kbd));

    kbd->kbd = kbd_register("ps2-keyboard");
    if (!kbd->kbd) {
        kfree(kbd);
        return -1;
    }

    if (request_irq(1, ps2_irq, IRQ_AFFINITY_ALL, kbd) != 0) {
        kbd_unregister(kbd->kbd);
        kfree(kbd);
        return -1;
    }

    return 0;
}

static void ps2_exit() {
    free_irq(1, ps2_irq);
    kbd_unregister(kbd->kbd);
    kfree(kbd);
}

MODULE_INFO("ps2_kbd", ps2_init, 0, ps2_exit, "kbd_core");