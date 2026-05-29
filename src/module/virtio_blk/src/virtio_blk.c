#include "mod/completion.h"
#include "mod/dma.h"
#include "mod/device.h"
#include "mod/heap.h"
#include "mod/interrupt.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "mod/scheduler.h"
#include "mod/util.h"
#include "mod/vmm.h"
#include "pci_bus.h"
#include "portio.h"
#include "smp/lock.h"
#include "string.h"

#define VIRTIO_VENDOR 0x1AF4
#define VIRTIO_LEGACY_BLK 0x1001
#define VIRTIO_MODERN_BLK 0x1042
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
#define PCI_CAP_VENDOR 0x09

#define VIRTIO_STATUS_ACK 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED 128

#define VIRTIO_F_VERSION_1 32
#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_GEOMETRY 4
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH 9
#define VIRTIO_BLK_F_CONFIG_WCE 11
#define VIRTIO_BLK_F_MQ 12
#define VIRTIO_BLK_F_DISCARD 13
#define VIRTIO_BLK_F_WRITE_ZEROES 14

#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_S_OK 0

#define VQ_NUM 8
#define VQ_LEGACY_MAX 1024
#define VQ_ALIGN 4096
#define VIRTIO_MAX_TRANSFER (128u * 1024u)
#define VIRTIO_NAME_MAX 16
#define VIRTIO_TIMEOUT_SPINS 10000000u
#define VIRTIO_TIMEOUT_TICKS 500u
#define VIRTIO_IRQ_NONE 0xFFu

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

struct common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed));

struct mmio_region {
    uint64_t phys;
    uint64_t size;
    volatile uint8_t *virt;
    uint16_t io;
    bool is_io;
};

struct virtio_disk {
    struct pci_device *pdev;
    bool modern;
    bool irq_registered;
    uint8_t irq;
    char name[VIRTIO_NAME_MAX];
    uint64_t sectors;
    uint32_t sector_size;
    uint32_t notify_multiplier;
    uint16_t notify_off;
    volatile uint8_t irq_seen;
    bool request_busy;
    spinlock_t lock;
    struct kernel_completion completion;

    struct mmio_region common;
    struct mmio_region notify;
    struct mmio_region isr;
    struct mmio_region device;
    uint16_t legacy_io;

    dma_allocation_t queue_dma;
    dma_allocation_t req_dma;
    dma_allocation_t status_dma;
    dma_allocation_t bounce_dma;
    uint8_t *queue;
    uint64_t queue_phys;
    uint32_t queue_size;
    struct vring_desc *desc;
    uint16_t *avail;
    uint16_t *used;
    uint16_t last_used;

    struct virtio_blk_req *req;
    uint8_t *status;
    uint8_t *bounce;
    uint64_t req_phys;
    uint64_t status_phys;
    uint64_t bounce_phys;
};

static struct device_ops virtio_blk_ops;
static uint64_t next_disk;

static uint16_t vring_used_offset(uint16_t n) { return (uint16_t)((16u * n + 6u + VQ_ALIGN - 1u) & ~(VQ_ALIGN - 1u)); }
static uint32_t vring_size(uint16_t n) { return (uint32_t)(vring_used_offset(n) + 6u + 8u * n); }

static void wr8(struct mmio_region *r, uint32_t off, uint8_t v) { if (r->is_io) outb((uint16_t)(r->io + off), v); else r->virt[off] = v; }
static void wr16(struct mmio_region *r, uint32_t off, uint16_t v) { if (r->is_io) outw((uint16_t)(r->io + off), v); else *(volatile uint16_t *)(r->virt + off) = v; }
static void wr32(struct mmio_region *r, uint32_t off, uint32_t v) { if (r->is_io) outl((uint16_t)(r->io + off), v); else *(volatile uint32_t *)(r->virt + off) = v; }
static void wr64(struct mmio_region *r, uint32_t off, uint64_t v) { wr32(r, off, (uint32_t)v); wr32(r, off + 4, (uint32_t)(v >> 32)); }
static uint8_t rd8(struct mmio_region *r, uint32_t off) { return r->is_io ? inb((uint16_t)(r->io + off)) : r->virt[off]; }
static uint16_t rd16(struct mmio_region *r, uint32_t off) { return r->is_io ? inw((uint16_t)(r->io + off)) : *(volatile uint16_t *)(r->virt + off); }
static uint32_t rd32(struct mmio_region *r, uint32_t off) { return r->is_io ? inl((uint16_t)(r->io + off)) : *(volatile uint32_t *)(r->virt + off); }
static uint64_t rd64(struct mmio_region *r, uint32_t off) { return (uint64_t)rd32(r, off) | ((uint64_t)rd32(r, off + 4) << 32); }

static bool map_cap_region(struct pci_device *pdev, uint8_t bar, uint32_t offset, uint32_t length, struct mmio_region *out) {
    if (bar >= 6 || length == 0) return false;
    struct pci_resource *res = &pdev->bars[bar];
    memset(out, 0, sizeof(*out));
    if (res->type == PCI_BAR_TYPE_IO) {
        if (res->base + offset > UINT16_MAX) return false;
        out->is_io = true;
        out->io = (uint16_t)(res->base + offset);
        out->size = length;
        return true;
    }
    if (res->type != PCI_BAR_TYPE_MMIO32 && res->type != PCI_BAR_TYPE_MMIO64) return false;
    out->phys = res->base + offset;
    out->size = length;
    out->virt = (volatile uint8_t *)vmm_mmio_map(out->phys, length);
    return out->virt != NULL;
}

static uint64_t legacy_features(struct virtio_disk *d) {
    outl(d->legacy_io + 0x0E, 0);
    return inl(d->legacy_io + 0x00);
}

static void legacy_status_set(struct virtio_disk *d, uint8_t status) { outb(d->legacy_io + 0x12, status); }
static uint8_t legacy_status_get(struct virtio_disk *d) { return inb(d->legacy_io + 0x12); }

static uint64_t modern_features(struct virtio_disk *d) {
    wr32(&d->common, 0, 0);
    uint64_t lo = rd32(&d->common, 4);
    wr32(&d->common, 0, 1);
    uint64_t hi = rd32(&d->common, 4);
    return lo | (hi << 32);
}

static void modern_write_features(struct virtio_disk *d, uint64_t features) {
    wr32(&d->common, 8, 0);
    wr32(&d->common, 12, (uint32_t)features);
    wr32(&d->common, 8, 1);
    wr32(&d->common, 12, (uint32_t)(features >> 32));
}

static void modern_status_set(struct virtio_disk *d, uint8_t status) { wr8(&d->common, 20, status); }
static uint8_t modern_status_get(struct virtio_disk *d) { return rd8(&d->common, 20); }

static enum irq_return virtio_irq(void *priv) {
    struct virtio_disk *d = (struct virtio_disk *)priv;
    uint8_t isr = d->modern ? rd8(&d->isr, 0) : inb(d->legacy_io + 0x13);
    if ((isr & 1) == 0) return IRQ_NOT_HANDLED;
    d->irq_seen = 1;
    kernel_completion_signal(&d->completion);
    return IRQ_HANDLED;
}

static void free_queue(struct virtio_disk *d) {
    dma_free_coherent(&d->bounce_dma);
    dma_free_coherent(&d->status_dma);
    dma_free_coherent(&d->req_dma);
    dma_free_coherent(&d->queue_dma);
    d->queue = NULL;
    d->queue_phys = 0;
    d->desc = NULL;
    d->avail = NULL;
    d->used = NULL;
    d->req = NULL;
    d->status = NULL;
    d->bounce = NULL;
}

static bool alloc_queue(struct virtio_disk *d, uint16_t qsize) {
    uint32_t sz = vring_size(qsize);
    uint64_t mask = d->modern ? DMA_MASK_64 : DMA_MASK_32;
    if (dma_alloc_coherent(sz, VQ_ALIGN, mask, &d->queue_dma) != 0) return false;
    if (dma_alloc_coherent(sizeof(*d->req), 16, mask, &d->req_dma) != 0) { free_queue(d); return false; }
    if (dma_alloc_coherent(1, 1, mask, &d->status_dma) != 0) { free_queue(d); return false; }
    if (dma_alloc_coherent(VIRTIO_MAX_TRANSFER, 4096, mask, &d->bounce_dma) != 0) { free_queue(d); return false; }

    d->queue = (uint8_t *)d->queue_dma.virt;
    d->queue_phys = d->queue_dma.phys;
    d->queue_size = qsize;
    d->desc = (struct vring_desc *)d->queue;
    d->avail = (uint16_t *)(d->queue + 16u * qsize);
    d->used = (uint16_t *)(d->queue + vring_used_offset(qsize));
    d->req = (struct virtio_blk_req *)d->req_dma.virt;
    d->status = (uint8_t *)d->status_dma.virt;
    d->bounce = (uint8_t *)d->bounce_dma.virt;
    d->req_phys = d->req_dma.phys;
    d->status_phys = d->status_dma.phys;
    d->bounce_phys = d->bounce_dma.phys;
    return true;
}

static void kick(struct virtio_disk *d) {
    if (d->modern) wr16(&d->notify, (uint32_t)d->notify_off * d->notify_multiplier, 0);
    else outw(d->legacy_io + 0x10, 0);
}

static void virtio_wait_turn(struct virtio_disk *d) {
    for (;;) {
        uint64_t flags;
        spinlock_acquire(&d->lock, &flags);
        if (!d->request_busy) {
            d->request_busy = true;
            spinlock_release(&d->lock, flags);
            return;
        }
        spinlock_release(&d->lock, flags);
        if (kernel_can_sleep()) kernel_sleep_ticks(1);
        else asm volatile("pause");
    }
}

static void virtio_finish_turn(struct virtio_disk *d) {
    uint64_t flags;
    spinlock_acquire(&d->lock, &flags);
    d->request_busy = false;
    spinlock_release(&d->lock, flags);
}

static bool virtio_wait_complete(struct virtio_disk *d, uint16_t expected) {
    if (d->irq_registered && kernel_can_sleep()) {
        for (;;) {
            __sync_synchronize();
            if (d->used[1] != expected) return true;
            if (kernel_completion_wait(&d->completion) != 0) break;
        }
    }

    bool sleeping = kernel_can_sleep();
    uint64_t deadline = sleeping ? kernel_ticks() + VIRTIO_TIMEOUT_TICKS : 0;
    uint32_t spins = VIRTIO_TIMEOUT_SPINS;
    for (;;) {
        __sync_synchronize();
        if (d->used[1] != expected) return true;
        if (sleeping) {
            if (kernel_ticks() >= deadline) return false;
            kernel_sleep_ticks(1);
        } else {
            if (!spins--) return false;
            asm volatile("pause");
        }
    }
}

static int virtio_cmd(struct virtio_disk *d, uint32_t type, uint64_t sector, void *buf, uint32_t bytes) {
    virtio_wait_turn(d);

    uint64_t flags;
    spinlock_acquire(&d->lock, &flags);
    d->req->type = type;
    d->req->reserved = 0;
    d->req->sector = sector;
    *d->status = 0xFF;

    d->desc[0].addr = d->req_phys;
    d->desc[0].len = sizeof(*d->req);
    d->desc[0].flags = VRING_DESC_F_NEXT;
    d->desc[0].next = 1;
    d->desc[1].addr = d->bounce_phys;
    d->desc[1].len = bytes;
    d->desc[1].flags = VRING_DESC_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0);
    d->desc[1].next = 2;
    d->desc[2].addr = d->status_phys;
    d->desc[2].len = 1;
    d->desc[2].flags = VRING_DESC_F_WRITE;
    d->desc[2].next = 0;

    uint16_t idx = d->avail[1];
    uint16_t expected = d->last_used;
    d->avail[2 + (idx % d->queue_size)] = 0;
    kernel_completion_reset(&d->completion);
    __sync_synchronize();
    d->avail[1] = idx + 1;
    d->irq_seen = 0;
    kick(d);
    spinlock_release(&d->lock, flags);

    bool completed = virtio_wait_complete(d, expected);

    int ok = 0;
    spinlock_acquire(&d->lock, &flags);
    if (completed && d->used[1] != expected) {
        d->last_used = expected + 1;
        ok = (*d->status == VIRTIO_BLK_S_OK);
    }
    spinlock_release(&d->lock, flags);
    virtio_finish_turn(d);
    return ok ? 0 : -1;
}

static uint64_t virtio_rw(struct virtio_disk *d, uint64_t off, uint64_t size, uint8_t *buf, bool write) {
    if (!d || !buf || d->sector_size == 0) return 0;
    if (d->sectors && d->sectors > UINT64_MAX / d->sector_size) return 0;
    uint64_t disk_bytes = d->sectors * (uint64_t)d->sector_size;
    if (off >= disk_bytes) return 0;
    if (size > disk_bytes - off) size = disk_bytes - off;
    uint64_t done = 0;
    while (done < size) {
        uint64_t pos = off + done;
        uint64_t sector = pos / d->sector_size;
        uint32_t in_sector = (uint32_t)(pos % d->sector_size);
        uint32_t chunk = (uint32_t)(size - done);
        if (chunk > VIRTIO_MAX_TRANSFER) chunk = VIRTIO_MAX_TRANSFER;
        uint32_t aligned = chunk;
        if (in_sector || (aligned % d->sector_size)) {
            uint32_t span = d->sector_size - in_sector;
            if (span > chunk) span = chunk;
            if (virtio_cmd(d, VIRTIO_BLK_T_IN, sector, d->bounce, d->sector_size) != 0) break;
            if (write) {
                memcpy(d->bounce + in_sector, buf + done, span);
                if (virtio_cmd(d, VIRTIO_BLK_T_OUT, sector, d->bounce, d->sector_size) != 0) break;
            } else {
                memcpy(buf + done, d->bounce + in_sector, span);
            }
            done += span;
            continue;
        }
        aligned -= aligned % d->sector_size;
        if (!aligned) aligned = d->sector_size;
        if (write) memcpy(d->bounce, buf + done, aligned);
        if (virtio_cmd(d, write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN, sector, d->bounce, aligned) != 0) break;
        if (!write) memcpy(buf + done, d->bounce, aligned);
        done += aligned;
    }
    return done;
}

static uint64_t virtio_read(void *priv, uint64_t off, uint64_t size, uint8_t *buf) { return virtio_rw((struct virtio_disk *)priv, off, size, buf, false); }
static uint64_t virtio_write(void *priv, uint64_t off, uint64_t size, uint8_t *buf) { return virtio_rw((struct virtio_disk *)priv, off, size, buf, true); }

static bool setup_modern_caps(struct virtio_disk *d) {
    uint16_t cap = pci_find_capability(d->pdev->segment, d->pdev->bus, d->pdev->slot, d->pdev->func, PCI_CAP_VENDOR);
    bool have_common = false, have_notify = false, have_isr = false, have_device = false;
    while (cap) {
        uint8_t cfg_type = pci_read8(d->pdev->segment, d->pdev->bus, d->pdev->slot, d->pdev->func, cap + 3);
        uint8_t bar = pci_read8(d->pdev->segment, d->pdev->bus, d->pdev->slot, d->pdev->func, cap + 4);
        uint32_t off = pci_read32(d->pdev->segment, d->pdev->bus, d->pdev->slot, d->pdev->func, cap + 8);
        uint32_t len = pci_read32(d->pdev->segment, d->pdev->bus, d->pdev->slot, d->pdev->func, cap + 12);
        if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) have_common = map_cap_region(d->pdev, bar, off, len, &d->common);
        else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
            d->notify_multiplier = pci_read32(d->pdev->segment, d->pdev->bus, d->pdev->slot, d->pdev->func, cap + 16);
            have_notify = map_cap_region(d->pdev, bar, off, len, &d->notify);
        } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG) have_isr = map_cap_region(d->pdev, bar, off, len, &d->isr);
        else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) have_device = map_cap_region(d->pdev, bar, off, len, &d->device);
        cap = pci_read8(d->pdev->segment, d->pdev->bus, d->pdev->slot, d->pdev->func, cap + 1);
    }
    return have_common && have_notify && have_isr && have_device;
}

static bool init_modern(struct virtio_disk *d) {
    if (!setup_modern_caps(d)) return false;
    modern_status_set(d, 0);
    modern_status_set(d, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    uint64_t offered = modern_features(d);
    if ((offered & (1ULL << VIRTIO_F_VERSION_1)) == 0) return false;
    uint64_t features = (1ULL << VIRTIO_F_VERSION_1);
    if (offered & (1ULL << VIRTIO_BLK_F_BLK_SIZE)) features |= 1ULL << VIRTIO_BLK_F_BLK_SIZE;
    modern_write_features(d, features);
    modern_status_set(d, modern_status_get(d) | VIRTIO_STATUS_FEATURES_OK);
    if ((modern_status_get(d) & VIRTIO_STATUS_FEATURES_OK) == 0) return false;

    wr16(&d->common, 22, 0);
    uint16_t qsize = rd16(&d->common, 24);
    if (qsize > VQ_NUM) qsize = VQ_NUM;
    if (qsize < 3 || !alloc_queue(d, qsize)) return false;
    wr16(&d->common, 24, qsize);
    d->notify_off = rd16(&d->common, 30);
    wr64(&d->common, 32, d->queue_phys);
    wr64(&d->common, 40, d->queue_phys + 16u * qsize);
    wr64(&d->common, 48, d->queue_phys + vring_used_offset(qsize));
    wr16(&d->common, 28, 1);

    d->sectors = rd64(&d->device, 0);
    d->sector_size = (features & (1ULL << VIRTIO_BLK_F_BLK_SIZE)) ? rd32(&d->device, 20) : 512;
    modern_status_set(d, modern_status_get(d) | VIRTIO_STATUS_DRIVER_OK);
    return d->sectors != 0 && d->sector_size != 0;
}

static bool init_legacy(struct virtio_disk *d) {
    struct pci_resource *bar = &d->pdev->bars[0];
    if (bar->type != PCI_BAR_TYPE_IO || bar->base > UINT16_MAX) return false;
    d->legacy_io = (uint16_t)bar->base;
    legacy_status_set(d, 0);
    legacy_status_set(d, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    uint64_t offered = legacy_features(d);
    uint64_t features = 0;
    if (offered & (1ULL << VIRTIO_BLK_F_BLK_SIZE)) features |= 1ULL << VIRTIO_BLK_F_BLK_SIZE;
    outl(d->legacy_io + 0x04, (uint32_t)features);
    legacy_status_set(d, legacy_status_get(d) | VIRTIO_STATUS_FEATURES_OK);

    outw(d->legacy_io + 0x0E, 0);
    uint16_t qsize = inw(d->legacy_io + 0x0C);
    if (qsize < 3 || qsize > VQ_LEGACY_MAX || !alloc_queue(d, qsize)) return false;
    outl(d->legacy_io + 0x08, (uint32_t)(d->queue_phys >> 12));
    d->sectors = ((uint64_t)inl(d->legacy_io + 0x14)) | ((uint64_t)inl(d->legacy_io + 0x18) << 32);
    d->sector_size = (features & (1ULL << VIRTIO_BLK_F_BLK_SIZE)) ? inl(d->legacy_io + 0x28) : 512;
    legacy_status_set(d, legacy_status_get(d) | VIRTIO_STATUS_DRIVER_OK);
    return d->sectors != 0 && d->sector_size != 0;
}

static int virtio_probe(struct device *dev) {
    struct pci_device *pdev = (struct pci_device *)dev->bus_data;
    if (!pdev) return -1;
    struct virtio_disk *d = (struct virtio_disk *)kmalloc(sizeof(*d));
    if (!d) return -1;
    memset(d, 0, sizeof(*d));
    d->pdev = pdev;
    spinlock_init(&d->lock);
    kernel_completion_init(&d->completion, false);
    pci_enable_bus_mastering(pdev->segment, pdev->bus, pdev->slot, pdev->func);

    bool ok = false;
    if (pdev->device_id == VIRTIO_MODERN_BLK || pdev->device_id >= 0x1040) {
        d->modern = true;
        ok = init_modern(d);
    }
    if (!ok && pdev->device_id == VIRTIO_LEGACY_BLK) {
        d->modern = false;
        ok = init_legacy(d);
    }
    if (!ok) {
        if (d->modern) modern_status_set(d, VIRTIO_STATUS_FAILED);
        else if (d->legacy_io) legacy_status_set(d, VIRTIO_STATUS_FAILED);
        free_queue(d);
        kfree(d);
        return -1;
    }

    d->irq = pci_read8(pdev->segment, pdev->bus, pdev->slot, pdev->func, 0x3C);
    if (d->irq != 0 && d->irq != VIRTIO_IRQ_NONE && request_irq(d->irq, virtio_irq, IRQ_AFFINITY_ALL, d) == 0) {
        d->irq_registered = true;
        klog(LOG_INFO, "virtio-blk: using IRQ %d\n", d->irq);
    } else {
        klog(LOG_WARN, "virtio-blk: using polling\n");
    }

    uint64_t id = next_disk++;
    if (id >= 26) { free_queue(d); kfree(d); return -1; }
    d->name[0] = 'v'; d->name[1] = 'd'; d->name[2] = (char)('a' + id); d->name[3] = 0;
    uint64_t bytes = d->sectors * (uint64_t)d->sector_size;
    if (devfs_register_block(d->name, &virtio_blk_ops, d, bytes) != 0) { free_queue(d); kfree(d); return -1; }

    klog(LOG_INFO, "virtio-blk: registered /dev/%s sectors=%lu sector_size=%u mode=%s\n", d->name, d->sectors, d->sector_size, d->modern ? "modern" : "legacy");
    return 0;
}

static void virtio_remove(struct device *dev) { (void)dev; }

static struct pci_device_id ids[] = {
    {VIRTIO_VENDOR, VIRTIO_LEGACY_BLK, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS},
    {VIRTIO_VENDOR, VIRTIO_MODERN_BLK, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS},
    {0, 0, 0, 0, 0},
};

static void virtio_copy_text(char *dst, uint64_t cap, const char *src) {
    if (!dst || !cap) return;
    uint64_t i = 0;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int virtio_info(void *priv, struct device_info *out) {
    struct virtio_disk *d = (struct virtio_disk *)priv;
    if (!d || !out || !d->sector_size) return -1;
    out->version = DEVICE_INFO_VERSION;
    out->kind = DEVICE_INFO_KIND_BLOCK;
    out->logical_block_size = d->sector_size;
    out->physical_block_size = d->sector_size;
    out->block_count = d->sectors;
    out->size_bytes = d->sectors * (uint64_t)d->sector_size;
    out->max_size_bytes = out->size_bytes;
    out->max_transfer_bytes = d->sector_size;
    virtio_copy_text(out->driver, sizeof(out->driver), "virtio_blk");
    virtio_copy_text(out->media_type, sizeof(out->media_type), d->modern ? "virtio-modern" : "virtio-legacy");
    virtio_copy_text(out->type, sizeof(out->type), "disk");
    return 0;
}

static struct device_ops virtio_blk_ops = {.read = virtio_read, .write = virtio_write, .info = virtio_info};
static struct driver virtio_driver = {.name = "virtio_blk", .bus = NULL, .module = NULL, .id_table = ids, .probe = virtio_probe, .remove = virtio_remove};

static int virtio_blk_init(void) {
    virtio_driver.bus = find_bus("pci");
    if (!virtio_driver.bus) return -1;
    driver_register(&virtio_driver);
    return 0;
}


MODULE_INFO("virtio_blk", virtio_blk_init, 0, NULL, "pci_bus");
