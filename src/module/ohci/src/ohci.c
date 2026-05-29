#include "usb_core.h"

#include "mod/dma.h"
#include "mod/device.h"
#include "mod/heap.h"
#include "mod/interrupt.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "mod/scheduler.h"
#include "mod/workqueue.h"
#include "mod/vmm.h"
#include "pci_bus.h"
#include "smp/lock.h"
#include "string.h"

#define PCI_CLASS_SERIAL 0x0cu
#define PCI_SUBCLASS_USB 0x03u
#define PCI_PROGIF_OHCI  0x10u

#define OHCI_REG_REVISION        0x00u
#define OHCI_REG_CONTROL         0x04u
#define OHCI_REG_CMD_STATUS      0x08u
#define OHCI_REG_INTR_STATUS     0x0cu
#define OHCI_REG_INTR_ENABLE     0x10u
#define OHCI_REG_INTR_DISABLE    0x14u
#define OHCI_REG_HCCA            0x18u
#define OHCI_REG_PERIOD_CURRENT  0x1cu
#define OHCI_REG_CONTROL_HEAD    0x20u
#define OHCI_REG_CONTROL_CURRENT 0x24u
#define OHCI_REG_BULK_HEAD       0x28u
#define OHCI_REG_BULK_CURRENT    0x2cu
#define OHCI_REG_DONE_HEAD       0x30u
#define OHCI_REG_FM_INTERVAL     0x34u
#define OHCI_REG_FM_REMAINING    0x38u
#define OHCI_REG_FM_NUMBER       0x3cu
#define OHCI_REG_PERIODIC_START  0x40u
#define OHCI_REG_LS_THRESHOLD    0x44u
#define OHCI_REG_RH_DESC_A       0x48u
#define OHCI_REG_RH_DESC_B       0x4cu
#define OHCI_REG_RH_STATUS       0x50u
#define OHCI_REG_RH_PORT_STATUS  0x54u

#define OHCI_CTL_CBSR_MASK 0x00000003u
#define OHCI_CTL_PLE       0x00000004u
#define OHCI_CTL_IE        0x00000008u
#define OHCI_CTL_CLE       0x00000010u
#define OHCI_CTL_BLE       0x00000020u
#define OHCI_CTL_HCFS_MASK 0x000000c0u
#define OHCI_CTL_USB_RESET 0x00000000u
#define OHCI_CTL_USB_RESUME 0x00000040u
#define OHCI_CTL_USB_OPER  0x00000080u
#define OHCI_CTL_USB_SUSP  0x000000c0u
#define OHCI_CTL_RWC       0x00000200u
#define OHCI_CTL_RWE       0x00000400u

#define OHCI_CMD_HCR       0x00000001u
#define OHCI_CMD_CLF       0x00000002u
#define OHCI_CMD_BLF       0x00000004u
#define OHCI_CMD_OCR       0x00000008u

#define OHCI_INT_SO        0x00000001u
#define OHCI_INT_WDH       0x00000002u
#define OHCI_INT_SF        0x00000004u
#define OHCI_INT_RD        0x00000008u
#define OHCI_INT_UE        0x00000010u
#define OHCI_INT_FNO       0x00000020u
#define OHCI_INT_RHSC      0x00000040u
#define OHCI_INT_OC        0x40000000u
#define OHCI_INT_MIE       0x80000000u

#define OHCI_RHDA_NDP_MASK 0x000000ffu
#define OHCI_RHDA_PSM      0x00000100u
#define OHCI_RHDA_NPS      0x00000200u
#define OHCI_RHDA_DT       0x00000400u
#define OHCI_RHDA_OCPM     0x00000800u
#define OHCI_RHDA_NOCP     0x00001000u

#define OHCI_RHS_LPS       0x00000001u
#define OHCI_RHS_OCI       0x00000002u
#define OHCI_RHS_DRWE      0x00008000u
#define OHCI_RHS_LPSC      0x00010000u
#define OHCI_RHS_OCIC      0x00020000u
#define OHCI_RHS_CRWE      0x80000000u

#define OHCI_PORT_CCS      0x00000001u
#define OHCI_PORT_PES      0x00000002u
#define OHCI_PORT_PSS      0x00000004u
#define OHCI_PORT_POCI     0x00000008u
#define OHCI_PORT_PRS      0x00000010u
#define OHCI_PORT_PPS      0x00000100u
#define OHCI_PORT_LSDA     0x00000200u
#define OHCI_PORT_CSC      0x00010000u
#define OHCI_PORT_PESC     0x00020000u
#define OHCI_PORT_PSSC     0x00040000u
#define OHCI_PORT_OCIC     0x00080000u
#define OHCI_PORT_PRSC     0x00100000u
#define OHCI_PORT_CHANGE   (OHCI_PORT_CSC | OHCI_PORT_PESC | OHCI_PORT_PSSC | OHCI_PORT_OCIC | OHCI_PORT_PRSC)

#define OHCI_ED_SKIP       0x00004000u
#define OHCI_ED_FORMAT_ISO 0x00008000u
#define OHCI_ED_HEAD_H     0x00000001u
#define OHCI_ED_HEAD_C     0x00000002u
#define OHCI_ED_HEAD_MASK  0xfffffff0u

#define OHCI_TD_DP_SETUP   0x00000000u
#define OHCI_TD_DP_OUT     0x00080000u
#define OHCI_TD_DP_IN      0x00100000u
#define OHCI_TD_DI_NO      0x00e00000u
#define OHCI_TD_T_DATA0    0x02000000u
#define OHCI_TD_T_DATA1    0x03000000u
#define OHCI_TD_T_FROM_ED  0x00000000u
#define OHCI_TD_CC_SHIFT   28u
#define OHCI_TD_CC_NOACC   0xfu
#define OHCI_TD_CC_OK      0x0u
#define OHCI_TD_CC_STALL   0x4u
#define OHCI_TD_CC_NAK     0xdu

#define OHCI_ISO_CC_NOACC  0xfu

#define OHCI_HCCA_SIZE 256u
#define OHCI_MAX_TDS 256u
#define OHCI_MAX_DATA 16384u
#define OHCI_CONTROL_TIMEOUT_MS 1000u
#define OHCI_BULK_TIMEOUT_MS 1000u
#define OHCI_INTR_POLL_MS 2u
#define OHCI_RESET_MS 50u
#define OHCI_FRAME_SETTLE_MS 2u

struct ohci_hcca {
    uint32_t intr_table[32];
    uint16_t frame_number;
    uint16_t pad1;
    uint32_t done_head;
    uint8_t reserved[116];
} __attribute__((packed, aligned(256)));

struct ohci_ed {
    uint32_t control;
    uint32_t tailp;
    uint32_t headp;
    uint32_t nexted;
} __attribute__((packed, aligned(16)));

struct ohci_td {
    uint32_t flags;
    uint32_t cbp;
    uint32_t nexttd;
    uint32_t be;
} __attribute__((packed, aligned(16)));

struct ohci_iso_td {
    uint32_t flags;
    uint32_t bp0;
    uint32_t nexttd;
    uint32_t be;
    uint16_t offsets[8];
} __attribute__((packed, aligned(32)));

struct ohci_transfer {
    dma_allocation_t ed_dma;
    dma_allocation_t td_dma;
    dma_allocation_t tail_dma;
    dma_allocation_t setup_dma;
    dma_allocation_t data_dma;
    struct ohci_ed *ed;
    struct ohci_td *td;
    struct ohci_td *tail;
    void *setup;
    void *data;
    uint32_t td_count;
};

struct ohci_controller {
    struct pci_device *pdev;
    struct device *dev;
    volatile uint32_t *regs;
    uint64_t regs_size;
    uint8_t irq;
    bool irq_registered;
    spinlock_t lock;
    dma_allocation_t hcca_dma;
    struct ohci_hcca *hcca;
    struct usb_hcd hcd;
    struct kernel_work init_work;
    volatile uint32_t transfer_busy;
    volatile uint32_t dead;
    volatile uint32_t removing;
    bool hcd_registered;
    uint32_t ports;
    uint8_t bulk_toggle[128][16][2];
    uint64_t transfer_count;
    uint64_t timeout_count;
    uint64_t error_count;
    struct ohci_controller *next;
};

static struct ohci_controller *controllers;

static inline uint32_t rd32(struct ohci_controller *c, uint32_t off) { return c->regs[off >> 2]; }
static inline void wr32(struct ohci_controller *c, uint32_t off, uint32_t val) { c->regs[off >> 2] = val; (void)c->regs[off >> 2]; }

static void ohci_sleep_ms(uint32_t ms) {
    if (ms == 0) return;
    if (kernel_can_sleep()) {
        (void)kernel_sleep_ticks(ms);
        return;
    }
    for (uint32_t i = 0; i < ms * 50000u; i++) asm volatile("pause");
}

static uint64_t ohci_deadline(uint32_t timeout_ms, uint32_t fallback_ms) {
    uint32_t use = timeout_ms ? timeout_ms : fallback_ms;
    if (use == 0) use = 1;
    return kernel_monotonic_ticks() + use;
}

static int ohci_deadline_passed(uint64_t deadline) {
    return (int64_t)(kernel_monotonic_ticks() - deadline) >= 0;
}

static int dma32_ok(const dma_allocation_t *a) {
    if (!a->virt) return 1;
    return a->phys <= UINT32_C(0xffffffff) && a->phys + a->size - 1u <= UINT32_C(0xffffffff);
}

static void ohci_enter_transfer(struct ohci_controller *c) {
    while (__sync_lock_test_and_set(&c->transfer_busy, 1u)) {
        if (kernel_can_sleep()) (void)kernel_sleep_ticks(1);
        else asm volatile("pause");
    }
    __sync_synchronize();
}

static void ohci_leave_transfer(struct ohci_controller *c) {
    __sync_synchronize();
    __sync_lock_release(&c->transfer_busy);
}

static int ohci_live(struct ohci_controller *c) {
    if (!c || c->dead) return 0;
    uint32_t st = rd32(c, OHCI_REG_INTR_STATUS);
    if (st & OHCI_INT_UE) {
        c->dead = 1;
        c->error_count++;
        klog(LOG_ERR, "OHCI: unrecoverable error irq=%u status=%08x\n", c->irq, st);
        return 0;
    }
    return 1;
}

static uint32_t td_cc(const struct ohci_td *td) { return (td->flags >> OHCI_TD_CC_SHIFT) & 0xfu; }

static int td_error_result(const struct ohci_td *td) {
    uint32_t cc = td_cc(td);
    if (cc == OHCI_TD_CC_NOACC) return 1;
    if (cc == OHCI_TD_CC_OK || cc == OHCI_TD_CC_NAK) return 0;
    if (cc == OHCI_TD_CC_STALL) return -4;
    return -3;
}

static uint32_t td_actual_len(const struct ohci_td *td, uint32_t requested) {
    if (requested == 0) return 0;
    if (td->cbp == 0) return requested;
    if (td->cbp > td->be) return requested;
    uint32_t left = td->be - td->cbp + 1u;
    return left >= requested ? 0 : requested - left;
}

static void free_transfer(struct ohci_transfer *t) {
    if (!t) return;
    dma_free_coherent(&t->data_dma);
    dma_free_coherent(&t->setup_dma);
    dma_free_coherent(&t->tail_dma);
    dma_free_coherent(&t->td_dma);
    dma_free_coherent(&t->ed_dma);
    memset(t, 0, sizeof(*t));
}

static int alloc_transfer(struct ohci_transfer *t, uint32_t td_count, uint32_t data_len, int need_setup) {
    memset(t, 0, sizeof(*t));
    if (td_count == 0 || td_count > OHCI_MAX_TDS || data_len > OHCI_MAX_DATA) return -1;
    if (dma_alloc_coherent(sizeof(struct ohci_ed), 16, DMA_MASK_32, &t->ed_dma) != 0) return -1;
    if (dma_alloc_coherent(sizeof(struct ohci_td) * td_count, 16, DMA_MASK_32, &t->td_dma) != 0) goto fail;
    if (dma_alloc_coherent(sizeof(struct ohci_td), 16, DMA_MASK_32, &t->tail_dma) != 0) goto fail;
    if (need_setup && dma_alloc_coherent(sizeof(struct usb_setup_packet), 8, DMA_MASK_32, &t->setup_dma) != 0) goto fail;
    if (data_len && dma_alloc_coherent(data_len, 16, DMA_MASK_32, &t->data_dma) != 0) goto fail;
    if (!dma32_ok(&t->ed_dma) || !dma32_ok(&t->td_dma) || !dma32_ok(&t->tail_dma) ||
        !dma32_ok(&t->setup_dma) || !dma32_ok(&t->data_dma)) goto fail;
    t->ed = (struct ohci_ed *)t->ed_dma.virt;
    t->td = (struct ohci_td *)t->td_dma.virt;
    t->tail = (struct ohci_td *)t->tail_dma.virt;
    t->setup = t->setup_dma.virt;
    t->data = t->data_dma.virt;
    t->td_count = td_count;
    memset(t->ed, 0, sizeof(*t->ed));
    memset(t->td, 0, sizeof(struct ohci_td) * td_count);
    memset(t->tail, 0, sizeof(*t->tail));
    return 0;
fail:
    free_transfer(t);
    return -1;
}

static uint32_t ed_control(uint8_t addr, uint8_t endpoint, uint8_t speed, uint16_t max_packet, uint32_t dir) {
    uint32_t ep = endpoint & 0x0fu;
    return ((uint32_t)(addr & 0x7fu)) | (ep << 7) | dir | (speed == USB_SPEED_LOW ? (1u << 13) : 0) |
           ((uint32_t)max_packet << 16);
}

static uint32_t td_flags(uint32_t dir, uint32_t toggle) {
    return dir | OHCI_TD_DI_NO | toggle | (OHCI_TD_CC_NOACC << OHCI_TD_CC_SHIFT);
}

static int valid_packet(uint8_t speed, uint16_t max_packet) {
    if (max_packet == 0 || max_packet > 64) return 0;
    if (speed == USB_SPEED_LOW && max_packet > 8) return 0;
    return 1;
}

static void link_td_chain(struct ohci_transfer *t) {
    for (uint32_t i = 0; i < t->td_count; i++) {
        uint64_t next = (i + 1u < t->td_count) ? t->td_dma.phys + sizeof(struct ohci_td) * (i + 1u) : t->tail_dma.phys;
        t->td[i].nexttd = (uint32_t)next;
    }
    t->ed->headp = (uint32_t)t->td_dma.phys;
    t->ed->tailp = (uint32_t)t->tail_dma.phys;
    t->ed->nexted = 0;
}

static void cancel_transfer(struct ohci_transfer *t) {
    if (!t || !t->ed) return;
    t->ed->control |= OHCI_ED_SKIP;
}

static int wait_transfer(struct ohci_controller *c, struct ohci_transfer *t, uint32_t timeout_ms, uint32_t *completed) {
    uint64_t deadline = ohci_deadline(timeout_ms, timeout_ms);
    if (completed) *completed = 0;
    for (;;) {
        if (!ohci_live(c)) return -5;
        uint32_t done = 0;
        int active = 0;
        for (uint32_t i = 0; i < t->td_count; i++) {
            int r = td_error_result(&t->td[i]);
            if (r == 1) {
                active = 1;
                continue;
            }
            if (r < 0) {
                c->error_count++;
                if (completed) *completed = done;
                return r;
            }
            done++;
        }
        if (!active) {
            if (completed) *completed = done;
            return 0;
        }
        if (ohci_deadline_passed(deadline)) {
            c->timeout_count++;
            if (completed) *completed = done;
            return -2;
        }
        if (kernel_can_sleep()) (void)kernel_sleep_ticks(1);
        else asm volatile("pause");
    }
}

static int run_transfer(struct ohci_controller *c, struct ohci_transfer *t, int bulk, uint32_t timeout_ms, uint32_t *completed) {
    if (!ohci_live(c)) return -5;
    ohci_enter_transfer(c);
    if (!ohci_live(c)) {
        ohci_leave_transfer(c);
        return -5;
    }
    link_td_chain(t);
    uint64_t flags;
    spinlock_acquire(&c->lock, &flags);
    if (bulk) {
        wr32(c, OHCI_REG_BULK_HEAD, (uint32_t)t->ed_dma.phys);
        wr32(c, OHCI_REG_BULK_CURRENT, 0);
        wr32(c, OHCI_REG_CMD_STATUS, OHCI_CMD_BLF);
    } else {
        wr32(c, OHCI_REG_CONTROL_HEAD, (uint32_t)t->ed_dma.phys);
        wr32(c, OHCI_REG_CONTROL_CURRENT, 0);
        wr32(c, OHCI_REG_CMD_STATUS, OHCI_CMD_CLF);
    }
    spinlock_release(&c->lock, flags);

    int r = wait_transfer(c, t, timeout_ms, completed);
    if (r == -2) cancel_transfer(t);

    spinlock_acquire(&c->lock, &flags);
    if (bulk) {
        if (rd32(c, OHCI_REG_BULK_HEAD) == (uint32_t)t->ed_dma.phys) wr32(c, OHCI_REG_BULK_HEAD, 0);
        if (rd32(c, OHCI_REG_BULK_CURRENT) == (uint32_t)t->ed_dma.phys) wr32(c, OHCI_REG_BULK_CURRENT, 0);
    } else {
        if (rd32(c, OHCI_REG_CONTROL_HEAD) == (uint32_t)t->ed_dma.phys) wr32(c, OHCI_REG_CONTROL_HEAD, 0);
        if (rd32(c, OHCI_REG_CONTROL_CURRENT) == (uint32_t)t->ed_dma.phys) wr32(c, OHCI_REG_CONTROL_CURRENT, 0);
    }
    spinlock_release(&c->lock, flags);
    ohci_sleep_ms(OHCI_FRAME_SETTLE_MS);
    c->transfer_count++;
    ohci_leave_transfer(c);
    return r;
}

static int ohci_submit_control(struct ohci_controller *c, uint8_t addr, uint8_t speed, uint8_t ep,
                               uint16_t max_packet, const struct usb_setup_packet *setup,
                               void *data, uint16_t len, uint32_t timeout_ms) {
    if (!c || !setup || ep != 0 || !valid_packet(speed, max_packet) || len > OHCI_MAX_DATA) return -1;
    if (len && !data) return -1;
    uint32_t packets = len ? (uint32_t)((len + max_packet - 1u) / max_packet) : 0;
    uint32_t td_count = 2u + packets;
    struct ohci_transfer t;
    if (alloc_transfer(&t, td_count, len, 1) != 0) return -1;
    memcpy(t.setup, setup, sizeof(*setup));
    if (len && !(setup->bmRequestType & USB_DIR_IN)) memcpy(t.data, data, len);
    t.ed->control = ed_control(addr, 0, speed, max_packet, 0);

    uint32_t idx = 0;
    t.td[idx].flags = td_flags(OHCI_TD_DP_SETUP, OHCI_TD_T_DATA0);
    t.td[idx].cbp = (uint32_t)t.setup_dma.phys;
    t.td[idx].be = (uint32_t)(t.setup_dma.phys + sizeof(*setup) - 1u);
    idx++;

    uint32_t done = 0;
    uint32_t toggle = OHCI_TD_T_DATA1;
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > max_packet) chunk = max_packet;
        t.td[idx].flags = td_flags((setup->bmRequestType & USB_DIR_IN) ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT, toggle);
        t.td[idx].cbp = (uint32_t)(t.data_dma.phys + done);
        t.td[idx].be = (uint32_t)(t.data_dma.phys + done + chunk - 1u);
        done += chunk;
        toggle = (toggle == OHCI_TD_T_DATA1) ? OHCI_TD_T_DATA0 : OHCI_TD_T_DATA1;
        idx++;
    }

    t.td[idx].flags = td_flags((setup->bmRequestType & USB_DIR_IN) ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN, OHCI_TD_T_DATA1);
    t.td[idx].cbp = 0;
    t.td[idx].be = 0;

    uint32_t completed = 0;
    int r = run_transfer(c, &t, 0, timeout_ms ? timeout_ms : OHCI_CONTROL_TIMEOUT_MS, &completed);
    if (r == 0 && len && (setup->bmRequestType & USB_DIR_IN)) {
        uint32_t got = 0;
        for (uint32_t i = 1; i < 1u + packets; i++) {
            uint32_t requested = max_packet;
            if (i == packets && (len % max_packet)) requested = len % max_packet;
            got += td_actual_len(&t.td[i], requested);
        }
        if (got > len) got = len;
        if (got) memcpy(data, t.data, got);
    }
    if (r == 0 && completed != td_count) r = -2;
    free_transfer(&t);
    return r;
}

static uint8_t *bulk_toggle_slot(struct ohci_controller *c, struct usb_device *dev, uint8_t endpoint) {
    uint8_t addr = dev->address & 0x7fu;
    uint8_t ep = endpoint & 0x0fu;
    uint8_t in = (endpoint & USB_DIR_IN) ? 1u : 0u;
    return &c->bulk_toggle[addr][ep][in];
}

static int ohci_submit_bulk(struct ohci_controller *c, struct usb_device *dev, uint8_t endpoint,
                            uint16_t max_packet, void *data, uint32_t len, uint32_t *actual,
                            uint32_t timeout_ms) {
    if (actual) *actual = 0;
    if (!c || !dev || dev->disconnected || dev->speed == USB_SPEED_LOW || max_packet == 0 || max_packet > 64 || len > OHCI_MAX_DATA) return -1;
    if (len && !data) return -1;
    uint32_t packets = len ? (len + max_packet - 1u) / max_packet : 1u;
    if (packets == 0 || packets > OHCI_MAX_TDS) return -1;
    struct ohci_transfer t;
    if (alloc_transfer(&t, packets, len ? len : 1u, 0) != 0) return -1;
    int in = (endpoint & USB_DIR_IN) != 0;
    if (!in && len) memcpy(t.data, data, len);
    t.ed->control = ed_control(dev->address, endpoint, dev->speed, max_packet, in ? (2u << 11) : (1u << 11));

    uint8_t *slot = bulk_toggle_slot(c, dev, endpoint);
    uint32_t toggle = *slot ? OHCI_TD_T_DATA1 : OHCI_TD_T_DATA0;
    uint32_t done = 0;
    for (uint32_t i = 0; i < packets; i++) {
        uint32_t chunk = len - done;
        if (chunk > max_packet) chunk = max_packet;
        t.td[i].flags = td_flags(in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT, toggle);
        if (chunk) {
            t.td[i].cbp = (uint32_t)(t.data_dma.phys + done);
            t.td[i].be = (uint32_t)(t.data_dma.phys + done + chunk - 1u);
        }
        done += chunk;
        toggle = (toggle == OHCI_TD_T_DATA1) ? OHCI_TD_T_DATA0 : OHCI_TD_T_DATA1;
    }

    uint32_t completed = 0;
    int r = run_transfer(c, &t, 1, timeout_ms ? timeout_ms : OHCI_BULK_TIMEOUT_MS, &completed);
    uint32_t got = 0;
    if (r == 0) {
        for (uint32_t i = 0, offset = 0; i < packets; i++) {
            uint32_t requested = len - offset;
            if (requested > max_packet) requested = max_packet;
            uint32_t chunk = td_actual_len(&t.td[i], requested);
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

static int ohci_interrupt_once(struct usb_interrupt_pipe *pipe) {
    if (!pipe || !pipe->hcd || !pipe->dev || !pipe->buffer) return -1;
    struct ohci_controller *c = (struct ohci_controller *)pipe->hcd->priv;
    struct usb_device *dev = pipe->dev;
    if (!c || dev->disconnected || pipe->max_packet == 0 || pipe->buffer_len == 0 ||
        pipe->buffer_len > pipe->max_packet || pipe->buffer_len > OHCI_MAX_DATA) return -1;
    struct ohci_transfer t;
    if (alloc_transfer(&t, 1, pipe->buffer_len, 0) != 0) return -1;
    t.ed->control = ed_control(dev->address, pipe->endpoint, dev->speed, pipe->max_packet, (2u << 11));
    t.td[0].flags = td_flags(OHCI_TD_DP_IN, pipe->data_toggle ? OHCI_TD_T_DATA1 : OHCI_TD_T_DATA0);
    t.td[0].cbp = (uint32_t)t.data_dma.phys;
    t.td[0].be = (uint32_t)(t.data_dma.phys + pipe->buffer_len - 1u);

    uint32_t completed = 0;
    int r = run_transfer(c, &t, 0, OHCI_INTR_POLL_MS, &completed);
    if (r == 0) {
        uint32_t got = td_actual_len(&t.td[0], pipe->buffer_len);
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

static int ohci_submit_isochronous(struct ohci_controller *c, struct usb_device *dev, uint8_t endpoint,
                                   uint16_t max_packet, void *data, uint32_t len, uint32_t *actual) {
    (void)endpoint;
    if (actual) *actual = 0;
    if (!c || !dev || dev->disconnected || dev->speed == USB_SPEED_LOW || max_packet == 0 || max_packet > 1023 || len > OHCI_MAX_DATA) return -1;
    if (len && !data) return -1;
    return -1;
}

static uint32_t ohci_port_count(struct usb_hcd *hcd) {
    struct ohci_controller *c = (struct ohci_controller *)hcd->priv;
    return c ? c->ports : 0;
}

static uint32_t ohci_port_read(struct ohci_controller *c, uint32_t port) {
    return rd32(c, OHCI_REG_RH_PORT_STATUS + port * 4u);
}

static void ohci_port_write(struct ohci_controller *c, uint32_t port, uint32_t val) {
    wr32(c, OHCI_REG_RH_PORT_STATUS + port * 4u, val);
}

static void ohci_port_clear_changes(struct ohci_controller *c, uint32_t port) {
    ohci_port_write(c, port, OHCI_PORT_CHANGE);
}

static int ohci_port_connected(struct usb_hcd *hcd, uint32_t port) {
    struct ohci_controller *c = (struct ohci_controller *)hcd->priv;
    if (!c || port >= c->ports || !ohci_live(c)) return 0;
    return (ohci_port_read(c, port) & OHCI_PORT_CCS) != 0;
}

static int ohci_port_reset(struct usb_hcd *hcd, uint32_t port, uint32_t *speed) {
    struct ohci_controller *c = (struct ohci_controller *)hcd->priv;
    if (!c || !speed || port >= c->ports || !ohci_live(c)) return -1;
    uint64_t deadline = ohci_deadline(100, 100);
    while (!(ohci_port_read(c, port) & OHCI_PORT_CCS)) {
        if (ohci_deadline_passed(deadline)) return -1;
        ohci_sleep_ms(1);
    }
    ohci_port_clear_changes(c, port);
    ohci_port_write(c, port, OHCI_PORT_PRS);
    ohci_sleep_ms(OHCI_RESET_MS);
    deadline = ohci_deadline(100, 100);
    while (!(ohci_port_read(c, port) & OHCI_PORT_PRSC)) {
        if (ohci_deadline_passed(deadline)) return -1;
        ohci_sleep_ms(1);
    }
    ohci_port_clear_changes(c, port);
    uint32_t ps = ohci_port_read(c, port);
    if (!(ps & OHCI_PORT_CCS)) return -1;
    ohci_port_write(c, port, OHCI_PORT_PES);
    ohci_sleep_ms(10);
    ps = ohci_port_read(c, port);
    if (!(ps & OHCI_PORT_PES)) return -1;
    *speed = (ps & OHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
    ohci_port_clear_changes(c, port);
    return 0;
}

static int ohci_control(struct usb_hcd *hcd, uint8_t addr, uint8_t speed, uint8_t ep,
                        uint16_t max_packet, const struct usb_setup_packet *setup,
                        void *data, uint16_t len, uint32_t timeout_ms) {
    if (!hcd) return -1;
    return ohci_submit_control((struct ohci_controller *)hcd->priv, addr, speed, ep, max_packet,
                               setup, data, len, timeout_ms);
}

static int ohci_bulk(struct usb_hcd *hcd, struct usb_device *dev, uint8_t endpoint,
                     uint16_t max_packet, void *data, uint32_t len, uint32_t *actual,
                     uint32_t timeout_ms) {
    if (!hcd) return -1;
    return ohci_submit_bulk((struct ohci_controller *)hcd->priv, dev, endpoint, max_packet,
                            data, len, actual, timeout_ms);
}

static int ohci_isochronous(struct usb_hcd *hcd, struct usb_device *dev, uint8_t endpoint,
                            uint16_t max_packet, void *data, uint32_t len, uint32_t *actual) {
    if (!hcd) return -1;
    return ohci_submit_isochronous((struct ohci_controller *)hcd->priv, dev, endpoint, max_packet,
                                   data, len, actual);
}

static enum irq_return ohci_irq(void *priv) {
    struct ohci_controller *c = (struct ohci_controller *)priv;
    if (!c) return IRQ_NOT_HANDLED;
    uint32_t st = rd32(c, OHCI_REG_INTR_STATUS);
    uint32_t enabled = rd32(c, OHCI_REG_INTR_ENABLE);
    st &= enabled | OHCI_INT_MIE;
    if ((st & (OHCI_INT_WDH | OHCI_INT_RD | OHCI_INT_UE | OHCI_INT_RHSC | OHCI_INT_FNO | OHCI_INT_SO)) == 0) return IRQ_NOT_HANDLED;
    wr32(c, OHCI_REG_INTR_STATUS, st & ~OHCI_INT_MIE);
    if (st & OHCI_INT_UE) {
        c->dead = 1;
        c->error_count++;
    }
    return IRQ_HANDLED;
}

static const struct usb_hcd_ops ohci_usb_ops = {
    .port_count = ohci_port_count,
    .port_connected = ohci_port_connected,
    .port_reset = ohci_port_reset,
    .control = ohci_control,
    .bulk = ohci_bulk,
    .isochronous = ohci_isochronous,
    .interrupt_in = ohci_interrupt_once,
    .destroy_pipe = NULL,
};

static int ohci_wait_reset_clear(struct ohci_controller *c) {
    uint64_t deadline = ohci_deadline(100, 100);
    while (rd32(c, OHCI_REG_CMD_STATUS) & OHCI_CMD_HCR) {
        if (ohci_deadline_passed(deadline)) return -1;
        ohci_sleep_ms(1);
    }
    return 0;
}

static int ohci_take_ownership(struct ohci_controller *c) {
    uint32_t ctl = rd32(c, OHCI_REG_CONTROL);
    if ((ctl & OHCI_CTL_HCFS_MASK) == OHCI_CTL_USB_OPER) return 0;
    if (ctl & OHCI_CTL_RWC) {
        wr32(c, OHCI_REG_CMD_STATUS, OHCI_CMD_OCR);
        uint64_t deadline = ohci_deadline(500, 500);
        while (rd32(c, OHCI_REG_CONTROL) & OHCI_CTL_RWC) {
            if (ohci_deadline_passed(deadline)) return -1;
            ohci_sleep_ms(10);
        }
    }
    return 0;
}

static int ohci_init_hw(struct ohci_controller *c) {
    if (ohci_take_ownership(c) != 0) return -1;
    uint32_t fminterval = rd32(c, OHCI_REG_FM_INTERVAL);
    if (fminterval == 0 || fminterval == UINT32_C(0xffffffff)) fminterval = 0x2edf2edfu;
    wr32(c, OHCI_REG_INTR_DISABLE, UINT32_C(0xffffffff));
    wr32(c, OHCI_REG_CONTROL, (rd32(c, OHCI_REG_CONTROL) & ~OHCI_CTL_HCFS_MASK) | OHCI_CTL_USB_RESET);
    ohci_sleep_ms(10);
    wr32(c, OHCI_REG_CMD_STATUS, OHCI_CMD_HCR);
    if (ohci_wait_reset_clear(c) != 0) return -1;

    wr32(c, OHCI_REG_HCCA, (uint32_t)c->hcca_dma.phys);
    wr32(c, OHCI_REG_CONTROL_HEAD, 0);
    wr32(c, OHCI_REG_CONTROL_CURRENT, 0);
    wr32(c, OHCI_REG_BULK_HEAD, 0);
    wr32(c, OHCI_REG_BULK_CURRENT, 0);
    wr32(c, OHCI_REG_DONE_HEAD, 0);
    wr32(c, OHCI_REG_FM_INTERVAL, fminterval);
    wr32(c, OHCI_REG_PERIODIC_START, ((fminterval & 0x3fffu) * 9u) / 10u);
    wr32(c, OHCI_REG_LS_THRESHOLD, 0x628u);
    wr32(c, OHCI_REG_INTR_STATUS, UINT32_C(0xffffffff));
    wr32(c, OHCI_REG_INTR_ENABLE, OHCI_INT_MIE | OHCI_INT_WDH | OHCI_INT_UE | OHCI_INT_RHSC | OHCI_INT_RD);
    wr32(c, OHCI_REG_CONTROL, OHCI_CTL_CBSR_MASK | OHCI_CTL_CLE | OHCI_CTL_BLE | OHCI_CTL_USB_OPER);
    ohci_sleep_ms(20);

    uint32_t rhda = rd32(c, OHCI_REG_RH_DESC_A);
    c->ports = rhda & OHCI_RHDA_NDP_MASK;
    if (c->ports == 0 || c->ports > 15) return -1;
    if (!(rhda & OHCI_RHDA_NPS)) {
        if (rhda & OHCI_RHDA_PSM) {
            for (uint32_t i = 0; i < c->ports; i++) ohci_port_write(c, i, OHCI_PORT_PPS);
        } else {
            wr32(c, OHCI_REG_RH_STATUS, OHCI_RHS_LPSC);
        }
        ohci_sleep_ms(100);
    }
    for (uint32_t i = 0; i < c->ports; i++) ohci_port_clear_changes(c, i);
    return 0;
}

static void ohci_stop_hw(struct ohci_controller *c) {
    if (!c || !c->regs) return;
    c->dead = 1;
    wr32(c, OHCI_REG_INTR_DISABLE, UINT32_C(0xffffffff));
    wr32(c, OHCI_REG_CONTROL, (rd32(c, OHCI_REG_CONTROL) & ~OHCI_CTL_HCFS_MASK) | OHCI_CTL_USB_SUSP);
    ohci_sleep_ms(2);
    wr32(c, OHCI_REG_INTR_STATUS, UINT32_C(0xffffffff));
}

static void ohci_unlink_controller(struct ohci_controller *c) {
    struct ohci_controller **pp = &controllers;
    while (*pp) {
        if (*pp == c) {
            *pp = c->next;
            c->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void ohci_free_controller(struct ohci_controller *c) {
    if (!c) return;
    if (c->irq_registered) {
        free_irq(c->irq, ohci_irq);
        c->irq_registered = false;
    }
    if (c->hcca_dma.virt) dma_free_coherent(&c->hcca_dma);
    if (c->regs) vmm_mmio_unmap((void *)c->regs, c->regs_size);
    kfree(c);
}

static void ohci_init_work(void *arg) {
    struct ohci_controller *c = (struct ohci_controller *)arg;
    if (!c || c->removing) return;

    if (ohci_init_hw(c) != 0) {
        klog(LOG_ERR, "OHCI: hardware init failed mmio=%016llx\n", (unsigned long long)c->pdev->bars[0].base);
        ohci_stop_hw(c);
        if (c->dev) c->dev->driver_data = NULL;
        ohci_free_controller(c);
        return;
    }

    if (c->removing) return;
    if (c->irq && c->irq != 0xff) {
        if (request_irq(c->irq, ohci_irq, IRQ_AFFINITY_ALL, c) == 0) c->irq_registered = true;
        else klog(LOG_WARN, "OHCI: failed to register irq=%u; continuing with polling\n", c->irq);
    }

    c->hcd.name = "ohci";
    c->hcd.priv = c;
    c->hcd.ops = &ohci_usb_ops;
    if (usb_hcd_register(&c->hcd) != 0) {
        klog(LOG_ERR, "OHCI: HCD registration failed mmio=%016llx\n", (unsigned long long)c->pdev->bars[0].base);
        if (c->irq_registered) {
            free_irq(c->irq, ohci_irq);
            c->irq_registered = false;
        }
        ohci_stop_hw(c);
        if (c->dev) c->dev->driver_data = NULL;
        ohci_free_controller(c);
        return;
    }

    c->hcd_registered = true;
    c->next = controllers;
    controllers = c;
    klog(LOG_INFO, "OHCI: controller mmio=%016llx irq=%u ports=%u\n", (unsigned long long)c->pdev->bars[0].base, c->irq, c->ports);
}

static int ohci_probe(struct device *dev) {
    struct pci_device *pdev = (struct pci_device *)dev->bus_data;
    if (!pdev || (pdev->bars[0].type != PCI_BAR_TYPE_MMIO32 && pdev->bars[0].type != PCI_BAR_TYPE_MMIO64) || pdev->bars[0].base == 0) return -1;
    struct ohci_controller *c = (struct ohci_controller *)kzalloc(sizeof(*c));
    if (!c) return -1;
    c->pdev = pdev;
    c->dev = dev;
    c->regs_size = pdev->bars[0].size ? pdev->bars[0].size : 4096u;
    if (c->regs_size < 0x100u) c->regs_size = 0x100u;
    c->regs = (volatile uint32_t *)vmm_mmio_map(pdev->bars[0].base, c->regs_size);
    if (!c->regs) goto fail;
    c->irq = pci_read8(pdev->segment, pdev->bus, pdev->slot, pdev->func, 0x3c);
    spinlock_init(&c->lock);
    if (dma_alloc_coherent(OHCI_HCCA_SIZE, 256, DMA_MASK_32, &c->hcca_dma) != 0) goto fail;
    if (!dma32_ok(&c->hcca_dma)) goto fail;
    c->hcca = (struct ohci_hcca *)c->hcca_dma.virt;
    memset(c->hcca, 0, sizeof(*c->hcca));

    pci_enable_bus_mastering(pdev->segment, pdev->bus, pdev->slot, pdev->func);
    dev->driver_data = c;
    kernel_work_init(&c->init_work, ohci_init_work, c);
    if (kernel_queue_work(&c->init_work) != 0) {
        dev->driver_data = NULL;
        goto fail;
    }

    klog(LOG_INFO, "OHCI: controller mmio=%016llx irq=%u init queued\n", (unsigned long long)pdev->bars[0].base, c->irq);
    return 0;
fail:
    ohci_free_controller(c);
    return -1;
}

static void ohci_remove(struct device *dev) {
    struct ohci_controller *c = (struct ohci_controller *)dev->driver_data;
    if (!c) return;
    c->removing = 1;
    while (c->init_work.flags & (KERNEL_WORK_PENDING | KERNEL_WORK_RUNNING)) ohci_sleep_ms(1);
    if (c->hcd_registered) {
        usb_hcd_unregister(&c->hcd);
        c->hcd_registered = false;
    }
    ohci_unlink_controller(c);
    ohci_stop_hw(c);
    while (__sync_lock_test_and_set(&c->transfer_busy, 1u)) ohci_sleep_ms(1);
    ohci_leave_transfer(c);
    ohci_free_controller(c);
    dev->driver_data = NULL;
}

static const struct pci_device_id ohci_ids[] = {
    {.vendor_id = PCI_ANY_ID, .device_id = PCI_ANY_ID, .class_code = PCI_CLASS_SERIAL,
     .subclass = PCI_SUBCLASS_USB, .prog_if = PCI_PROGIF_OHCI},
    {0, 0, 0, 0, 0},
};

static struct driver ohci_driver = {
    .name = "ohci",
    .bus = NULL,
    .module = NULL,
    .id_table = ohci_ids,
    .probe = ohci_probe,
    .remove = ohci_remove,
    .next = NULL,
};

static int ohci_init(void) {
    ohci_driver.bus = find_bus("pci");
    if (!ohci_driver.bus) return -1;
    driver_register(&ohci_driver);
    return 0;
}

MODULE_INFO("ohci", ohci_init, 0, NULL, "pci_bus", "usb_core");
