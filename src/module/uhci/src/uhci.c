#include "usb_core.h"

#include "mod/device.h"
#include "mod/dma.h"
#include "mod/heap.h"
#include "mod/interrupt.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "mod/scheduler.h"
#include "pci_bus.h"
#include "portio.h"
#include "smp/lock.h"
#include "string.h"

#define PCI_CLASS_SERIAL 0x0cu
#define PCI_SUBCLASS_USB 0x03u
#define PCI_PROGIF_UHCI  0x00u

#define UHCI_USBCMD   0x00u
#define UHCI_USBSTS   0x02u
#define UHCI_USBINTR  0x04u
#define UHCI_FRNUM    0x06u
#define UHCI_FLBASE   0x08u
#define UHCI_SOFMOD   0x0cu
#define UHCI_PORTSC1  0x10u

#define UHCI_CMD_RS      0x0001u
#define UHCI_CMD_HCRESET 0x0002u
#define UHCI_CMD_GRESET  0x0004u
#define UHCI_CMD_CF      0x0040u
#define UHCI_CMD_MAXP    0x0080u

#define UHCI_STS_USBINT  0x0001u
#define UHCI_STS_ERROR   0x0002u
#define UHCI_STS_RD      0x0004u
#define UHCI_STS_HSE     0x0008u
#define UHCI_STS_HCPE    0x0010u
#define UHCI_STS_HCH     0x0020u

#define UHCI_PORT_CCS    0x0001u
#define UHCI_PORT_CSC    0x0002u
#define UHCI_PORT_PE     0x0004u
#define UHCI_PORT_PEC    0x0008u
#define UHCI_PORT_LSDA   0x0100u
#define UHCI_PORT_RESET  0x0200u
#define UHCI_PORT_SUSP   0x1000u
#define UHCI_PORT_CHANGE (UHCI_PORT_CSC | UHCI_PORT_PEC)

#define UHCI_LINK_TERM 1u
#define UHCI_LINK_QH   2u

#define UHCI_TD_ACTLEN_MASK 0x000007ffu
#define UHCI_TD_BITSTUFF   (1u << 17)
#define UHCI_TD_TIMEOUT    (1u << 18)
#define UHCI_TD_NAK        (1u << 19)
#define UHCI_TD_BABBLE     (1u << 20)
#define UHCI_TD_BUFFER     (1u << 21)
#define UHCI_TD_STALLED    (1u << 22)
#define UHCI_TD_ACTIVE     (1u << 23)
#define UHCI_TD_IOC        (1u << 24)
#define UHCI_TD_ISO        (1u << 25)
#define UHCI_TD_LS         (1u << 26)
#define UHCI_TD_CERR3      (3u << 27)
#define UHCI_TD_SPD        (1u << 29)
#define UHCI_TD_FATAL      (UHCI_TD_BITSTUFF | UHCI_TD_TIMEOUT | UHCI_TD_BABBLE | UHCI_TD_BUFFER | UHCI_TD_STALLED)

#define USB_PID_OUT   0xe1u
#define USB_PID_IN    0x69u
#define USB_PID_SETUP 0x2du

#define UHCI_FRAME_COUNT 1024u
#define UHCI_MAX_TDS 256u
#define UHCI_MAX_DATA 16384u
#define UHCI_CONTROL_TIMEOUT_MS 500u
#define UHCI_BULK_TIMEOUT_MS 1000u
#define UHCI_INTR_POLL_MS 2u
#define UHCI_RESET_WAIT_MS 100u
#define UHCI_FRAME_SETTLE_MS 2u

struct uhci_qh {
    uint32_t link;
    uint32_t element;
} __attribute__((packed, aligned(16)));

struct uhci_td {
    uint32_t link;
    uint32_t status;
    uint32_t token;
    uint32_t buffer;
} __attribute__((packed, aligned(16)));

struct uhci_transfer {
    dma_allocation_t qh_dma;
    dma_allocation_t td_dma;
    dma_allocation_t setup_dma;
    dma_allocation_t data_dma;
    struct uhci_qh *qh;
    struct uhci_td *td;
    void *setup;
    void *data;
    uint32_t td_count;
};

struct uhci_controller {
    struct pci_device *pdev;
    uint16_t io;
    uint8_t irq;
    bool irq_registered;
    spinlock_t lock;
    dma_allocation_t frame_dma;
    dma_allocation_t skeleton_dma;
    uint32_t *frames;
    struct uhci_qh *skeleton;
    struct usb_hcd hcd;
    uint32_t index;
    volatile uint32_t transfer_busy;
    volatile uint32_t dead;
    uint8_t bulk_toggle[128][16][2];
    uint64_t transfer_count;
    uint64_t timeout_count;
    uint64_t error_count;
    uint64_t short_count;
    struct uhci_controller *next;
};

static struct uhci_controller *controllers;
static uint32_t next_index;

static inline uint16_t rd16(struct uhci_controller *c, uint16_t off) { return inw((uint16_t)(c->io + off)); }
static inline void wr16(struct uhci_controller *c, uint16_t off, uint16_t v) { outw((uint16_t)(c->io + off), v); }
static inline uint32_t rd32(struct uhci_controller *c, uint16_t off) { return inl((uint16_t)(c->io + off)); }
static inline void wr32(struct uhci_controller *c, uint16_t off, uint32_t v) { outl((uint16_t)(c->io + off), v); }
static inline uint8_t rd8(struct uhci_controller *c, uint16_t off) { return inb((uint16_t)(c->io + off)); }
static inline void wr8(struct uhci_controller *c, uint16_t off, uint8_t v) { outb((uint16_t)(c->io + off), v); }

static void uhci_sleep_ms(uint32_t ms) {
    if (ms == 0) return;
    if (kernel_can_sleep()) {
        (void)kernel_sleep_ticks(ms);
        return;
    }
    for (uint32_t i = 0; i < ms * 50000u; i++) io_wait();
}

static uint32_t uhci_timeout_deadline(uint32_t timeout_ms, uint32_t fallback_ms) {
    uint32_t use = timeout_ms ? timeout_ms : fallback_ms;
    if (use == 0) use = 1;
    return (uint32_t)(kernel_monotonic_ticks() + use);
}

static int uhci_deadline_passed(uint32_t deadline) {
    return (int32_t)((uint32_t)kernel_monotonic_ticks() - deadline) >= 0;
}

static uint32_t td_token(uint8_t pid, uint8_t addr, uint8_t ep, uint8_t toggle, uint16_t len) {
    uint32_t max = len == 0 ? 0x7ffu : (uint32_t)(len - 1u);
    return (uint32_t)pid | ((uint32_t)(addr & 0x7fu) << 8) | ((uint32_t)(ep & 0x0fu) << 15) |
           ((uint32_t)(toggle & 1u) << 19) | (max << 21);
}

static uint32_t td_status(uint8_t low_speed, uint32_t extra) {
    return UHCI_TD_ACTIVE | UHCI_TD_CERR3 | (low_speed ? UHCI_TD_LS : 0) | extra;
}

static uint32_t td_actual_len(const struct uhci_td *td) {
    uint32_t n = td->status & UHCI_TD_ACTLEN_MASK;
    return n == UHCI_TD_ACTLEN_MASK ? 0u : n + 1u;
}

static int td_done_ok(const struct uhci_td *td) {
    return (td->status & (UHCI_TD_ACTIVE | UHCI_TD_FATAL)) == 0;
}

static int td_error_result(const struct uhci_td *td) {
    uint32_t s = td->status;
    if (s & UHCI_TD_ACTIVE) return 1;
    if (s & UHCI_TD_STALLED) return -4;
    if (s & UHCI_TD_FATAL) return -3;
    return 0;
}

static int uhci_controller_live(struct uhci_controller *c) {
    if (!c || c->dead) return 0;
    uint16_t st = rd16(c, UHCI_USBSTS);
    if (st & (UHCI_STS_HSE | UHCI_STS_HCPE)) {
        c->dead = 1;
        c->error_count++;
        klog(LOG_ERR, "UHCI: fatal controller status io=%04x sts=%04x\n", c->io, st);
        return 0;
    }
    return 1;
}

static void uhci_enter_transfer(struct uhci_controller *c) {
    while (__sync_lock_test_and_set(&c->transfer_busy, 1u)) {
        if (kernel_can_sleep()) (void)kernel_sleep_ticks(1);
        else kernel_yield();
    }
    __sync_synchronize();
}

static void uhci_leave_transfer(struct uhci_controller *c) {
    __sync_synchronize();
    __sync_lock_release(&c->transfer_busy);
}

static void free_transfer(struct uhci_transfer *t) {
    if (!t) return;
    dma_free_coherent(&t->data_dma);
    dma_free_coherent(&t->setup_dma);
    dma_free_coherent(&t->td_dma);
    dma_free_coherent(&t->qh_dma);
    memset(t, 0, sizeof(*t));
}

static int dma32_ok(const dma_allocation_t *a) {
    if (!a->virt) return 1;
    return a->phys <= UINT32_C(0xffffffff) && a->phys + a->size - 1u <= UINT32_C(0xffffffff);
}

static int alloc_transfer(struct uhci_transfer *t, uint32_t td_count, uint32_t data_len, int need_setup) {
    memset(t, 0, sizeof(*t));
    if (td_count == 0 || td_count > UHCI_MAX_TDS || data_len > UHCI_MAX_DATA) return -1;
    if (dma_alloc_coherent(sizeof(struct uhci_qh), 16, DMA_MASK_32, &t->qh_dma) != 0) return -1;
    if (dma_alloc_coherent(sizeof(struct uhci_td) * td_count, 16, DMA_MASK_32, &t->td_dma) != 0) goto fail;
    if (need_setup && dma_alloc_coherent(sizeof(struct usb_setup_packet), 8, DMA_MASK_32, &t->setup_dma) != 0) goto fail;
    if (data_len && dma_alloc_coherent(data_len, 16, DMA_MASK_32, &t->data_dma) != 0) goto fail;
    if (!dma32_ok(&t->qh_dma) || !dma32_ok(&t->td_dma) || !dma32_ok(&t->setup_dma) || !dma32_ok(&t->data_dma)) goto fail;
    t->qh = (struct uhci_qh *)t->qh_dma.virt;
    t->td = (struct uhci_td *)t->td_dma.virt;
    t->setup = t->setup_dma.virt;
    t->data = t->data_dma.virt;
    t->td_count = td_count;
    memset(t->qh, 0, sizeof(*t->qh));
    memset(t->td, 0, sizeof(struct uhci_td) * td_count);
    t->qh->link = UHCI_LINK_TERM;
    t->qh->element = UHCI_LINK_TERM;
    return 0;
fail:
    free_transfer(t);
    return -1;
}

static void link_transfer_locked(struct uhci_controller *c, struct uhci_transfer *t) {
    t->qh->link = c->skeleton->link;
    t->qh->element = (uint32_t)t->td_dma.phys;
    c->skeleton->link = (uint32_t)t->qh_dma.phys | UHCI_LINK_QH;
}

static void unlink_transfer_locked(struct uhci_controller *c, struct uhci_transfer *t) {
    uint32_t self = (uint32_t)t->qh_dma.phys | UHCI_LINK_QH;
    if (c->skeleton->link == self) c->skeleton->link = t->qh->link;
    t->qh->element = UHCI_LINK_TERM;
    t->qh->link = UHCI_LINK_TERM;
}

static void cancel_transfer(struct uhci_transfer *t) {
    for (uint32_t i = 0; i < t->td_count; i++) t->td[i].status &= ~UHCI_TD_ACTIVE;
}

static int wait_transfer(struct uhci_controller *c, struct uhci_transfer *t, uint32_t timeout_ms, uint32_t *completed) {
    uint32_t deadline = uhci_timeout_deadline(timeout_ms, UHCI_CONTROL_TIMEOUT_MS);
    if (completed) *completed = 0;
    for (;;) {
        int any_active = 0;
        uint32_t done = 0;
        if (!uhci_controller_live(c)) return -5;
        for (uint32_t i = 0; i < t->td_count; i++) {
            int r = td_error_result(&t->td[i]);
            if (r == 1) {
                any_active = 1;
                continue;
            }
            if (r < 0) {
                c->error_count++;
                if (completed) *completed = done;
                return r;
            }
            done++;
        }
        if (!any_active) {
            if (completed) *completed = done;
            return 0;
        }
        if (uhci_deadline_passed(deadline)) {
            c->timeout_count++;
            if (completed) *completed = done;
            return -2;
        }
        if (kernel_can_sleep()) (void)kernel_sleep_ticks(1);
        else io_wait();
    }
}

static int run_transfer(struct uhci_controller *c, struct uhci_transfer *t, uint32_t timeout_ms, uint32_t *completed) {
    if (!uhci_controller_live(c)) return -5;
    uhci_enter_transfer(c);
    if (!uhci_controller_live(c)) {
        uhci_leave_transfer(c);
        return -5;
    }
    uint64_t flags;
    spinlock_acquire(&c->lock, &flags);
    link_transfer_locked(c, t);
    spinlock_release(&c->lock, flags);

    int r = wait_transfer(c, t, timeout_ms, completed);
    if (r == -2) cancel_transfer(t);

    spinlock_acquire(&c->lock, &flags);
    unlink_transfer_locked(c, t);
    spinlock_release(&c->lock, flags);

    uhci_sleep_ms(UHCI_FRAME_SETTLE_MS);
    c->transfer_count++;
    uhci_leave_transfer(c);
    return r;
}

static int valid_packet(uint8_t speed, uint16_t max_packet) {
    if (max_packet == 0 || max_packet > 64) return 0;
    if (speed == USB_SPEED_LOW && max_packet > 8) return 0;
    return 1;
}

static int uhci_submit_control(struct uhci_controller *c, uint8_t addr, uint8_t speed, uint8_t ep,
                               uint16_t max_packet, const struct usb_setup_packet *setup,
                               void *data, uint16_t len, uint32_t timeout_ms) {
    if (!c || !setup || ep != 0 || !valid_packet(speed, max_packet) || len > UHCI_MAX_DATA) return -1;
    if (len && !data) return -1;
    uint32_t packets = len ? (uint32_t)((len + max_packet - 1u) / max_packet) : 0;
    uint32_t td_count = 2u + packets;
    struct uhci_transfer t;
    if (alloc_transfer(&t, td_count, len, 1) != 0) return -1;
    memcpy(t.setup, setup, sizeof(*setup));
    if (len && !(setup->bmRequestType & USB_DIR_IN)) memcpy(t.data, data, len);

    uint32_t idx = 0;
    t.td[idx].link = (uint32_t)(t.td_dma.phys + sizeof(struct uhci_td));
    t.td[idx].status = td_status(speed == USB_SPEED_LOW, 0);
    t.td[idx].token = td_token(USB_PID_SETUP, addr, 0, 0, sizeof(*setup));
    t.td[idx].buffer = (uint32_t)t.setup_dma.phys;
    idx++;

    uint16_t done = 0;
    uint8_t toggle = 1;
    while (done < len) {
        uint16_t chunk = (uint16_t)(len - done);
        if (chunk > max_packet) chunk = max_packet;
        t.td[idx].link = (idx + 1u < td_count) ? (uint32_t)(t.td_dma.phys + sizeof(struct uhci_td) * (idx + 1u)) : UHCI_LINK_TERM;
        t.td[idx].status = td_status(speed == USB_SPEED_LOW, (setup->bmRequestType & USB_DIR_IN) ? UHCI_TD_SPD : 0);
        t.td[idx].token = td_token((setup->bmRequestType & USB_DIR_IN) ? USB_PID_IN : USB_PID_OUT,
                                   addr, 0, toggle, chunk);
        t.td[idx].buffer = (uint32_t)(t.data_dma.phys + done);
        done = (uint16_t)(done + chunk);
        toggle ^= 1u;
        idx++;
    }

    uint8_t status_pid = (setup->bmRequestType & USB_DIR_IN) ? USB_PID_OUT : USB_PID_IN;
    t.td[idx].link = UHCI_LINK_TERM;
    t.td[idx].status = td_status(speed == USB_SPEED_LOW, UHCI_TD_IOC);
    t.td[idx].token = td_token(status_pid, addr, 0, 1, 0);
    t.td[idx].buffer = 0;

    uint32_t completed = 0;
    int r = run_transfer(c, &t, timeout_ms ? timeout_ms : UHCI_CONTROL_TIMEOUT_MS, &completed);
    if (r == 0 && len && (setup->bmRequestType & USB_DIR_IN)) {
        uint32_t got = 0;
        for (uint32_t i = 1; i < 1u + packets; i++) {
            uint32_t requested = len - got;
            if (requested > max_packet) requested = max_packet;
            uint32_t chunk = td_actual_len(&t.td[i]);
            if (chunk > requested) chunk = requested;
            got += chunk;
            if (chunk < requested) break;
        }
        if (got) memcpy(data, t.data, got);
    }
    if (r == 0 && completed != td_count) r = -2;
    free_transfer(&t);
    return r;
}

static uint8_t *bulk_toggle_slot(struct uhci_controller *c, struct usb_device *dev, uint8_t endpoint) {
    uint8_t addr = dev->address & 0x7fu;
    uint8_t ep = endpoint & 0x0fu;
    uint8_t in = (endpoint & USB_DIR_IN) ? 1u : 0u;
    return &c->bulk_toggle[addr][ep][in];
}

static int uhci_submit_bulk(struct uhci_controller *c, struct usb_device *dev, uint8_t endpoint,
                            uint16_t max_packet, void *data, uint32_t len, uint32_t *actual,
                            uint32_t timeout_ms) {
    if (actual) *actual = 0;
    if (!c || !dev || dev->disconnected || dev->speed == USB_SPEED_LOW || max_packet == 0 || max_packet > 64 || len > UHCI_MAX_DATA) return -1;
    if (len && !data) return -1;
    uint32_t packets = len ? (len + max_packet - 1u) / max_packet : 1u;
    if (packets == 0 || packets > UHCI_MAX_TDS) return -1;
    struct uhci_transfer t;
    if (alloc_transfer(&t, packets, len ? len : 1u, 0) != 0) return -1;
    int in = (endpoint & USB_DIR_IN) != 0;
    if (!in && len) memcpy(t.data, data, len);

    uint8_t *slot = bulk_toggle_slot(c, dev, endpoint);
    uint8_t toggle = *slot;
    uint32_t done = 0;
    for (uint32_t i = 0; i < packets; i++) {
        uint32_t chunk = len - done;
        if (chunk > max_packet) chunk = max_packet;
        t.td[i].link = (i + 1u < packets) ? (uint32_t)(t.td_dma.phys + sizeof(struct uhci_td) * (i + 1u)) : UHCI_LINK_TERM;
        t.td[i].status = td_status(0, (in ? UHCI_TD_SPD : 0) | (i + 1u == packets ? UHCI_TD_IOC : 0));
        t.td[i].token = td_token(in ? USB_PID_IN : USB_PID_OUT, dev->address, endpoint & 0x0f, toggle, (uint16_t)chunk);
        t.td[i].buffer = (uint32_t)(t.data_dma.phys + done);
        done += chunk;
        toggle ^= 1u;
    }

    uint32_t completed = 0;
    int r = run_transfer(c, &t, timeout_ms ? timeout_ms : UHCI_BULK_TIMEOUT_MS, &completed);
    uint32_t got = 0;
    if (r == 0) {
        for (uint32_t i = 0; i < packets; i++) {
            if (!td_done_ok(&t.td[i])) break;
            uint32_t chunk = td_actual_len(&t.td[i]);
            got += chunk;
            if (chunk < max_packet) break;
        }
        if (got > len) got = len;
        if (in && got) memcpy(data, t.data, got);
        if (!in) got = len;
        for (uint32_t i = 0; i < completed && i < packets; i++) *slot ^= 1u;
    }
    if (actual) *actual = got;
    free_transfer(&t);
    return r;
}

static int uhci_submit_isochronous(struct uhci_controller *c, struct usb_device *dev, uint8_t endpoint,
                                   uint16_t max_packet, void *data, uint32_t len, uint32_t *actual) {
    if (actual) *actual = 0;
    if (!c || !dev || dev->disconnected || dev->speed == USB_SPEED_LOW || max_packet == 0 || max_packet > 1023 || len > UHCI_MAX_DATA) return -1;
    if (len && !data) return -1;
    uint32_t packets = len ? (len + max_packet - 1u) / max_packet : 1u;
    if (packets == 0 || packets > 16u) return -1;

    struct uhci_transfer t;
    if (alloc_transfer(&t, packets, len ? len : 1u, 0) != 0) return -1;
    int in = (endpoint & USB_DIR_IN) != 0;
    if (!in && len) memcpy(t.data, data, len);

    uint32_t done = 0;
    for (uint32_t i = 0; i < packets; i++) {
        uint32_t chunk = len - done;
        if (chunk > max_packet) chunk = max_packet;
        t.td[i].link = UHCI_LINK_TERM;
        t.td[i].status = UHCI_TD_ACTIVE | UHCI_TD_ISO | (i + 1u == packets ? UHCI_TD_IOC : 0);
        t.td[i].token = td_token(in ? USB_PID_IN : USB_PID_OUT, dev->address, endpoint & 0x0f, 0, (uint16_t)chunk);
        t.td[i].buffer = (uint32_t)(t.data_dma.phys + done);
        done += chunk;
    }

    uhci_enter_transfer(c);
    uint32_t saved[16];
    uint16_t start = (uint16_t)((rd16(c, UHCI_FRNUM) + 4u) & (UHCI_FRAME_COUNT - 1u));
    uint64_t flags;
    spinlock_acquire(&c->lock, &flags);
    for (uint32_t i = 0; i < packets; i++) {
        uint32_t frame = (start + i) & (UHCI_FRAME_COUNT - 1u);
        saved[i] = c->frames[frame];
        t.td[i].link = saved[i];
        c->frames[frame] = (uint32_t)(t.td_dma.phys + sizeof(struct uhci_td) * i);
    }
    spinlock_release(&c->lock, flags);

    uint32_t completed = 0;
    int r = wait_transfer(c, &t, packets + 16u, &completed);
    if (r == -2) cancel_transfer(&t);

    spinlock_acquire(&c->lock, &flags);
    for (uint32_t i = 0; i < packets; i++) {
        uint32_t frame = (start + i) & (UHCI_FRAME_COUNT - 1u);
        if (c->frames[frame] == (uint32_t)(t.td_dma.phys + sizeof(struct uhci_td) * i)) c->frames[frame] = saved[i];
    }
    spinlock_release(&c->lock, flags);
    uhci_sleep_ms(UHCI_FRAME_SETTLE_MS);
    uhci_leave_transfer(c);

    uint32_t got = 0;
    if (r == 0) {
        for (uint32_t i = 0; i < packets; i++) got += td_actual_len(&t.td[i]);
        if (got > len) got = len;
        if (in && got) memcpy(data, t.data, got);
        if (!in) got = len;
    }
    if (actual) *actual = got;
    free_transfer(&t);
    return r;
}

static int uhci_interrupt_once(struct usb_interrupt_pipe *pipe) {
    if (!pipe) return -1;
    struct uhci_controller *c = (struct uhci_controller *)pipe->hcd->priv;
    if (!c || !pipe->dev || pipe->dev->disconnected || !pipe->buffer || pipe->buffer_len == 0 || pipe->buffer_len > 64) return -1;
    struct usb_device *udev = pipe->dev;
    uint16_t max_packet = pipe->max_packet ? pipe->max_packet : pipe->buffer_len;
    if (!valid_packet(udev->speed, max_packet) || pipe->buffer_len > max_packet) return -1;

    struct uhci_transfer t;
    if (alloc_transfer(&t, 1, pipe->buffer_len, 0) != 0) return -1;
    t.td[0].link = UHCI_LINK_TERM;
    t.td[0].status = td_status(udev->speed == USB_SPEED_LOW, UHCI_TD_IOC | UHCI_TD_SPD);
    t.td[0].token = td_token(USB_PID_IN, udev->address, pipe->endpoint & 0x0f, pipe->data_toggle, pipe->buffer_len);
    t.td[0].buffer = (uint32_t)t.data_dma.phys;

    uint32_t completed = 0;
    int r = run_transfer(c, &t, UHCI_INTR_POLL_MS, &completed);
    if (r == 0) {
        uint32_t got = td_actual_len(&t.td[0]);
        if (got > pipe->buffer_len) got = pipe->buffer_len;
        if (got) memcpy(pipe->buffer, t.data, got);
        pipe->data_toggle ^= 1u;
        if (pipe->active) pipe->callback(pipe, pipe->buffer, got, 0, pipe->callback_priv);
    } else if (r != -2 && pipe->active) {
        pipe->callback(pipe, pipe->buffer, 0, r, pipe->callback_priv);
    }
    free_transfer(&t);
    return r;
}

static uint32_t uhci_port_count(struct usb_hcd *hcd) {
    (void)hcd;
    return 2;
}

static uint16_t uhci_port_read(struct uhci_controller *c, uint32_t port) {
    return rd16(c, (uint16_t)(UHCI_PORTSC1 + port * 2u));
}

static void uhci_port_write(struct uhci_controller *c, uint32_t port, uint16_t value) {
    wr16(c, (uint16_t)(UHCI_PORTSC1 + port * 2u), value);
}

static void uhci_port_clear_changes(struct uhci_controller *c, uint32_t port) {
    uint16_t ps = uhci_port_read(c, port);
    uhci_port_write(c, port, (uint16_t)((ps & ~(UHCI_PORT_RESET | UHCI_PORT_SUSP)) | UHCI_PORT_CHANGE));
}

static int uhci_port_connected(struct usb_hcd *hcd, uint32_t port) {
    struct uhci_controller *c = (struct uhci_controller *)hcd->priv;
    if (!c || port >= 2 || !uhci_controller_live(c)) return 0;
    return (uhci_port_read(c, port) & UHCI_PORT_CCS) != 0;
}

static int uhci_port_reset(struct usb_hcd *hcd, uint32_t port, uint32_t *speed) {
    struct uhci_controller *c = (struct uhci_controller *)hcd->priv;
    if (!c || !speed || port >= 2 || !uhci_controller_live(c)) return -1;
    uint32_t deadline = uhci_timeout_deadline(UHCI_RESET_WAIT_MS, UHCI_RESET_WAIT_MS);
    while (!(uhci_port_read(c, port) & UHCI_PORT_CCS)) {
        if (uhci_deadline_passed(deadline)) return -1;
        uhci_sleep_ms(1);
    }

    uhci_port_clear_changes(c, port);
    uint16_t ps = uhci_port_read(c, port);
    uhci_port_write(c, port, (uint16_t)((ps & ~UHCI_PORT_SUSP) | UHCI_PORT_RESET | UHCI_PORT_CHANGE));
    uhci_sleep_ms(50);
    ps = uhci_port_read(c, port);
    uhci_port_write(c, port, (uint16_t)((ps & ~(UHCI_PORT_RESET | UHCI_PORT_SUSP)) | UHCI_PORT_CHANGE));
    uhci_sleep_ms(20);

    ps = uhci_port_read(c, port);
    if (!(ps & UHCI_PORT_CCS)) return -1;
    uhci_port_write(c, port, (uint16_t)((ps | UHCI_PORT_PE | UHCI_PORT_CHANGE) & ~(UHCI_PORT_RESET | UHCI_PORT_SUSP)));
    uhci_sleep_ms(10);
    ps = uhci_port_read(c, port);
    if (!(ps & UHCI_PORT_PE)) return -1;
    *speed = (ps & UHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
    uhci_port_clear_changes(c, port);
    return 0;
}

static int uhci_control(struct usb_hcd *hcd, uint8_t addr, uint8_t speed, uint8_t ep,
                        uint16_t max_packet, const struct usb_setup_packet *setup,
                        void *data, uint16_t len, uint32_t timeout_ms) {
    if (!hcd) return -1;
    return uhci_submit_control((struct uhci_controller *)hcd->priv, addr, speed, ep, max_packet,
                               setup, data, len, timeout_ms);
}

static int uhci_bulk(struct usb_hcd *hcd, struct usb_device *dev, uint8_t endpoint,
                     uint16_t max_packet, void *data, uint32_t len, uint32_t *actual,
                     uint32_t timeout_ms) {
    if (!hcd) return -1;
    return uhci_submit_bulk((struct uhci_controller *)hcd->priv, dev, endpoint, max_packet,
                            data, len, actual, timeout_ms);
}

static int uhci_isochronous(struct usb_hcd *hcd, struct usb_device *dev, uint8_t endpoint,
                            uint16_t max_packet, void *data, uint32_t len, uint32_t *actual) {
    if (!hcd) return -1;
    return uhci_submit_isochronous((struct uhci_controller *)hcd->priv, dev, endpoint, max_packet,
                                   data, len, actual);
}

static enum irq_return uhci_irq(void *priv) {
    struct uhci_controller *c = (struct uhci_controller *)priv;
    if (!c) return IRQ_NOT_HANDLED;
    uint16_t st = rd16(c, UHCI_USBSTS);
    if ((st & (UHCI_STS_USBINT | UHCI_STS_ERROR | UHCI_STS_RD | UHCI_STS_HSE | UHCI_STS_HCPE)) == 0)
        return IRQ_NOT_HANDLED;
    wr16(c, UHCI_USBSTS, st);
    if (st & (UHCI_STS_HSE | UHCI_STS_HCPE)) {
        c->dead = 1;
        c->error_count++;
    }
    return IRQ_HANDLED;
}

static const struct usb_hcd_ops uhci_usb_ops = {
    .port_count = uhci_port_count,
    .port_connected = uhci_port_connected,
    .port_reset = uhci_port_reset,
    .control = uhci_control,
    .bulk = uhci_bulk,
    .isochronous = uhci_isochronous,
    .interrupt_in = uhci_interrupt_once,
    .destroy_pipe = NULL,
};

static int uhci_wait_cmd_clear(struct uhci_controller *c, uint16_t bit, uint32_t timeout_ms) {
    uint32_t deadline = uhci_timeout_deadline(timeout_ms, timeout_ms);
    while (rd16(c, UHCI_USBCMD) & bit) {
        if (uhci_deadline_passed(deadline)) return -1;
        uhci_sleep_ms(1);
    }
    return 0;
}

static int uhci_init_hw(struct uhci_controller *c) {
    wr16(c, UHCI_USBCMD, 0);
    uhci_sleep_ms(2);
    wr16(c, UHCI_USBCMD, UHCI_CMD_HCRESET);
    if (uhci_wait_cmd_clear(c, UHCI_CMD_HCRESET, 100) != 0) return -1;
    wr16(c, UHCI_USBSTS, 0xffff);
    wr16(c, UHCI_USBINTR, 0x000fu);
    wr16(c, UHCI_FRNUM, 0);
    wr32(c, UHCI_FLBASE, (uint32_t)c->frame_dma.phys);
    wr8(c, UHCI_SOFMOD, 0x40);
    for (uint32_t i = 0; i < 2; i++) uhci_port_clear_changes(c, i);
    wr16(c, UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    uhci_sleep_ms(10);
    uint16_t st = rd16(c, UHCI_USBSTS);
    if (st & (UHCI_STS_HCH | UHCI_STS_HSE | UHCI_STS_HCPE)) return -1;
    return 0;
}

static void uhci_stop_hw(struct uhci_controller *c) {
    if (!c) return;
    c->dead = 1;
    wr16(c, UHCI_USBINTR, 0);
    wr16(c, UHCI_USBCMD, 0);
    uhci_sleep_ms(2);
    wr16(c, UHCI_USBSTS, 0xffff);
}

static int uhci_probe(struct device *dev) {
    struct pci_device *pdev = (struct pci_device *)dev->bus_data;
    if (!pdev || pdev->bars[4].type != PCI_BAR_TYPE_IO || pdev->bars[4].base == 0 || pdev->bars[4].base > 0xff00u) return -1;
    struct uhci_controller *c = (struct uhci_controller *)kzalloc(sizeof(*c));
    if (!c) return -1;
    c->pdev = pdev;
    c->io = (uint16_t)pdev->bars[4].base;
    c->irq = pci_read8(pdev->segment, pdev->bus, pdev->slot, pdev->func, 0x3c);
    c->index = next_index++;
    spinlock_init(&c->lock);

    if (dma_alloc_coherent(UHCI_FRAME_COUNT * sizeof(uint32_t), 4096, DMA_MASK_32, &c->frame_dma) != 0) goto fail;
    if (dma_alloc_coherent(sizeof(struct uhci_qh), 16, DMA_MASK_32, &c->skeleton_dma) != 0) goto fail;
    if (!dma32_ok(&c->frame_dma) || !dma32_ok(&c->skeleton_dma)) goto fail;
    c->frames = (uint32_t *)c->frame_dma.virt;
    c->skeleton = (struct uhci_qh *)c->skeleton_dma.virt;
    memset(c->frames, 0, UHCI_FRAME_COUNT * sizeof(uint32_t));
    memset(c->skeleton, 0, sizeof(*c->skeleton));
    c->skeleton->link = UHCI_LINK_TERM;
    c->skeleton->element = UHCI_LINK_TERM;
    for (uint32_t i = 0; i < UHCI_FRAME_COUNT; i++) c->frames[i] = (uint32_t)c->skeleton_dma.phys | UHCI_LINK_QH;

    pci_enable_bus_mastering(pdev->segment, pdev->bus, pdev->slot, pdev->func);
    pci_write16(pdev->segment, pdev->bus, pdev->slot, pdev->func, 0xc0, 0x2000);

    if (uhci_init_hw(c) != 0) goto fail_stop;
    if (c->irq && c->irq != 0xff) {
        if (request_irq(c->irq, uhci_irq, IRQ_AFFINITY_ALL, c) == 0) c->irq_registered = true;
        else klog(LOG_WARN, "UHCI: failed to register irq=%u; continuing with polling\n", c->irq);
    }

    c->hcd.name = "uhci";
    c->hcd.priv = c;
    c->hcd.ops = &uhci_usb_ops;
    if (usb_hcd_register(&c->hcd) != 0) goto fail_stop;

    c->next = controllers;
    controllers = c;
    dev->driver_data = c;
    klog(LOG_INFO, "UHCI: controller io=%04x irq=%u\n", c->io, c->irq);
    return 0;
fail_stop:
    if (c->irq_registered) free_irq(c->irq, uhci_irq);
    uhci_stop_hw(c);
fail:
    if (c->skeleton_dma.virt) dma_free_coherent(&c->skeleton_dma);
    if (c->frame_dma.virt) dma_free_coherent(&c->frame_dma);
    kfree(c);
    return -1;
}

static void uhci_remove(struct device *dev) {
    struct uhci_controller *c = (struct uhci_controller *)dev->driver_data;
    if (!c) return;
    uhci_stop_hw(c);
    usb_hcd_unregister(&c->hcd);
    if (c->irq_registered) free_irq(c->irq, uhci_irq);
    while (__sync_lock_test_and_set(&c->transfer_busy, 1u)) uhci_sleep_ms(1);
    dma_free_coherent(&c->skeleton_dma);
    dma_free_coherent(&c->frame_dma);
    uhci_leave_transfer(c);
    kfree(c);
    dev->driver_data = NULL;
}

static const struct pci_device_id uhci_ids[] = {
    {.vendor_id = PCI_ANY_ID, .device_id = PCI_ANY_ID, .class_code = PCI_CLASS_SERIAL,
     .subclass = PCI_SUBCLASS_USB, .prog_if = PCI_PROGIF_UHCI},
    {0, 0, 0, 0, 0},
};

static struct driver uhci_driver = {
    .name = "uhci",
    .bus = NULL,
    .module = NULL,
    .id_table = uhci_ids,
    .probe = uhci_probe,
    .remove = uhci_remove,
    .next = NULL,
};

static int uhci_init(void) {
    uhci_driver.bus = find_bus("pci");
    if (!uhci_driver.bus) return -1;
    driver_register(&uhci_driver);
    return 0;
}

MODULE_INFO("uhci", uhci_init, 0, NULL, "pci_bus", "usb_core");
