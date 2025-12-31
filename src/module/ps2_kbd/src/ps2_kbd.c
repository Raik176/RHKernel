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
    struct kdb_device *kdb;
    int released;
};

static struct ps2_keyboard *kbd;

static enum irq_return ps2_irq(void *priv) {
    struct ps2_keyboard *ps2 = (struct ps2_keyboard *)priv;
    uint8_t sc = inb(PS2_DATA);

    // Simple PS/2 Set 1: Bit 7 set means Key Released
    int pressed = !(sc & 0x80);
    uint8_t code = sc & 0x7F;

    kdb_handle_scancode(ps2->kdb, code, pressed);

    return IRQ_HANDLED;
}

static int ps2_init() {
    kbd = kmalloc(sizeof(*kbd));
    memset(kbd, 0, sizeof(*kbd));

    kbd->kdb = kdb_register("ps2-keyboard");
    if (!kbd->kdb) {
        kfree(kbd);
        return -1;
    }

    if (request_irq(1, ps2_irq, IRQ_AFFINITY_ALL, kbd) != 0) {
        kdb_unregister(kbd->kdb);
        kfree(kbd);
        return -1;
    }

    klog(LOG_INFO, "PS/2 keyboard driver loaded\n");
    return 0;
}

static void ps2_exit() {
    free_irq(1, ps2_irq);
    kdb_unregister(kbd->kdb);
    kfree(kbd);
}

MODULE_INFO("ps2_kdb", ps2_init, ps2_exit, "kdb_core");