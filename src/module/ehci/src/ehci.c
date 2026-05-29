#include "usb_core.h"

#include "mod/device.h"
#include "mod/dma.h"
#include "mod/heap.h"
#include "mod/interrupt.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "mod/scheduler.h"
#include "mod/vmm.h"
#include "pci_bus.h"
#include "smp/lock.h"
#include "string.h"

#define PCI_CLASS_SERIAL 0x0cu
#define PCI_SUBCLASS_USB 0x03u
#define PCI_PROGIF_EHCI  0x20u

#define EHCI_CAP_CAPLENGTH 0x00u
#define EHCI_CAP_HCIVERSION 0x02u
#define EHCI_CAP_HCSPARAMS 0x04u
#define EHCI_CAP_HCCPARAMS 0x08u

#define EHCI_USBCMD      0x00u
#define EHCI_USBSTS      0x04u
#define EHCI_USBINTR     0x08u
#define EHCI_FRINDEX     0x0cu
#define EHCI_CTRLDSSEG   0x10u
#define EHCI_PERIODIC    0x14u
#define EHCI_ASYNC       0x18u
#define EHCI_CONFIGFLAG  0x40u
#define EHCI_PORTSC_BASE 0x44u

#define EHCI_CMD_RS       0x00000001u
#define EHCI_CMD_HCRESET  0x00000002u
#define EHCI_CMD_PSE      0x00000010u
#define EHCI_CMD_ASE      0x00000020u
#define EHCI_CMD_IAAD     0x00000040u
#define EHCI_CMD_ASPMC_3  0x00000300u
#define EHCI_CMD_ASPE     0x00000800u
#define EHCI_CMD_ITC_1    0x00010000u

#define EHCI_STS_USBINT   0x00000001u
#define EHCI_STS_USBERR   0x00000002u
#define EHCI_STS_PCD      0x00000004u
#define EHCI_STS_FLR      0x00000008u
#define EHCI_STS_HSE      0x00000010u
#define EHCI_STS_IAA      0x00000020u
#define EHCI_STS_HCH      0x00001000u
#define EHCI_STS_PSS      0x00004000u
#define EHCI_STS_ASS      0x00008000u

#define EHCI_PORT_CCS     0x00000001u
#define EHCI_PORT_CSC     0x00000002u
#define EHCI_PORT_PE      0x00000004u
#define EHCI_PORT_PEC     0x00000008u
#define EHCI_PORT_OCA     0x00000010u
#define EHCI_PORT_OCC     0x00000020u
#define EHCI_PORT_RESET   0x00000100u
#define EHCI_PORT_POWER   0x00001000u
#define EHCI_PORT_OWNER   0x00002000u
#define EHCI_PORT_CHANGE  (EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC)
#define EHCI_PORT_RW_MASK 0x00002d05u

#define EHCI_LINK_TERM 1u
#define EHCI_LINK_ITD  0u
#define EHCI_LINK_QH   2u
#define EHCI_LINK_SITD 4u

#define EHCI_QTD_NEXT_TERM 1u
#define EHCI_QTD_STS_ACTIVE 0x00000080u
#define EHCI_QTD_STS_HALTED 0x00000040u
#define EHCI_QTD_STS_DBE    0x00000020u
#define EHCI_QTD_STS_BABBLE 0x00000010u
#define EHCI_QTD_STS_XACT   0x00000008u
#define EHCI_QTD_STS_MMF    0x00000004u
#define EHCI_QTD_STS_SPLIT  0x00000002u
#define EHCI_QTD_STS_PING   0x00000001u
#define EHCI_QTD_STS_FATAL  (EHCI_QTD_STS_HALTED | EHCI_QTD_STS_DBE | EHCI_QTD_STS_BABBLE | EHCI_QTD_STS_XACT | EHCI_QTD_STS_MMF | EHCI_QTD_STS_SPLIT)

#define EHCI_PID_OUT   0u
#define EHCI_PID_IN    1u
#define EHCI_PID_SETUP 2u

#define EHCI_ITD_ACTIVE 0x80000000u
#define EHCI_ITD_IOC    0x00008000u
#define EHCI_SITD_ACTIVE 0x00000080u
#define EHCI_SITD_IOC    0x80000000u

#define EHCI_MAX_QTDS 192u
#define EHCI_MAX_DATA 65536u
#define EHCI_MAX_QTD_BYTES 16384u
#define EHCI_PERIODIC_FRAMES 1024u
#define EHCI_CONTROL_TIMEOUT_MS 500u
#define EHCI_BULK_TIMEOUT_MS 1000u
#define EHCI_INTR_TIMEOUT_MS 100u
#define EHCI_ISO_TIMEOUT_MS 100u
#define EHCI_RESET_MS 50u
#define EHCI_FRAME_SETTLE_MS 2u

#ifndef USB_SPEED_HIGH
#define USB_SPEED_HIGH 3u
#endif

struct ehci_qtd {
    uint32_t next;
    uint32_t alt_next;
    uint32_t token;
    uint32_t buffer[5];
    uint32_t ext_buffer[5];
} __attribute__((packed, aligned(32)));

struct ehci_qh {
    uint32_t horiz_link;
    uint32_t ep_char;
    uint32_t ep_cap;
    uint32_t current_qtd;
    struct ehci_qtd overlay;
} __attribute__((packed, aligned(32)));

struct ehci_itd {
    uint32_t next;
    uint32_t trans[8];
    uint32_t buffer[7];
    uint32_t ext_buffer[7];
} __attribute__((packed, aligned(32)));

struct ehci_sitd {
    uint32_t next;
    uint32_t ep_char;
    uint32_t uframe;
    uint32_t state;
    uint32_t buffer[2];
    uint32_t back;
    uint32_t ext_buffer[2];
} __attribute__((packed, aligned(32)));

struct ehci_transfer {
    dma_allocation_t qh_dma;
    dma_allocation_t qtd_dma;
    dma_allocation_t setup_dma;
    dma_allocation_t data_dma;
    struct ehci_qh *qh;
    struct ehci_qtd *qtd;
    void *setup;
    void *data;
    uint32_t qtd_count;
};

struct ehci_controller {
    struct pci_device *pdev;
    struct device *dev;
    volatile uint8_t *cap_regs;
    volatile uint32_t *op_regs;
    uint64_t regs_size;
    uint32_t op_offset;
    uint8_t irq;
    bool irq_registered;
    spinlock_t lock;
    dma_allocation_t async_dma;
    dma_allocation_t periodic_dma;
    struct ehci_qh *async_head;
    uint32_t *periodic;
    struct usb_hcd hcd;
    volatile uint32_t transfer_busy;
    volatile uint32_t dead;
    bool hcd_registered;
    uint32_t ports;
    uint64_t dma_mask;
    uint8_t bulk_toggle[128][16][2];
    uint64_t transfer_count;
    uint64_t timeout_count;
    uint64_t error_count;
    uint64_t recovery_count;
    struct ehci_controller *next;
};

static struct ehci_controller *controllers;

static inline uint8_t cap8(struct ehci_controller *c, uint32_t off) { return c->cap_regs[off]; }
static inline uint16_t cap16(struct ehci_controller *c, uint32_t off) { return *(volatile uint16_t *)(c->cap_regs + off); }
static inline uint32_t cap32(struct ehci_controller *c, uint32_t off) { return *(volatile uint32_t *)(c->cap_regs + off); }
static inline uint32_t op32(struct ehci_controller *c, uint32_t off) { return c->op_regs[off >> 2]; }
static inline void wr32(struct ehci_controller *c, uint32_t off, uint32_t val) { c->op_regs[off >> 2] = val; (void)c->op_regs[off >> 2]; }

static void ehci_sleep_ms(uint32_t ms) {
    if (ms == 0) return;
    if (kernel_can_sleep()) {
        (void)kernel_sleep_ticks(ms);
        return;
    }
    for (uint32_t i = 0; i < ms * 50000u; i++) asm volatile("pause");
}

static uint64_t ehci_deadline(uint32_t timeout_ms, uint32_t fallback_ms) {
    uint32_t use = timeout_ms ? timeout_ms : fallback_ms;
    if (use == 0) use = 1;
    return kernel_monotonic_ticks() + use;
}

static int ehci_deadline_passed(uint64_t deadline) {
    return (int64_t)(kernel_monotonic_ticks() - deadline) >= 0;
}

static int dma_ok(const dma_allocation_t *a, uint64_t mask) {
    if (!a->virt) return 1;
    if (a->size == 0) return 1;
    if (a->phys > mask) return 0;
    if (a->phys + a->size - 1u < a->phys) return 0;
    return a->phys + a->size - 1u <= mask;
}

static void ehci_enter_transfer(struct ehci_controller *c) {
    while (__sync_lock_test_and_set(&c->transfer_busy, 1u)) {
        if (kernel_can_sleep()) (void)kernel_sleep_ticks(1);
        else asm volatile("pause");
    }
    __sync_synchronize();
}

static void ehci_leave_transfer(struct ehci_controller *c) {
    __sync_synchronize();
    __sync_lock_release(&c->transfer_busy);
}

static int ehci_live(struct ehci_controller *c) {
    if (!c || c->dead) return 0;
    uint32_t st = op32(c, EHCI_USBSTS);
    if (st & EHCI_STS_HSE) {
        c->dead = 1;
        c->error_count++;
        klog(LOG_ERR, "EHCI: host system error mmio=%016llx sts=%08x\n", (unsigned long long)c->pdev->bars[0].base, st);
        return 0;
    }
    return 1;
}

static uint32_t qh_speed(uint8_t speed) {
    if (speed == USB_SPEED_HIGH) return 2u;
    if (speed == USB_SPEED_LOW) return 1u;
    return 0u;
}

static uint8_t tt_hub_addr(struct usb_device *dev) {
    struct usb_device *p = dev ? dev->parent : NULL;
    while (p && p->parent && p->speed != USB_SPEED_HIGH) p = p->parent;
    return p ? (p->address & 0x7fu) : 0;
}

static uint8_t tt_port(struct usb_device *dev) {
    struct usb_device *p = dev;
    while (p && p->parent && p->parent->speed != USB_SPEED_HIGH) p = p->parent;
    return p ? (uint8_t)(p->hub_port & 0x7fu) : 0;
}

static uint32_t split_masks(uint8_t speed, uint8_t xfer_type) {
    if (speed == USB_SPEED_HIGH) return 0;
    if (xfer_type == USB_ENDPOINT_XFER_INTR) return 0x01u | (0x1cu << 8);
    return 0x01u | (0x1cu << 8);
}

static uint32_t qh_ep_char(uint8_t addr, uint8_t ep, uint8_t speed, uint16_t max_packet, int control, int head) {
    return ((uint32_t)(addr & 0x7fu)) | ((uint32_t)(ep & 0x0fu) << 8) |
           (qh_speed(speed) << 12) | (1u << 14) | (head ? (1u << 15) : 0) |
           ((uint32_t)max_packet << 16) | ((control && speed != USB_SPEED_HIGH) ? (1u << 27) : 0) |
           (8u << 28);
}

static uint32_t qh_ep_cap_dev(struct usb_device *dev, uint8_t xfer_type) {
    if (!dev || dev->speed == USB_SPEED_HIGH) return 1u << 30;
    return split_masks((uint8_t)dev->speed, xfer_type) |
           ((uint32_t)tt_hub_addr(dev) << 16) | ((uint32_t)tt_port(dev) << 23) | (1u << 30);
}

static uint32_t qh_ep_cap_control(uint8_t speed) {
    if (speed == USB_SPEED_HIGH) return 1u << 30;
    return split_masks(speed, USB_ENDPOINT_XFER_CONTROL) | (1u << 30);
}

static uint32_t qtd_token(uint32_t pid, uint32_t toggle, uint32_t len, int ioc) {
    return EHCI_QTD_STS_ACTIVE | (3u << 10) | (ioc ? (1u << 15) : 0) |
           ((len & 0x7fffu) << 16) | ((pid & 3u) << 8) | ((toggle & 1u) << 31);
}

static void desc_set_ptr(uint32_t *lo, uint32_t *hi, uint32_t count, uint64_t phys) {
    for (uint32_t i = 0; i < count; i++) {
        lo[i] = 0;
        hi[i] = 0;
    }
    lo[0] = (uint32_t)phys;
    hi[0] = (uint32_t)(phys >> 32);
}

static void qtd_set_buffer(struct ehci_qtd *qtd, uint64_t phys, uint32_t len) {
    for (uint32_t i = 0; i < 5; i++) {
        qtd->buffer[i] = 0;
        qtd->ext_buffer[i] = 0;
    }
    if (len == 0) return;
    uint64_t p = phys;
    uint32_t left = len;
    for (uint32_t i = 0; i < 5 && left; i++) {
        qtd->buffer[i] = (uint32_t)p;
        qtd->ext_buffer[i] = (uint32_t)(p >> 32);
        uint32_t page_left = 4096u - ((uint32_t)p & 4095u);
        if (page_left > left) page_left = left;
        p += page_left;
        left -= page_left;
        if (left) p = (p + 4095u) & ~UINT64_C(4095);
    }
}

static uint32_t qtd_remaining(const struct ehci_qtd *qtd) {
    return (qtd->token >> 16) & 0x7fffu;
}

static int qtd_error_result(const struct ehci_qtd *qtd) {
    uint32_t st = qtd->token & 0xffu;
    if (st & EHCI_QTD_STS_ACTIVE) return 1;
    if (st & EHCI_QTD_STS_FATAL) return -3;
    return 0;
}

static void ehci_recover_async(struct ehci_controller *c) {
    uint64_t flags;
    c->recovery_count++;
    spinlock_acquire(&c->lock, &flags);
    wr32(c, EHCI_USBCMD, op32(c, EHCI_USBCMD) & ~EHCI_CMD_ASE);
    spinlock_release(&c->lock, flags);
    uint64_t deadline = ehci_deadline(100, 100);
    while ((op32(c, EHCI_USBSTS) & EHCI_STS_ASS) && !ehci_deadline_passed(deadline)) ehci_sleep_ms(1);
    memset(c->async_head, 0, sizeof(*c->async_head));
    c->async_head->horiz_link = ((uint32_t)c->async_dma.phys) | EHCI_LINK_QH;
    c->async_head->ep_char = qh_ep_char(0, 0, USB_SPEED_HIGH, 64, 0, 1);
    c->async_head->ep_cap = 1u << 30;
    c->async_head->overlay.next = EHCI_QTD_NEXT_TERM;
    c->async_head->overlay.alt_next = EHCI_QTD_NEXT_TERM;
    c->async_head->overlay.token = EHCI_QTD_STS_HALTED;
    wr32(c, EHCI_ASYNC, (uint32_t)c->async_dma.phys);
    wr32(c, EHCI_USBSTS, EHCI_STS_USBERR | EHCI_STS_IAA);
    wr32(c, EHCI_USBCMD, op32(c, EHCI_USBCMD) | EHCI_CMD_ASE | EHCI_CMD_IAAD);
}

static void free_transfer(struct ehci_transfer *t) {
    if (!t) return;
    dma_free_coherent(&t->data_dma);
    dma_free_coherent(&t->setup_dma);
    dma_free_coherent(&t->qtd_dma);
    dma_free_coherent(&t->qh_dma);
    memset(t, 0, sizeof(*t));
}

static int alloc_transfer(struct ehci_controller *c, struct ehci_transfer *t, uint32_t qtd_count, uint32_t data_len, int need_setup) {
    memset(t, 0, sizeof(*t));
    if (!c || qtd_count == 0 || qtd_count > EHCI_MAX_QTDS || data_len > EHCI_MAX_DATA) return -1;
    if (dma_alloc_coherent(sizeof(struct ehci_qh), 32, c->dma_mask, &t->qh_dma) != 0) return -1;
    if (dma_alloc_coherent(sizeof(struct ehci_qtd) * qtd_count, 32, c->dma_mask, &t->qtd_dma) != 0) goto fail;
    if (need_setup && dma_alloc_coherent(sizeof(struct usb_setup_packet), 8, c->dma_mask, &t->setup_dma) != 0) goto fail;
    if (data_len && dma_alloc_coherent(data_len, 32, c->dma_mask, &t->data_dma) != 0) goto fail;
    if (!dma_ok(&t->qh_dma, c->dma_mask) || !dma_ok(&t->qtd_dma, c->dma_mask) ||
        !dma_ok(&t->setup_dma, c->dma_mask) || !dma_ok(&t->data_dma, c->dma_mask)) goto fail;
    t->qh = (struct ehci_qh *)t->qh_dma.virt;
    t->qtd = (struct ehci_qtd *)t->qtd_dma.virt;
    t->setup = t->setup_dma.virt;
    t->data = t->data_dma.virt;
    t->qtd_count = qtd_count;
    memset(t->qh, 0, sizeof(*t->qh));
    memset(t->qtd, 0, sizeof(struct ehci_qtd) * qtd_count);
    return 0;
fail:
    free_transfer(t);
    return -1;
}

static uint8_t *bulk_toggle_slot(struct ehci_controller *c, struct usb_device *dev, uint8_t endpoint) {
    uint8_t addr = dev->address & 0x7fu;
    uint8_t ep = endpoint & 0x0fu;
    uint8_t dir = (endpoint & USB_DIR_IN) ? 1u : 0u;
    return &c->bulk_toggle[addr][ep][dir];
}

static void link_qtd_chain(struct ehci_transfer *t) {
    for (uint32_t i = 0; i < t->qtd_count; i++) {
        uint32_t next = (i + 1u < t->qtd_count) ? (uint32_t)(t->qtd_dma.phys + sizeof(struct ehci_qtd) * (i + 1u)) : EHCI_QTD_NEXT_TERM;
        t->qtd[i].next = next;
        t->qtd[i].alt_next = EHCI_QTD_NEXT_TERM;
    }
    t->qh->current_qtd = 0;
    t->qh->overlay.next = (uint32_t)t->qtd_dma.phys;
    t->qh->overlay.alt_next = EHCI_QTD_NEXT_TERM;
    t->qh->overlay.token = 0;
}

static int wait_state(struct ehci_controller *c, uint32_t bit, int set, uint32_t timeout_ms) {
    uint64_t deadline = ehci_deadline(timeout_ms, timeout_ms);
    for (;;) {
        uint32_t st = op32(c, EHCI_USBSTS);
        if (((st & bit) != 0) == set) return 0;
        if (ehci_deadline_passed(deadline)) return -1;
        ehci_sleep_ms(1);
    }
}

static int wait_transfer(struct ehci_controller *c, struct ehci_transfer *t, uint32_t timeout_ms, uint32_t *completed) {
    uint64_t deadline = ehci_deadline(timeout_ms, timeout_ms);
    if (completed) *completed = 0;
    for (;;) {
        if (!ehci_live(c)) return -5;
        uint32_t done = 0;
        int active = 0;
        for (uint32_t i = 0; i < t->qtd_count; i++) {
            int r = qtd_error_result(&t->qtd[i]);
            if (r == 1) { active = 1; continue; }
            if (r < 0) { c->error_count++; if (completed) *completed = done; return r; }
            done++;
        }
        if (!active) { if (completed) *completed = done; return 0; }
        if (ehci_deadline_passed(deadline)) { c->timeout_count++; if (completed) *completed = done; return -2; }
        if (kernel_can_sleep()) (void)kernel_sleep_ticks(1);
        else asm volatile("pause");
    }
}

static int run_async_transfer(struct ehci_controller *c, struct ehci_transfer *t, uint32_t timeout_ms, uint32_t *completed) {
    if (!ehci_live(c)) return -5;
    ehci_enter_transfer(c);
    if (!ehci_live(c)) { ehci_leave_transfer(c); return -5; }
    link_qtd_chain(t);
    t->qh->horiz_link = ((uint32_t)c->async_dma.phys) | EHCI_LINK_QH;

    uint64_t flags;
    spinlock_acquire(&c->lock, &flags);
    c->async_head->horiz_link = ((uint32_t)t->qh_dma.phys) | EHCI_LINK_QH;
    wr32(c, EHCI_USBCMD, op32(c, EHCI_USBCMD) | EHCI_CMD_ASE | EHCI_CMD_IAAD);
    spinlock_release(&c->lock, flags);
    (void)wait_state(c, EHCI_STS_ASS, 1, 100);

    int r = wait_transfer(c, t, timeout_ms, completed);

    spinlock_acquire(&c->lock, &flags);
    c->async_head->horiz_link = ((uint32_t)c->async_dma.phys) | EHCI_LINK_QH;
    wr32(c, EHCI_USBCMD, op32(c, EHCI_USBCMD) | EHCI_CMD_IAAD);
    spinlock_release(&c->lock, flags);
    ehci_sleep_ms(EHCI_FRAME_SETTLE_MS);
    if (r < 0) ehci_recover_async(c);
    c->transfer_count++;
    ehci_leave_transfer(c);
    return r;
}

static int valid_control_packet(uint8_t speed, uint16_t max_packet) {
    if (speed == USB_SPEED_LOW) return max_packet == 8;
    return max_packet == 8 || max_packet == 16 || max_packet == 32 || max_packet == 64;
}

static int valid_bulk_packet(uint8_t speed, uint16_t max_packet) {
    if (speed != USB_SPEED_HIGH && speed != USB_SPEED_FULL) return 0;
    return max_packet != 0 && max_packet <= (speed == USB_SPEED_HIGH ? 512u : 64u);
}

static int ehci_submit_control_dev(struct ehci_controller *c, struct usb_device *dev, uint8_t addr, uint8_t speed, uint8_t ep,
                                   uint16_t max_packet, const struct usb_setup_packet *setup,
                                   void *data, uint16_t len, uint32_t timeout_ms) {
    if (!c || !setup || ep != 0 || !valid_control_packet(speed, max_packet)) return -1;
    if (len && !data) return -1;
    uint32_t packets = len ? (uint32_t)((len + max_packet - 1u) / max_packet) : 0;
    uint32_t qtd_count = 2u + packets;
    struct ehci_transfer t;
    if (alloc_transfer(c, &t, qtd_count, len, 1) != 0) return -1;
    memcpy(t.setup, setup, sizeof(*setup));
    if (len && !(setup->bmRequestType & USB_DIR_IN)) memcpy(t.data, data, len);
    t.qh->ep_char = qh_ep_char(addr, 0, speed, max_packet, 1, 0);
    t.qh->ep_cap = dev ? qh_ep_cap_dev(dev, USB_ENDPOINT_XFER_CONTROL) : qh_ep_cap_control(speed);

    uint32_t idx = 0;
    t.qtd[idx].token = qtd_token(EHCI_PID_SETUP, 0, sizeof(*setup), 0);
    qtd_set_buffer(&t.qtd[idx], t.setup_dma.phys, sizeof(*setup));
    idx++;

    uint32_t done = 0;
    uint32_t toggle = 1;
    uint32_t data_pid = (setup->bmRequestType & USB_DIR_IN) ? EHCI_PID_IN : EHCI_PID_OUT;
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > max_packet) chunk = max_packet;
        t.qtd[idx].token = qtd_token(data_pid, toggle, chunk, 0);
        qtd_set_buffer(&t.qtd[idx], t.data_dma.phys + done, chunk);
        done += chunk;
        toggle ^= 1u;
        idx++;
    }

    uint32_t status_pid = (setup->bmRequestType & USB_DIR_IN) ? EHCI_PID_OUT : EHCI_PID_IN;
    t.qtd[idx].token = qtd_token(status_pid, 1, 0, 1);

    uint32_t completed = 0;
    int r = run_async_transfer(c, &t, timeout_ms ? timeout_ms : EHCI_CONTROL_TIMEOUT_MS, &completed);
    if (r == 0 && len && (setup->bmRequestType & USB_DIR_IN)) {
        uint32_t got = 0;
        for (uint32_t i = 1, offset = 0; i < idx; i++) {
            uint32_t requested = len - offset;
            if (requested > max_packet) requested = max_packet;
            uint32_t left = qtd_remaining(&t.qtd[i]);
            uint32_t actual = left >= requested ? 0 : requested - left;
            got += actual;
            offset += requested;
            if (actual < requested) break;
        }
        if (got > len) got = len;
        if (got) memcpy(data, t.data, got);
    }
    free_transfer(&t);
    return r;
}

static int ehci_submit_bulk(struct ehci_controller *c, struct usb_device *dev, uint8_t endpoint,
                            uint16_t max_packet, void *data, uint32_t len, uint32_t *actual,
                            uint32_t timeout_ms) {
    if (actual) *actual = 0;
    if (!c || !dev || dev->disconnected || !valid_bulk_packet((uint8_t)dev->speed, max_packet)) return -1;
    if (len && !data) return -1;
    uint32_t packets = len ? (uint32_t)((len + max_packet - 1u) / max_packet) : 1u;
    if (packets > EHCI_MAX_QTDS) return -1;
    struct ehci_transfer t;
    if (alloc_transfer(c, &t, packets, len, 0) != 0) return -1;
    int in = (endpoint & USB_DIR_IN) != 0;
    if (!in && len) memcpy(t.data, data, len);
    t.qh->ep_char = qh_ep_char(dev->address, endpoint & 0x0fu, (uint8_t)dev->speed, max_packet, 0, 0);
    t.qh->ep_cap = qh_ep_cap_dev(dev, USB_ENDPOINT_XFER_BULK);

    uint8_t *slot = bulk_toggle_slot(c, dev, endpoint);
    uint32_t toggle = *slot ? 1u : 0u;
    uint32_t done = 0;
    for (uint32_t i = 0; i < packets; i++) {
        uint32_t chunk = len - done;
        if (chunk > max_packet) chunk = max_packet;
        t.qtd[i].token = qtd_token(in ? EHCI_PID_IN : EHCI_PID_OUT, toggle, chunk, i + 1u == packets);
        qtd_set_buffer(&t.qtd[i], chunk ? t.data_dma.phys + done : 0, chunk);
        done += chunk;
        toggle ^= 1u;
    }

    uint32_t completed = 0;
    int r = run_async_transfer(c, &t, timeout_ms ? timeout_ms : EHCI_BULK_TIMEOUT_MS, &completed);
    uint32_t got = 0;
    if (r == 0) {
        for (uint32_t i = 0, offset = 0; i < packets; i++) {
            uint32_t requested = len - offset;
            if (requested > max_packet) requested = max_packet;
            uint32_t left = qtd_remaining(&t.qtd[i]);
            uint32_t chunk = left >= requested ? 0 : requested - left;
            got += chunk;
            offset += requested;
            if (chunk < requested) break;
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

static uint32_t periodic_slot(struct ehci_controller *c, uint32_t interval) {
    uint32_t frame = (op32(c, EHCI_FRINDEX) >> 3) & (EHCI_PERIODIC_FRAMES - 1u);
    uint32_t step = interval ? interval : 1u;
    if (step > 32u) step = 32u;
    return (frame + step) & (EHCI_PERIODIC_FRAMES - 1u);
}

static int periodic_link(struct ehci_controller *c, uint32_t slot, uint32_t link) {
    if (!c->periodic || slot >= EHCI_PERIODIC_FRAMES) return -1;
    c->periodic[slot] = link;
    wr32(c, EHCI_USBCMD, op32(c, EHCI_USBCMD) | EHCI_CMD_PSE);
    return wait_state(c, EHCI_STS_PSS, 1, 100);
}

static void periodic_unlink(struct ehci_controller *c, uint32_t slot) {
    if (c->periodic && slot < EHCI_PERIODIC_FRAMES) c->periodic[slot] = EHCI_LINK_TERM;
    ehci_sleep_ms(EHCI_FRAME_SETTLE_MS);
}

static int ehci_periodic_qh(struct ehci_controller *c, struct usb_device *dev, uint8_t endpoint,
                            uint16_t max_packet, void *data, uint32_t len, uint32_t *actual,
                            uint32_t timeout_ms, uint8_t interval) {
    if (actual) *actual = 0;
    if (!c || !dev || dev->disconnected || max_packet == 0 || len > max_packet) return -1;
    struct ehci_transfer t;
    if (alloc_transfer(c, &t, 1, len, 0) != 0) return -1;
    int in = (endpoint & USB_DIR_IN) != 0;
    if (!in && len) memcpy(t.data, data, len);
    t.qh->ep_char = qh_ep_char(dev->address, endpoint & 0x0fu, (uint8_t)dev->speed, max_packet, 0, 0);
    t.qh->ep_cap = qh_ep_cap_dev(dev, USB_ENDPOINT_XFER_INTR);
    t.qh->horiz_link = EHCI_LINK_TERM;
    t.qtd[0].token = qtd_token(in ? EHCI_PID_IN : EHCI_PID_OUT, 0, len, 1);
    qtd_set_buffer(&t.qtd[0], len ? t.data_dma.phys : 0, len);
    link_qtd_chain(&t);

    ehci_enter_transfer(c);
    uint32_t slot = periodic_slot(c, interval);
    int r = periodic_link(c, slot, ((uint32_t)t.qh_dma.phys) | EHCI_LINK_QH);
    uint32_t completed = 0;
    if (r == 0) r = wait_transfer(c, &t, timeout_ms ? timeout_ms : EHCI_INTR_TIMEOUT_MS, &completed);
    periodic_unlink(c, slot);
    if (r < 0) ehci_recover_async(c);
    ehci_leave_transfer(c);

    if (r == 0) {
        uint32_t left = qtd_remaining(&t.qtd[0]);
        uint32_t got = left >= len ? 0 : len - left;
        if (got > len) got = len;
        if (in && got) memcpy(data, t.data, got);
        if (!in) got = len;
        if (actual) *actual = got;
    }
    free_transfer(&t);
    return r;
}

static int ehci_interrupt_once(struct usb_interrupt_pipe *pipe) {
    if (!pipe || !pipe->hcd || !pipe->dev || !pipe->buffer) return -1;
    struct ehci_controller *c = (struct ehci_controller *)pipe->hcd->priv;
    if (!c || pipe->dev->disconnected || pipe->max_packet == 0 || pipe->buffer_len == 0 || pipe->buffer_len > pipe->max_packet) return -1;
    uint32_t actual = 0;
    int r = ehci_periodic_qh(c, pipe->dev, pipe->endpoint | USB_DIR_IN, pipe->max_packet, pipe->buffer,
                             pipe->buffer_len, &actual, EHCI_INTR_TIMEOUT_MS, pipe->interval);
    if (r == 0) {
        pipe->data_toggle ^= 1u;
        if (pipe->active) pipe->callback(pipe, pipe->buffer, actual, 0, pipe->callback_priv);
    } else if (r != -2 && pipe->active) {
        pipe->callback(pipe, pipe->buffer, 0, r, pipe->callback_priv);
    }
    return r;
}

static int wait_periodic_done(struct ehci_controller *c, volatile uint32_t *state, uint32_t active_mask, uint32_t timeout_ms) {
    uint64_t deadline = ehci_deadline(timeout_ms, timeout_ms);
    for (;;) {
        if (!ehci_live(c)) return -5;
        if (!(*state & active_mask)) return (*state & (EHCI_SITD_ACTIVE | EHCI_QTD_STS_FATAL)) ? -3 : 0;
        if (ehci_deadline_passed(deadline)) { c->timeout_count++; return -2; }
        if (kernel_can_sleep()) (void)kernel_sleep_ticks(1);
        else asm volatile("pause");
    }
}

static int ehci_iso_hs(struct ehci_controller *c, struct usb_device *dev, uint8_t endpoint,
                       uint16_t max_packet, void *data, uint32_t len, uint32_t *actual) {
    if (actual) *actual = 0;
    if (len > max_packet || len > 3072u) return -1;
    dma_allocation_t itd_dma;
    dma_allocation_t data_dma;
    memset(&itd_dma, 0, sizeof(itd_dma));
    memset(&data_dma, 0, sizeof(data_dma));
    struct ehci_itd *itd;
    int in;
    uint32_t page;
    uint32_t slot;
    int r;
    if (dma_alloc_coherent(sizeof(struct ehci_itd), 32, c->dma_mask, &itd_dma) != 0) return -1;
    if (len && dma_alloc_coherent(len, 32, c->dma_mask, &data_dma) != 0) goto fail;
    if (!dma_ok(&itd_dma, c->dma_mask) || !dma_ok(&data_dma, c->dma_mask)) goto fail;
    itd = (struct ehci_itd *)itd_dma.virt;
    memset(itd, 0, sizeof(*itd));
    in = (endpoint & USB_DIR_IN) != 0;
    if (!in && len) memcpy(data_dma.virt, data, len);
    itd->next = EHCI_LINK_TERM;
    page = len ? ((uint32_t)data_dma.phys & 4095u) : 0;
    itd->trans[0] = EHCI_ITD_ACTIVE | EHCI_ITD_IOC | ((len & 0x0fffu) << 16) | page;
    itd->buffer[0] = ((uint32_t)data_dma.phys & ~4095u) | (dev->address & 0x7fu) | ((uint32_t)(endpoint & 0x0fu) << 8);
    itd->buffer[1] = ((uint32_t)((data_dma.phys + 4095u) & ~UINT64_C(4095))) | (in ? (1u << 11) : 0u) | ((uint32_t)max_packet << 16);
    itd->buffer[2] = ((uint32_t)((data_dma.phys + 8191u) & ~UINT64_C(4095))) | (1u << 0);
    itd->ext_buffer[0] = (uint32_t)(data_dma.phys >> 32);
    itd->ext_buffer[1] = (uint32_t)((data_dma.phys + 4095u) >> 32);
    itd->ext_buffer[2] = (uint32_t)((data_dma.phys + 8191u) >> 32);

    ehci_enter_transfer(c);
    slot = periodic_slot(c, 1);
    r = periodic_link(c, slot, ((uint32_t)itd_dma.phys) | EHCI_LINK_ITD);
    if (r == 0) r = wait_periodic_done(c, &itd->trans[0], EHCI_ITD_ACTIVE, EHCI_ISO_TIMEOUT_MS);
    periodic_unlink(c, slot);
    ehci_leave_transfer(c);
    if (r == 0) {
        uint32_t remain = (itd->trans[0] >> 16) & 0x0fffu;
        uint32_t got = remain >= len ? 0 : len - remain;
        if (got > len) got = len;
        if (in && got) memcpy(data, data_dma.virt, got);
        if (!in) got = len;
        if (actual) *actual = got;
    }
    dma_free_coherent(&data_dma);
    dma_free_coherent(&itd_dma);
    return r;
fail:
    dma_free_coherent(&data_dma);
    dma_free_coherent(&itd_dma);
    return -1;
}

static int ehci_iso_fs(struct ehci_controller *c, struct usb_device *dev, uint8_t endpoint,
                       uint16_t max_packet, void *data, uint32_t len, uint32_t *actual) {
    if (actual) *actual = 0;
    if (len > max_packet || len > 1023u) return -1;
    dma_allocation_t sitd_dma;
    dma_allocation_t data_dma;
    memset(&sitd_dma, 0, sizeof(sitd_dma));
    memset(&data_dma, 0, sizeof(data_dma));
    struct ehci_sitd *s;
    int in;
    uint32_t slot;
    int r;
    if (dma_alloc_coherent(sizeof(struct ehci_sitd), 32, c->dma_mask, &sitd_dma) != 0) return -1;
    if (len && dma_alloc_coherent(len, 32, c->dma_mask, &data_dma) != 0) goto fail;
    if (!dma_ok(&sitd_dma, c->dma_mask) || !dma_ok(&data_dma, c->dma_mask)) goto fail;
    s = (struct ehci_sitd *)sitd_dma.virt;
    memset(s, 0, sizeof(*s));
    in = (endpoint & USB_DIR_IN) != 0;
    if (!in && len) memcpy(data_dma.virt, data, len);
    s->next = EHCI_LINK_TERM;
    s->ep_char = (dev->address & 0x7fu) | ((uint32_t)(endpoint & 0x0fu) << 8) | (in ? (1u << 31) : 0u) |
                 ((uint32_t)tt_hub_addr(dev) << 16) | ((uint32_t)tt_port(dev) << 24);
    s->uframe = 0x01u | (0x1cu << 8);
    s->state = EHCI_SITD_ACTIVE | EHCI_SITD_IOC | ((len & 0x03ffu) << 16);
    s->buffer[0] = (uint32_t)data_dma.phys;
    s->buffer[1] = ((uint32_t)(data_dma.phys + len) & ~4095u);
    s->back = EHCI_LINK_TERM;
    s->ext_buffer[0] = (uint32_t)(data_dma.phys >> 32);
    s->ext_buffer[1] = (uint32_t)((data_dma.phys + len) >> 32);

    ehci_enter_transfer(c);
    slot = periodic_slot(c, 1);
    r = periodic_link(c, slot, ((uint32_t)sitd_dma.phys) | EHCI_LINK_SITD);
    if (r == 0) r = wait_periodic_done(c, &s->state, EHCI_SITD_ACTIVE, EHCI_ISO_TIMEOUT_MS);
    periodic_unlink(c, slot);
    ehci_leave_transfer(c);
    if (r == 0) {
        uint32_t remain = (s->state >> 16) & 0x03ffu;
        uint32_t got = remain >= len ? 0 : len - remain;
        if (got > len) got = len;
        if (in && got) memcpy(data, data_dma.virt, got);
        if (!in) got = len;
        if (actual) *actual = got;
    }
    dma_free_coherent(&data_dma);
    dma_free_coherent(&sitd_dma);
    return r;
fail:
    dma_free_coherent(&data_dma);
    dma_free_coherent(&sitd_dma);
    return -1;
}

static int ehci_submit_isochronous(struct ehci_controller *c, struct usb_device *dev, uint8_t endpoint,
                                   uint16_t max_packet, void *data, uint32_t len, uint32_t *actual) {
    if (!c || !dev || dev->disconnected || max_packet == 0 || (len && !data)) return -1;
    if (dev->speed == USB_SPEED_HIGH) return ehci_iso_hs(c, dev, endpoint, max_packet, data, len, actual);
    if (dev->speed == USB_SPEED_FULL) return ehci_iso_fs(c, dev, endpoint, max_packet, data, len, actual);
    if (actual) *actual = 0;
    return -1;
}

static uint32_t ehci_port_reg(uint32_t port) { return EHCI_PORTSC_BASE + port * 4u; }
static uint32_t ehci_port_read(struct ehci_controller *c, uint32_t port) { return op32(c, ehci_port_reg(port)); }
static void ehci_port_write(struct ehci_controller *c, uint32_t port, uint32_t val) { wr32(c, ehci_port_reg(port), val); }
static void ehci_port_clear_changes(struct ehci_controller *c, uint32_t port) {
    uint32_t ps = ehci_port_read(c, port);
    ehci_port_write(c, port, (ps & EHCI_PORT_RW_MASK) | EHCI_PORT_CHANGE);
}

static uint32_t ehci_port_count(struct usb_hcd *hcd) {
    struct ehci_controller *c = (struct ehci_controller *)hcd->priv;
    return c ? c->ports : 0;
}

static int ehci_port_connected(struct usb_hcd *hcd, uint32_t port) {
    struct ehci_controller *c = (struct ehci_controller *)hcd->priv;
    if (!c || port >= c->ports || !ehci_live(c)) return 0;
    return (ehci_port_read(c, port) & EHCI_PORT_CCS) != 0;
}

static int ehci_port_reset(struct usb_hcd *hcd, uint32_t port, uint32_t *speed) {
    struct ehci_controller *c = (struct ehci_controller *)hcd->priv;
    if (!c || !speed || port >= c->ports || !ehci_live(c)) return -1;
    uint64_t deadline = ehci_deadline(100, 100);
    while (!(ehci_port_read(c, port) & EHCI_PORT_CCS)) {
        if (ehci_deadline_passed(deadline)) return -1;
        ehci_sleep_ms(1);
    }
    ehci_port_clear_changes(c, port);
    uint32_t ps = ehci_port_read(c, port);
    ehci_port_write(c, port, (ps & EHCI_PORT_RW_MASK & ~EHCI_PORT_PE) | EHCI_PORT_RESET);
    ehci_sleep_ms(EHCI_RESET_MS);
    ps = ehci_port_read(c, port);
    ehci_port_write(c, port, ps & EHCI_PORT_RW_MASK & ~EHCI_PORT_RESET);
    ehci_sleep_ms(10);
    deadline = ehci_deadline(100, 100);
    do {
        ps = ehci_port_read(c, port);
        if (!(ps & EHCI_PORT_CCS)) return -1;
        if (ps & EHCI_PORT_PE) { *speed = USB_SPEED_HIGH; ehci_port_clear_changes(c, port); return 0; }
        if (ehci_deadline_passed(deadline)) break;
        ehci_sleep_ms(1);
    } while (1);
    ps = ehci_port_read(c, port);
    ehci_port_write(c, port, (ps & EHCI_PORT_RW_MASK) | EHCI_PORT_OWNER);
    ehci_port_clear_changes(c, port);
    return -1;
}

static int ehci_control(struct usb_hcd *hcd, uint8_t addr, uint8_t speed, uint8_t ep,
                        uint16_t max_packet, const struct usb_setup_packet *setup,
                        void *data, uint16_t len, uint32_t timeout_ms) {
    if (!hcd) return -1;
    return ehci_submit_control_dev((struct ehci_controller *)hcd->priv, NULL, addr, speed, ep, max_packet,
                                   setup, data, len, timeout_ms);
}

static int ehci_bulk(struct usb_hcd *hcd, struct usb_device *dev, uint8_t endpoint,
                     uint16_t max_packet, void *data, uint32_t len, uint32_t *actual,
                     uint32_t timeout_ms) {
    if (!hcd) return -1;
    return ehci_submit_bulk((struct ehci_controller *)hcd->priv, dev, endpoint, max_packet,
                            data, len, actual, timeout_ms);
}

static int ehci_isochronous(struct usb_hcd *hcd, struct usb_device *dev, uint8_t endpoint,
                            uint16_t max_packet, void *data, uint32_t len, uint32_t *actual) {
    if (!hcd) return -1;
    return ehci_submit_isochronous((struct ehci_controller *)hcd->priv, dev, endpoint, max_packet,
                                   data, len, actual);
}

static enum irq_return ehci_irq(void *priv) {
    struct ehci_controller *c = (struct ehci_controller *)priv;
    if (!c) return IRQ_NOT_HANDLED;
    uint32_t st = op32(c, EHCI_USBSTS);
    uint32_t handled = st & (EHCI_STS_USBINT | EHCI_STS_USBERR | EHCI_STS_PCD | EHCI_STS_FLR | EHCI_STS_HSE | EHCI_STS_IAA);
    if (!handled) return IRQ_NOT_HANDLED;
    wr32(c, EHCI_USBSTS, handled);
    if (handled & EHCI_STS_HSE) { c->dead = 1; c->error_count++; }
    if ((handled & EHCI_STS_PCD) && c->hcd_registered) (void)usb_hcd_schedule_scan(&c->hcd, 2);
    return IRQ_HANDLED;
}

static const struct usb_hcd_ops ehci_usb_ops = {
    .port_count = ehci_port_count,
    .port_connected = ehci_port_connected,
    .port_reset = ehci_port_reset,
    .control = ehci_control,
    .bulk = ehci_bulk,
    .isochronous = ehci_isochronous,
    .interrupt_in = ehci_interrupt_once,
    .destroy_pipe = NULL,
};

static int ehci_wait_halted(struct ehci_controller *c, int halted, uint32_t timeout_ms) {
    uint64_t deadline = ehci_deadline(timeout_ms, timeout_ms);
    for (;;) {
        uint32_t st = op32(c, EHCI_USBSTS);
        if (((st & EHCI_STS_HCH) != 0) == halted) return 0;
        if (ehci_deadline_passed(deadline)) return -1;
        ehci_sleep_ms(1);
    }
}

static int ehci_stop(struct ehci_controller *c) {
    uint32_t cmd = op32(c, EHCI_USBCMD);
    cmd &= ~(EHCI_CMD_RS | EHCI_CMD_ASE | EHCI_CMD_PSE);
    wr32(c, EHCI_USBCMD, cmd);
    return ehci_wait_halted(c, 1, 100);
}

static int ehci_reset(struct ehci_controller *c) {
    if (ehci_stop(c) != 0) return -1;
    wr32(c, EHCI_USBCMD, op32(c, EHCI_USBCMD) | EHCI_CMD_HCRESET);
    uint64_t deadline = ehci_deadline(100, 100);
    while (op32(c, EHCI_USBCMD) & EHCI_CMD_HCRESET) {
        if (ehci_deadline_passed(deadline)) return -1;
        ehci_sleep_ms(1);
    }
    return 0;
}

static void ehci_take_ownership(struct ehci_controller *c) {
    uint32_t hcc = cap32(c, EHCI_CAP_HCCPARAMS);
    uint8_t eecp = (uint8_t)((hcc >> 8) & 0xffu);
    uint32_t guard = 0;
    while (eecp >= 0x40 && guard++ < 32) {
        uint32_t cap = pci_read32(c->pdev->segment, c->pdev->bus, c->pdev->slot, c->pdev->func, eecp);
        if ((cap & 0xffu) == 1u) {
            pci_write32(c->pdev->segment, c->pdev->bus, c->pdev->slot, c->pdev->func, eecp + 4u, 1u << 24);
            uint64_t deadline = ehci_deadline(1000, 1000);
            while (!ehci_deadline_passed(deadline)) {
                uint32_t ctl = pci_read32(c->pdev->segment, c->pdev->bus, c->pdev->slot, c->pdev->func, eecp + 4u);
                if (!(ctl & (1u << 16)) && (ctl & (1u << 24))) break;
                ehci_sleep_ms(10);
            }
            return;
        }
        eecp = (uint8_t)((cap >> 8) & 0xffu);
    }
}

static int ehci_init_hw(struct ehci_controller *c) {
    ehci_take_ownership(c);
    uint32_t hcc = cap32(c, EHCI_CAP_HCCPARAMS);
    c->dma_mask = (hcc & 1u) ? DMA_MASK_64 : DMA_MASK_32;
    if (ehci_reset(c) != 0) return -1;
    memset(c->async_head, 0, sizeof(*c->async_head));
    c->async_head->horiz_link = ((uint32_t)c->async_dma.phys) | EHCI_LINK_QH;
    c->async_head->ep_char = qh_ep_char(0, 0, USB_SPEED_HIGH, 64, 0, 1);
    c->async_head->ep_cap = 1u << 30;
    c->async_head->overlay.next = EHCI_QTD_NEXT_TERM;
    c->async_head->overlay.alt_next = EHCI_QTD_NEXT_TERM;
    c->async_head->overlay.token = EHCI_QTD_STS_HALTED;
    for (uint32_t i = 0; i < EHCI_PERIODIC_FRAMES; i++) c->periodic[i] = EHCI_LINK_TERM;

    wr32(c, EHCI_CTRLDSSEG, (uint32_t)(c->dma_mask == DMA_MASK_64 ? (c->periodic_dma.phys >> 32) : 0));
    wr32(c, EHCI_PERIODIC, (uint32_t)c->periodic_dma.phys);
    wr32(c, EHCI_ASYNC, (uint32_t)c->async_dma.phys);
    wr32(c, EHCI_USBINTR, EHCI_STS_USBINT | EHCI_STS_USBERR | EHCI_STS_PCD | EHCI_STS_HSE);
    wr32(c, EHCI_USBSTS, UINT32_C(0x00003f));
    uint32_t cmd = EHCI_CMD_RS | EHCI_CMD_ASE | EHCI_CMD_PSE | EHCI_CMD_ASPMC_3 | EHCI_CMD_ITC_1;
    if (hcc & (1u << 11)) cmd |= EHCI_CMD_ASPE;
    wr32(c, EHCI_USBCMD, cmd);
    if (ehci_wait_halted(c, 0, 100) != 0) return -1;
    (void)wait_state(c, EHCI_STS_ASS, 1, 100);
    (void)wait_state(c, EHCI_STS_PSS, 1, 100);
    wr32(c, EHCI_CONFIGFLAG, 1);
    ehci_sleep_ms(20);

    uint32_t hcs = cap32(c, EHCI_CAP_HCSPARAMS);
    c->ports = hcs & 0x0fu;
    if (c->ports == 0 || c->ports > 15) return -1;
    for (uint32_t i = 0; i < c->ports; i++) {
        uint32_t ps = ehci_port_read(c, i);
        if (!(ps & EHCI_PORT_POWER)) ehci_port_write(c, i, (ps & EHCI_PORT_RW_MASK) | EHCI_PORT_POWER);
        ehci_port_clear_changes(c, i);
    }
    return 0;
}

static void ehci_stop_hw(struct ehci_controller *c) {
    if (!c || !c->cap_regs) return;
    c->dead = 1;
    wr32(c, EHCI_USBINTR, 0);
    (void)ehci_stop(c);
    wr32(c, EHCI_CONFIGFLAG, 0);
}

static void ehci_unlink_controller(struct ehci_controller *c) {
    struct ehci_controller **pp = &controllers;
    while (*pp) {
        if (*pp == c) { *pp = c->next; c->next = NULL; return; }
        pp = &(*pp)->next;
    }
}

static void ehci_free_controller(struct ehci_controller *c) {
    if (!c) return;
    if (c->irq_registered) free_irq(c->irq, ehci_irq);
    if (c->periodic_dma.virt) dma_free_coherent(&c->periodic_dma);
    if (c->async_dma.virt) dma_free_coherent(&c->async_dma);
    if (c->cap_regs) vmm_mmio_unmap((void *)c->cap_regs, c->regs_size);
    kfree(c);
}

static int ehci_probe(struct device *dev) {
    struct pci_device *pdev = (struct pci_device *)dev->bus_data;
    if (!pdev || (pdev->bars[0].type != PCI_BAR_TYPE_MMIO32 && pdev->bars[0].type != PCI_BAR_TYPE_MMIO64) || pdev->bars[0].base == 0) return -1;
    struct ehci_controller *c = (struct ehci_controller *)kzalloc(sizeof(*c));
    if (!c) return -1;
    c->pdev = pdev;
    c->dev = dev;
    c->regs_size = pdev->bars[0].size ? pdev->bars[0].size : 4096u;
    if (c->regs_size < 0x100u) c->regs_size = 0x100u;
    c->cap_regs = (volatile uint8_t *)vmm_mmio_map(pdev->bars[0].base, c->regs_size);
    if (!c->cap_regs) goto fail;
    c->op_offset = cap8(c, EHCI_CAP_CAPLENGTH);
    if (c->op_offset < 0x10u || c->op_offset + 0x80u > c->regs_size) goto fail;
    c->op_regs = (volatile uint32_t *)(c->cap_regs + c->op_offset);
    c->irq = pci_read8(pdev->segment, pdev->bus, pdev->slot, pdev->func, 0x3c);
    spinlock_init(&c->lock);
    c->dma_mask = (cap32(c, EHCI_CAP_HCCPARAMS) & 1u) ? DMA_MASK_64 : DMA_MASK_32;
    if (dma_alloc_coherent(sizeof(struct ehci_qh), 32, c->dma_mask, &c->async_dma) != 0) goto fail;
    if (dma_alloc_coherent(EHCI_PERIODIC_FRAMES * sizeof(uint32_t), 4096, c->dma_mask, &c->periodic_dma) != 0) goto fail;
    if (!dma_ok(&c->async_dma, c->dma_mask) || !dma_ok(&c->periodic_dma, c->dma_mask)) goto fail;
    c->async_head = (struct ehci_qh *)c->async_dma.virt;
    c->periodic = (uint32_t *)c->periodic_dma.virt;

    pci_enable_bus_mastering(pdev->segment, pdev->bus, pdev->slot, pdev->func);
    if (ehci_init_hw(c) != 0) goto fail_stop;
    if (c->irq && c->irq != 0xff) {
        if (request_irq(c->irq, ehci_irq, IRQ_AFFINITY_ALL, c) == 0) c->irq_registered = true;
        else klog(LOG_WARN, "EHCI: failed to register irq=%u; continuing with polling\n", c->irq);
    }

    c->hcd.name = "ehci";
    c->hcd.priv = c;
    c->hcd.ops = &ehci_usb_ops;
    if (usb_hcd_register(&c->hcd) != 0) goto fail_stop;
    c->hcd_registered = true;
    c->next = controllers;
    controllers = c;
    dev->driver_data = c;
    klog(LOG_INFO, "EHCI: controller mmio=%016llx irq=%u ports=%u version=%04x dma=%u\n",
         (unsigned long long)pdev->bars[0].base, c->irq, c->ports, cap16(c, EHCI_CAP_HCIVERSION), c->dma_mask == DMA_MASK_64 ? 64u : 32u);
    return 0;
fail_stop:
    if (c->irq_registered) { free_irq(c->irq, ehci_irq); c->irq_registered = false; }
    ehci_stop_hw(c);
fail:
    ehci_free_controller(c);
    return -1;
}

static void ehci_remove(struct device *dev) {
    struct ehci_controller *c = (struct ehci_controller *)dev->driver_data;
    if (!c) return;
    if (c->hcd_registered) { usb_hcd_unregister(&c->hcd); c->hcd_registered = false; }
    ehci_unlink_controller(c);
    ehci_stop_hw(c);
    while (__sync_lock_test_and_set(&c->transfer_busy, 1u)) ehci_sleep_ms(1);
    ehci_leave_transfer(c);
    ehci_free_controller(c);
    dev->driver_data = NULL;
}

static const struct pci_device_id ehci_ids[] = {
    {.vendor_id = PCI_ANY_ID, .device_id = PCI_ANY_ID, .class_code = PCI_CLASS_SERIAL,
     .subclass = PCI_SUBCLASS_USB, .prog_if = PCI_PROGIF_EHCI},
    {0, 0, 0, 0, 0},
};

static struct driver ehci_driver = {
    .name = "ehci",
    .bus = NULL,
    .module = NULL,
    .id_table = ehci_ids,
    .probe = ehci_probe,
    .remove = ehci_remove,
    .next = NULL,
};

static int ehci_init(void) {
    ehci_driver.bus = find_bus("pci");
    if (!ehci_driver.bus) return -1;
    driver_register(&ehci_driver);
    return 0;
}

MODULE_INFO("ehci", ehci_init, 0, NULL, "pci_bus", "usb_core");
