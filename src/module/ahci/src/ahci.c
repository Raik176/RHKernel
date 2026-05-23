#include "mod/device.h"
#include "mod/heap.h"
#include "mod/interrupt.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "mod/util.h"
#include "mod/vmm.h"
#include "pci_bus.h"
#include "smp/lock.h"
#include "string.h"

#define AHCI_CAP 0x00
#define AHCI_GHC 0x04
#define AHCI_IS  0x08
#define AHCI_PI  0x0C
#define AHCI_VS  0x10
#define AHCI_CAP2 0x24
#define AHCI_BOHC 0x28

#define AHCI_CAP_NCS_SHIFT 8
#define AHCI_CAP_NCS_MASK  (0x1Fu << AHCI_CAP_NCS_SHIFT)
#define AHCI_CAP_S64A      (1u << 31)
#define AHCI_CAP2_BOH       (1u << 0)

#define AHCI_GHC_HR (1u << 0)
#define AHCI_GHC_IE (1u << 1)
#define AHCI_GHC_AE (1u << 31)

#define AHCI_BOHC_BOS (1u << 0)
#define AHCI_BOHC_OOS (1u << 1)
#define AHCI_BOHC_BB  (1u << 4)

#define AHCI_PxCLB  0x00
#define AHCI_PxCLBU 0x04
#define AHCI_PxFB   0x08
#define AHCI_PxFBU  0x0C
#define AHCI_PxIS   0x10
#define AHCI_PxIE   0x14
#define AHCI_PxCMD  0x18
#define AHCI_PxTFD  0x20
#define AHCI_PxSIG  0x24
#define AHCI_PxSSTS 0x28
#define AHCI_PxSCTL 0x2C
#define AHCI_PxSERR 0x30
#define AHCI_PxSACT 0x34
#define AHCI_PxCI   0x38

#define AHCI_PxIS_DHRS (1u << 0)
#define AHCI_PxIS_PSS  (1u << 1)
#define AHCI_PxIS_DSS  (1u << 2)
#define AHCI_PxIS_SDBS (1u << 3)
#define AHCI_PxIS_UFS  (1u << 4)
#define AHCI_PxIS_DPS  (1u << 5)
#define AHCI_PxIS_PCS  (1u << 6)
#define AHCI_PxIS_DMPS (1u << 7)
#define AHCI_PxIS_PRCS (1u << 22)
#define AHCI_PxIS_IPMS (1u << 23)
#define AHCI_PxIS_OFS  (1u << 24)
#define AHCI_PxIS_INFS (1u << 26)
#define AHCI_PxIS_IFS  (1u << 27)
#define AHCI_PxIS_HBDS (1u << 28)
#define AHCI_PxIS_HBFS (1u << 29)
#define AHCI_PxIS_TFES (1u << 30)
#define AHCI_PxIS_CPDS (1u << 31)
#define AHCI_PxIS_ERROR (AHCI_PxIS_UFS | AHCI_PxIS_IPMS | AHCI_PxIS_OFS | AHCI_PxIS_INFS | \
                         AHCI_PxIS_IFS | AHCI_PxIS_HBDS | AHCI_PxIS_HBFS | AHCI_PxIS_TFES)
#define AHCI_PxIE_DEFAULT (AHCI_PxIS_DHRS | AHCI_PxIS_PSS | AHCI_PxIS_DSS | AHCI_PxIS_SDBS | \
                           AHCI_PxIS_ERROR)

#define AHCI_CMD_ST  (1u << 0)
#define AHCI_CMD_FRE (1u << 4)
#define AHCI_CMD_FR  (1u << 14)
#define AHCI_CMD_CR  (1u << 15)
#define AHCI_CMD_ICC_ACTIVE (1u << 28)

#define AHCI_TFD_BSY 0x80
#define AHCI_TFD_DRQ 0x08

#define AHCI_SIG_ATA   0x00000101u
#define AHCI_SIG_ATAPI 0xEB140101u
#define AHCI_SIG_SEMB  0xC33C0101u
#define AHCI_SIG_PM    0x96690101u

#define SATA_DEV_PRESENT 3
#define SATA_IPM_ACTIVE  1

#define ATA_CMD_IDENTIFY      0xEC
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

#define ATA_DEV_BUSY 0x80
#define ATA_DEV_DRQ  0x08

#define FIS_TYPE_REG_H2D 0x27

#define AHCI_LEGACY_SECTOR_SIZE 512u
#define AHCI_MAX_LOGICAL_SECTOR_SIZE 4096u
#define AHCI_MAX_SECTORS 256u
#define AHCI_BOUNCE_SIZE (AHCI_MAX_LOGICAL_SECTOR_SIZE * AHCI_MAX_SECTORS)
#define AHCI_PORT_DMA_SIZE 16384u
#define AHCI_DISK_NAME_MAX 40
#define AHCI_PART_NAME_MAX 48
#define AHCI_MAX_GPT_PARTITION_SCAN 4096u
#define AHCI_MAX_EBR_CHAIN_SCAN 256
#define AHCI_TIMEOUT 1000000u
#define AHCI_RESET_TIMEOUT 10000000u
#define AHCI_IRQ_NONE 0xFFu
#define AHCI_GPT_MAX_ENTRY_SIZE 1024u

struct hba_cmd_header {
    uint8_t cfl;
    uint8_t flags;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} __attribute__((packed));

struct hba_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc_i;
} __attribute__((packed));

struct hba_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct hba_prdt_entry prdt[1];
} __attribute__((packed));

struct fis_reg_h2d {
    uint8_t fis_type;
    uint8_t pmport_c;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
} __attribute__((packed));

struct ahci_controller;

struct ahci_partition {
    struct ahci_disk *disk;
    uint64_t start_lba;
    uint64_t sectors;
    char name[AHCI_PART_NAME_MAX];
    struct ahci_partition *next;
};

struct ahci_disk {
    struct ahci_controller *ctrl;
    int port_no;
    uint64_t index;
    uint64_t sectors;
    uint32_t sector_size;
    char name[AHCI_DISK_NAME_MAX];
    uint8_t *bounce;
    void *bounce_raw;
    void *port_dma_raw;
    spinlock_t lock;
    spinlock_t bounce_lock;
    uint32_t active_slots;
    struct ahci_partition *partitions;
};

struct ahci_controller {
    volatile uint8_t *abar;
    uint64_t abar_phys;
    uint64_t abar_size;
    uint8_t irq;
    bool irq_registered;
    volatile uint32_t irq_pending;
    volatile uint32_t port_irq[32];
    struct ahci_disk *ports[32];
    uint8_t command_slots;
    bool supports_64bit_dma;
};

static uint64_t ahci_next_disk_index = 0;
static uint64_t ahci_part_count = 0;
static struct device_ops ahci_disk_ops;
static struct device_ops ahci_part_ops;


static int ahci_make_sd_name(uint64_t index, char *out, size_t out_size) {
    char suffix[AHCI_DISK_NAME_MAX - 2];
    size_t suffix_len = 0;

    if (!out || out_size < 4) return -1;

    for (;;) {
        if (suffix_len >= sizeof(suffix)) return -1;

        suffix[suffix_len++] = (char)('a' + (index % 26));

        if (index < 26) break;
        index = (index / 26) - 1;
    }

    if (2 + suffix_len + 1 > out_size) return -1;

    out[0] = 's';
    out[1] = 'd';
    for (size_t i = 0; i < suffix_len; ++i) {
        out[2 + i] = suffix[suffix_len - 1 - i];
    }
    out[2 + suffix_len] = 0;
    return 0;
}

static inline volatile uint32_t *ahci_reg(struct ahci_controller *ctrl, uint32_t off) {
    return (volatile uint32_t *)(ctrl->abar + off);
}

static inline volatile uint32_t *ahci_port_reg(struct ahci_controller *ctrl, int port, uint32_t off) {
    return (volatile uint32_t *)(ctrl->abar + 0x100 + ((uint32_t)port * 0x80) + off);
}

static uint64_t ahci_v2p(void *ptr) {
    return v2p(ptr);
}

static int ahci_wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t timeout) {
    while ((*reg & mask) && timeout--) asm volatile("pause");
    return (*reg & mask) ? -1 : 0;
}

static void ahci_stop_port(struct ahci_controller *ctrl, int port) {
    volatile uint32_t *cmd = ahci_port_reg(ctrl, port, AHCI_PxCMD);
    *cmd &= ~AHCI_CMD_ST;
    ahci_wait_clear(cmd, AHCI_CMD_CR, AHCI_TIMEOUT);
    *cmd &= ~AHCI_CMD_FRE;
    ahci_wait_clear(cmd, AHCI_CMD_FR, AHCI_TIMEOUT);
}

static int ahci_start_port(struct ahci_controller *ctrl, int port) {
    volatile uint32_t *cmd = ahci_port_reg(ctrl, port, AHCI_PxCMD);
    if (ahci_wait_clear(cmd, AHCI_CMD_CR, AHCI_TIMEOUT) != 0) return -1;
    *cmd |= AHCI_CMD_FRE;
    *cmd |= AHCI_CMD_ST | AHCI_CMD_ICC_ACTIVE;
    return 0;
}

static int ahci_reserve_slot(struct ahci_disk *disk) {
    struct ahci_controller *ctrl = disk->ctrl;
    int port = disk->port_no;
    uint64_t flags;
    spinlock_acquire(&disk->lock, &flags);
    uint32_t slots = *ahci_port_reg(ctrl, port, AHCI_PxSACT) | *ahci_port_reg(ctrl, port, AHCI_PxCI) | disk->active_slots;
    uint8_t command_slots = ctrl->command_slots ? ctrl->command_slots : 1;
    for (uint8_t i = 0; i < command_slots; i++) {
        if ((slots & (1u << i)) == 0) {
            disk->active_slots |= 1u << i;
            spinlock_release(&disk->lock, flags);
            return i;
        }
    }
    spinlock_release(&disk->lock, flags);
    return -1;
}

static void ahci_release_slot(struct ahci_disk *disk, int slot) {
    uint64_t flags;
    spinlock_acquire(&disk->lock, &flags);
    disk->active_slots &= ~(1u << slot);
    spinlock_release(&disk->lock, flags);
}

static enum irq_return ahci_irq_handler(void *priv) {
    struct ahci_controller *ctrl = (struct ahci_controller *)priv;
    if (!ctrl || !ctrl->abar) return IRQ_NOT_HANDLED;

    uint32_t global = *ahci_reg(ctrl, AHCI_IS);
    if (global == 0) return IRQ_NOT_HANDLED;

    for (int port = 0; port < 32; port++) {
        if ((global & (1u << port)) == 0) continue;
        uint32_t status = *ahci_port_reg(ctrl, port, AHCI_PxIS);
        if (status) {
            ctrl->port_irq[port] |= status;
            *ahci_port_reg(ctrl, port, AHCI_PxIS) = status;
        }
    }

    ctrl->irq_pending |= global;
    *ahci_reg(ctrl, AHCI_IS) = global;
    return IRQ_HANDLED;
}

static void ahci_clear_interrupts(struct ahci_controller *ctrl) {
    if (!ctrl || !ctrl->abar) return;
    uint32_t pi = *ahci_reg(ctrl, AHCI_PI);
    for (int port = 0; port < 32; port++) {
        if ((pi & (1u << port)) == 0) continue;
        ctrl->port_irq[port] = 0;
        *ahci_port_reg(ctrl, port, AHCI_PxIS) = 0xFFFFFFFFu;
    }
    ctrl->irq_pending = 0;
    *ahci_reg(ctrl, AHCI_IS) = 0xFFFFFFFFu;
}

static bool ahci_dma_range_supported(struct ahci_controller *ctrl, uint64_t phys, uint64_t bytes) {
    if (!bytes) return false;
    if (UINT64_MAX - phys < bytes - 1) return false;
    if (ctrl->supports_64bit_dma) return true;
    return phys + bytes - 1 <= UINT32_MAX;
}

static bool ahci_size_mul_ok(uint64_t a, uint64_t b, uint64_t *out) {
    if (a && b > UINT64_MAX / a) return false;
    *out = a * b;
    return true;
}

static bool ahci_sector_buffer_valid(struct ahci_disk *disk, uint32_t sectors, uint32_t *bytes_out) {
    uint64_t bytes;
    if (!disk || sectors == 0) return false;
    if (!ahci_size_mul_ok(sectors, disk->sector_size, &bytes)) return false;
    if (bytes > AHCI_BOUNCE_SIZE || bytes > UINT32_MAX) return false;
    *bytes_out = (uint32_t)bytes;
    return true;
}

static uint32_t ahci_max_transfer_sectors(struct ahci_disk *disk) {
    if (!disk || disk->sector_size == 0) return 0;
    uint32_t max = AHCI_BOUNCE_SIZE / disk->sector_size;
    return max > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : max;
}

static void ahci_recover_port(struct ahci_controller *ctrl, int port) {
    ahci_stop_port(ctrl, port);
    *ahci_port_reg(ctrl, port, AHCI_PxSERR) = 0xFFFFFFFFu;
    *ahci_port_reg(ctrl, port, AHCI_PxIS) = 0xFFFFFFFFu;
    ahci_start_port(ctrl, port);
}


static int ahci_command(struct ahci_disk *disk, uint8_t command, uint64_t lba, uint16_t count,
                        void *buffer, uint32_t bytes, int write) {
    struct ahci_controller *ctrl = disk->ctrl;
    int port = disk->port_no;

    if (count == 0) count = AHCI_MAX_SECTORS;
    if (bytes == 0 || bytes > AHCI_BOUNCE_SIZE || lba > 0x0000FFFFFFFFFFFFULL) return -1;

    volatile uint32_t *tfd = ahci_port_reg(ctrl, port, AHCI_PxTFD);
    uint32_t spin = AHCI_TIMEOUT;
    while ((*tfd & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) && spin--) asm volatile("pause");
    if (*tfd & (AHCI_TFD_BSY | AHCI_TFD_DRQ)) return -1;

    int slot = ahci_reserve_slot(disk);
    if (slot < 0) return -1;

    uint64_t cmdlist_phys = ((uint64_t)(*ahci_port_reg(ctrl, port, AHCI_PxCLBU)) << 32) |
                            *ahci_port_reg(ctrl, port, AHCI_PxCLB);
    struct hba_cmd_header *cmdlist = (struct hba_cmd_header *)p2v(cmdlist_phys);
    struct hba_cmd_header *hdr = &cmdlist[slot];

    uint64_t table_phys = ((uint64_t)hdr->ctbau << 32) | hdr->ctba;
    struct hba_cmd_table *tbl = (struct hba_cmd_table *)p2v(table_phys);
    memset(tbl, 0, sizeof(struct hba_cmd_table));

    hdr->cfl = sizeof(struct fis_reg_h2d) / sizeof(uint32_t);
    hdr->flags = write ? (1u << 6) : 0;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    uint64_t buf_phys = ahci_v2p(buffer);
    if (!ahci_dma_range_supported(ctrl, buf_phys, bytes)) { ahci_release_slot(disk, slot); return -1; }
    tbl->prdt[0].dba = (uint32_t)buf_phys;
    tbl->prdt[0].dbau = (uint32_t)(buf_phys >> 32);
    tbl->prdt[0].dbc_i = (bytes - 1) | (1u << 31);

    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 1u << 7;
    fis->command = command;
    fis->device = 1u << 6;
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count >> 8);

    *ahci_port_reg(ctrl, port, AHCI_PxIS) = 0xFFFFFFFFu;
    ctrl->port_irq[port] = 0;
    ctrl->irq_pending &= ~(1u << port);
    *ahci_port_reg(ctrl, port, AHCI_PxCI) = 1u << slot;

    uint32_t timeout = AHCI_TIMEOUT * 10;
    while (timeout--) {
        uint32_t ci = *ahci_port_reg(ctrl, port, AHCI_PxCI);
        uint32_t is = *ahci_port_reg(ctrl, port, AHCI_PxIS) | ctrl->port_irq[port];
        if (is & AHCI_PxIS_ERROR) { ahci_recover_port(ctrl, port); ahci_release_slot(disk, slot); return -1; }
        if ((ci & (1u << slot)) == 0) break;
        if (ctrl->irq_pending & (1u << port)) {
            ctrl->irq_pending &= ~(1u << port);
        }
        asm volatile("pause");
    }
    if (*ahci_port_reg(ctrl, port, AHCI_PxCI) & (1u << slot)) { ahci_recover_port(ctrl, port); ahci_release_slot(disk, slot); return -1; }

    if ((*ahci_port_reg(ctrl, port, AHCI_PxIS) | ctrl->port_irq[port]) & AHCI_PxIS_ERROR) { ahci_recover_port(ctrl, port); ahci_release_slot(disk, slot); return -1; }
    ahci_release_slot(disk, slot);
    return 0;
}

static int ahci_identify(struct ahci_disk *disk) {
    memset(disk->bounce, 0, AHCI_LEGACY_SECTOR_SIZE);
    if (ahci_command(disk, ATA_CMD_IDENTIFY, 0, 1, disk->bounce, AHCI_LEGACY_SECTOR_SIZE, 0) != 0) {
        return -1;
    }

    uint16_t *id = (uint16_t *)disk->bounce;
    if ((id[83] & (1u << 10)) != 0) {
        disk->sectors = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                        ((uint64_t)id[101] << 16) | id[100];
    } else {
        disk->sectors = ((uint32_t)id[61] << 16) | id[60];
    }
    disk->sector_size = AHCI_LEGACY_SECTOR_SIZE;
    if ((id[106] & (1u << 14)) && !(id[106] & (1u << 15)) && (id[106] & (1u << 12))) {
        uint32_t words = ((uint32_t)id[118] << 16) | id[117];
        uint64_t bytes = (uint64_t)words * 2u;
        if (bytes < AHCI_LEGACY_SECTOR_SIZE || bytes > AHCI_MAX_LOGICAL_SECTOR_SIZE ||
            (bytes & (bytes - 1)) != 0) {
            klog(LOG_WARN, "AHCI: unsupported logical sector size %x:%x on port %d\n",
                 (uint32_t)(bytes >> 32), (uint32_t)bytes, disk->port_no);
            return -1;
        }
        disk->sector_size = (uint32_t)bytes;
    }
    return disk->sectors ? 0 : -1;
}

static int ahci_rw_sectors(struct ahci_disk *disk, uint64_t lba, uint32_t sectors, int write) {
    uint8_t cmd = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    uint32_t bytes = 0;
    if (!ahci_sector_buffer_valid(disk, sectors, &bytes)) return -1;
    uint16_t count = (sectors == AHCI_MAX_SECTORS) ? 0 : (uint16_t)sectors;
    return ahci_command(disk, cmd, lba, count, disk->bounce, bytes, write);
}

static int ahci_rw_sectors_into(struct ahci_disk *disk, uint64_t lba, uint32_t sectors, int write, void *buffer) {
    uint8_t cmd = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    uint32_t bytes = 0;
    if (!ahci_sector_buffer_valid(disk, sectors, &bytes)) return -1;
    uint64_t phys = ahci_v2p(buffer);
    if ((((uintptr_t)buffer & 4095u) + bytes > 4096u) || !ahci_dma_range_supported(disk->ctrl, phys, bytes)) return -1;
    uint16_t count = (sectors == AHCI_MAX_SECTORS) ? 0 : (uint16_t)sectors;
    return ahci_command(disk, cmd, lba, count, buffer, bytes, write);
}

static uint64_t ahci_read_dev(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer) {
    struct ahci_disk *disk = (struct ahci_disk *)priv;
    if (!disk || !buffer || size == 0) return 0;
    uint64_t disk_bytes;
    if (!ahci_size_mul_ok(disk->sectors, disk->sector_size, &disk_bytes)) return 0;
    if (offset >= disk_bytes) return 0;

    uint64_t max = disk_bytes - offset;
    if (size > max) size = max;

    uint64_t done = 0;
    while (done < size) {
        uint64_t abs = offset + done;
        uint64_t lba = abs / disk->sector_size;
        uint32_t in_sector = (uint32_t)(abs % disk->sector_size);
        uint32_t sectors = ahci_max_transfer_sectors(disk);
        uint64_t remaining = size - done;

        if (in_sector != 0 || remaining < disk->sector_size) {
            sectors = 1;
        } else {
            uint64_t whole = remaining / disk->sector_size;
            if (whole < sectors) sectors = (uint32_t)whole;
            if (sectors == 0) sectors = 1;
        }

        uint64_t copy = (uint64_t)sectors * disk->sector_size - in_sector;
        if (copy > remaining) copy = remaining;

        if (in_sector == 0 && copy == (uint64_t)sectors * disk->sector_size &&
            ahci_rw_sectors_into(disk, lba, sectors, 0, buffer + done) == 0) {
            done += copy;
            continue;
        }

        uint64_t flags;
        spinlock_acquire(&disk->bounce_lock, &flags);
        if (ahci_rw_sectors(disk, lba, sectors, 0) != 0) {
            spinlock_release(&disk->bounce_lock, flags);
            break;
        }
        memcpy(buffer + done, disk->bounce + in_sector, copy);
        spinlock_release(&disk->bounce_lock, flags);
        done += copy;
    }

    return done;
}

static uint64_t ahci_write_dev(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer) {
    struct ahci_disk *disk = (struct ahci_disk *)priv;
    if (!disk || !buffer || size == 0) return 0;
    uint64_t disk_bytes;
    if (!ahci_size_mul_ok(disk->sectors, disk->sector_size, &disk_bytes)) return 0;
    if (offset >= disk_bytes) return 0;

    uint64_t max = disk_bytes - offset;
    if (size > max) size = max;

    uint64_t done = 0;
    while (done < size) {
        uint64_t abs = offset + done;
        uint64_t lba = abs / disk->sector_size;
        uint32_t in_sector = (uint32_t)(abs % disk->sector_size);
        uint64_t remaining = size - done;
        uint32_t sectors = ahci_max_transfer_sectors(disk);

        if (in_sector != 0 || remaining < disk->sector_size) {
            sectors = 1;
        } else {
            uint64_t whole = remaining / disk->sector_size;
            if (whole < sectors) sectors = (uint32_t)whole;
            if (sectors == 0) sectors = 1;
        }

        uint64_t copy = (uint64_t)sectors * disk->sector_size - in_sector;
        if (copy > remaining) copy = remaining;

        if (in_sector == 0 && copy == (uint64_t)sectors * disk->sector_size &&
            ahci_rw_sectors_into(disk, lba, sectors, 1, buffer + done) == 0) {
            done += copy;
            continue;
        }

        uint64_t flags;
        spinlock_acquire(&disk->bounce_lock, &flags);
        if (ahci_rw_sectors(disk, lba, sectors, 0) != 0) {
            spinlock_release(&disk->bounce_lock, flags);
            break;
        }
        memcpy(disk->bounce + in_sector, buffer + done, copy);

        if (ahci_rw_sectors(disk, lba, sectors, 1) != 0) {
            spinlock_release(&disk->bounce_lock, flags);
            break;
        }
        spinlock_release(&disk->bounce_lock, flags);
        done += copy;
    }

    return done;
}

static struct device_ops ahci_disk_ops = {.read = ahci_read_dev, .write = ahci_write_dev};

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const uint8_t *p) {
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

static void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint64_t ahci_part_read(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer) {
    struct ahci_partition *part = (struct ahci_partition *)priv;
    if (!part || !part->disk || !buffer || size == 0) return 0;

    uint64_t bytes;
    if (!ahci_size_mul_ok(part->sectors, part->disk->sector_size, &bytes)) return 0;
    if (offset >= bytes) return 0;
    if (size > bytes - offset) size = bytes - offset;
    uint64_t base;
    if (!ahci_size_mul_ok(part->start_lba, part->disk->sector_size, &base)) return 0;
    return ahci_read_dev(part->disk, base + offset, size, buffer);
}

static uint64_t ahci_part_write(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer) {
    struct ahci_partition *part = (struct ahci_partition *)priv;
    if (!part || !part->disk || !buffer || size == 0) return 0;

    uint64_t bytes;
    if (!ahci_size_mul_ok(part->sectors, part->disk->sector_size, &bytes)) return 0;
    if (offset >= bytes) return 0;
    if (size > bytes - offset) size = bytes - offset;
    uint64_t base;
    if (!ahci_size_mul_ok(part->start_lba, part->disk->sector_size, &base)) return 0;
    return ahci_write_dev(part->disk, base + offset, size, buffer);
}

static struct device_ops ahci_part_ops = {.read = ahci_part_read, .write = ahci_part_write};

static bool lba_range_valid(struct ahci_disk *disk, uint64_t start, uint64_t sectors);

static int ahci_register_partition(struct ahci_disk *disk, uint64_t start_lba, uint64_t sectors,
                                   int number) {
    if (!lba_range_valid(disk, start_lba, sectors)) return -1;
    struct ahci_partition *part = (struct ahci_partition *)kmalloc(sizeof(*part));
    if (!part) return -1;
    memset(part, 0, sizeof(*part));
    part->disk = disk;
    part->start_lba = start_lba;
    part->sectors = sectors;
    snprintf(part->name, sizeof(part->name), "%s%d", disk->name, number);

    uint64_t part_bytes;
    if (!ahci_size_mul_ok(sectors, disk->sector_size, &part_bytes)) {
        kfree(part);
        return -1;
    }

    if (devfs_register_block(part->name, &ahci_part_ops, part, part_bytes) != 0) {
        kfree(part);
        return -1;
    }

    part->next = disk->partitions;
    disk->partitions = part;
    ahci_part_count++;
    klog(LOG_INFO, "AHCI: registered /dev/%s start=%x:%x sectors=%x:%x\n", part->name,
         (uint32_t)(start_lba >> 32), (uint32_t)start_lba, (uint32_t)(sectors >> 32),
         (uint32_t)sectors);
    return 0;
}

static bool u64_add_overflows(uint64_t a, uint64_t b) {
    return UINT64_MAX - a < b;
}

static bool lba_range_valid(struct ahci_disk *disk, uint64_t start, uint64_t sectors) {
    if (!disk || sectors == 0 || start >= disk->sectors) return false;
    if (u64_add_overflows(start, sectors)) return false;
    return start + sectors <= disk->sectors;
}

static bool byte_range_for_lba_valid(struct ahci_disk *disk, uint64_t start_lba, uint64_t sectors) {
    if (!lba_range_valid(disk, start_lba, sectors)) return false;
    if (start_lba > UINT64_MAX / disk->sector_size || sectors > UINT64_MAX / disk->sector_size) return false;
    return true;
}

static bool lba_ranges_overlap(uint64_t a_start, uint64_t a_count, uint64_t b_start, uint64_t b_count) {
    return a_start < b_start + b_count && b_start < a_start + a_count;
}

static bool mbr_entry_is_empty(const uint8_t *ent) {
    for (int i = 0; i < 16; i++) if (ent[i]) return false;
    return true;
}

static bool mbr_boot_valid(uint8_t boot) {
    return boot == 0x00 || boot == 0x80;
}

static bool mbr_entry_valid(struct ahci_disk *disk, const uint8_t *ent) {
    uint8_t type = ent[4];
    uint32_t start = rd_le32(ent + 8);
    uint32_t count = rd_le32(ent + 12);

    if (!mbr_boot_valid(ent[0])) return false;
    if (type == 0 || count == 0) return mbr_entry_is_empty(ent);
    return lba_range_valid(disk, start, count);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, uint64_t len) {
    crc = ~crc;
    for (uint64_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static bool gpt_crc_ok(uint8_t *buf, uint32_t size, uint32_t crc_off, uint32_t expected) {
    uint32_t saved = rd_le32(buf + crc_off);
    wr_le32(buf + crc_off, 0);
    uint32_t actual = crc32_update(0, buf, size);
    wr_le32(buf + crc_off, saved);
    return actual == expected;
}

static bool ahci_read_gpt_header(struct ahci_disk *disk, uint64_t lba, uint8_t *hdr) {
    if (!disk || !hdr || lba >= disk->sectors) return false;
    uint64_t off;
    if (!ahci_size_mul_ok(lba, disk->sector_size, &off)) return false;
    if (ahci_read_dev(disk, off, disk->sector_size, hdr) != disk->sector_size) return false;
    if (memcmp(hdr, "EFI PART", 8) != 0) return false;

    uint32_t revision = rd_le32(hdr + 8);
    uint32_t header_size = rd_le32(hdr + 12);
    uint32_t header_crc = rd_le32(hdr + 16);
    uint64_t current_lba = rd_le64(hdr + 24);
    uint64_t backup_lba = rd_le64(hdr + 32);
    uint64_t first_usable = rd_le64(hdr + 40);
    uint64_t last_usable = rd_le64(hdr + 48);
    uint64_t entries_lba = rd_le64(hdr + 72);
    uint32_t entry_count = rd_le32(hdr + 80);
    uint32_t entry_size = rd_le32(hdr + 84);

    if (revision != 0x00010000u) return false;
    if (header_size < 92 || header_size > disk->sector_size) return false;
    for (uint32_t i = header_size; i < disk->sector_size; i++) if (hdr[i]) return false;
    if (!gpt_crc_ok(hdr, header_size, 16, header_crc)) return false;
    if (current_lba != lba || backup_lba >= disk->sectors || backup_lba == current_lba) return false;
    if (first_usable > last_usable || last_usable >= disk->sectors) return false;
    if (entry_count == 0 || entry_count > AHCI_MAX_GPT_PARTITION_SCAN ||
        entry_size < 128 || entry_size > AHCI_GPT_MAX_ENTRY_SIZE || (entry_size & 7u) != 0) return false;
    if (u64_add_overflows((uint64_t)entry_count, entry_size - 1)) return false;

    uint64_t entries_bytes = (uint64_t)entry_count * entry_size;
    uint64_t entries_sectors = (entries_bytes + disk->sector_size - 1) / disk->sector_size;
    if (!byte_range_for_lba_valid(disk, entries_lba, entries_sectors)) return false;
    if (lba_ranges_overlap(entries_lba, entries_sectors, current_lba, 1) ||
        lba_ranges_overlap(entries_lba, entries_sectors, backup_lba, 1) ||
        lba_ranges_overlap(entries_lba, entries_sectors, first_usable, last_usable - first_usable + 1))
        return false;
    return true;
}

static bool ahci_has_gpt(struct ahci_disk *disk) {
    uint8_t *hdr = (uint8_t *)kmalloc(disk->sector_size);
    if (!hdr) return false;
    bool ok = ahci_read_gpt_header(disk, 1, hdr);
    kfree(hdr);
    return ok;
}

static bool ahci_gpt_protective_mbr_valid(struct ahci_disk *disk) {
    uint8_t *mbr = (uint8_t *)kmalloc(disk->sector_size);
    if (!mbr) return false;
    if (ahci_read_dev(disk, 0, disk->sector_size, mbr) != disk->sector_size) { kfree(mbr); return false; }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) { kfree(mbr); return false; }

    int protective = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *ent = mbr + 446 + (i * 16);
        if (!mbr_entry_valid(disk, ent)) { kfree(mbr); return false; }
        if (ent[4] == 0xEE) {
            uint32_t start = rd_le32(ent + 8);
            uint32_t count = rd_le32(ent + 12);
            uint32_t expected = disk->sectors - 1 > UINT32_MAX ? UINT32_MAX : (uint32_t)(disk->sectors - 1);
            if (start != 1 || count != expected) { kfree(mbr); return false; }
            protective++;
        } else if (ent[4] != 0) { kfree(mbr); return false; }
    }
    bool ok = protective == 1;
    kfree(mbr);
    return ok;
}

static bool ahci_mbr_has_protective(struct ahci_disk *disk) {
    uint8_t *mbr = (uint8_t *)kmalloc(disk->sector_size);
    if (!mbr) return false;
    if (ahci_read_dev(disk, 0, disk->sector_size, mbr) != disk->sector_size) { kfree(mbr); return false; }
    bool protective = false;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            if (mbr[446 + i * 16 + 4] == 0xEE) { protective = true; break; }
        }
    }
    kfree(mbr);
    return protective;
}

static void ahci_scan_gpt(struct ahci_disk *disk) {
    uint8_t *hdr = (uint8_t *)kmalloc(disk->sector_size);
    if (!hdr) return;
    if (!ahci_gpt_protective_mbr_valid(disk)) {
        klog(LOG_WARN, "AHCI: invalid protective MBR on /dev/%s\n", disk->name);
        kfree(hdr);
        return;
    }
    if (!ahci_read_gpt_header(disk, 1, hdr)) {
        klog(LOG_WARN, "AHCI: invalid GPT header on /dev/%s\n", disk->name);
        kfree(hdr);
        return;
    }

    uint64_t backup_lba = rd_le64(hdr + 32);
    uint8_t *backup_hdr = (uint8_t *)kmalloc(disk->sector_size);
    if (!backup_hdr) { kfree(hdr); return; }
    if (!ahci_read_gpt_header(disk, backup_lba, backup_hdr)) {
        klog(LOG_WARN, "AHCI: invalid backup GPT header on /dev/%s\n", disk->name);
        kfree(backup_hdr);
        kfree(hdr);
        return;
    }
    if (rd_le64(backup_hdr + 32) != 1 || rd_le64(backup_hdr + 40) != rd_le64(hdr + 40) ||
        rd_le64(backup_hdr + 48) != rd_le64(hdr + 48) || memcmp(backup_hdr + 56, hdr + 56, 16) != 0 ||
        rd_le32(backup_hdr + 80) != rd_le32(hdr + 80) || rd_le32(backup_hdr + 84) != rd_le32(hdr + 84) ||
        rd_le32(backup_hdr + 88) != rd_le32(hdr + 88)) {
        klog(LOG_WARN, "AHCI: inconsistent GPT headers on /dev/%s\n", disk->name);
        kfree(backup_hdr);
        kfree(hdr);
        return;
    }

    uint64_t entries_lba = rd_le64(hdr + 72);
    uint32_t entry_count = rd_le32(hdr + 80);
    uint32_t entry_size = rd_le32(hdr + 84);
    uint32_t entries_crc = rd_le32(hdr + 88);
    uint64_t first_usable = rd_le64(hdr + 40);
    uint64_t last_usable = rd_le64(hdr + 48);
    uint64_t entries_bytes = (uint64_t)entry_count * entry_size;
    uint64_t entries_sectors = (entries_bytes + disk->sector_size - 1) / disk->sector_size;
    if (rd_le64(backup_hdr + 72) + entries_sectors != backup_lba) {
        klog(LOG_WARN, "AHCI: invalid backup GPT entry-array placement on /dev/%s\n", disk->name);
        kfree(backup_hdr);
        kfree(hdr);
        return;
    }

    kfree(backup_hdr);
    uint8_t *entries = (uint8_t *)kmalloc(entries_bytes);
    if (!entries) { kfree(hdr); return; }
    uint64_t entries_off;
    if (!ahci_size_mul_ok(entries_lba, disk->sector_size, &entries_off) ||
        ahci_read_dev(disk, entries_off, entries_bytes, entries) != entries_bytes) {
        kfree(entries);
        kfree(hdr);
        return;
    }
    if (crc32_update(0, entries, entries_bytes) != entries_crc) {
        klog(LOG_WARN, "AHCI: invalid GPT entry-array CRC on /dev/%s\n", disk->name);
        kfree(entries);
        kfree(hdr);
        return;
    }

    uint64_t *starts = (uint64_t *)kmalloc(entry_count * sizeof(uint64_t));
    uint64_t *counts = (uint64_t *)kmalloc(entry_count * sizeof(uint64_t));
    if (!starts || !counts) {
        if (starts) kfree(starts);
        if (counts) kfree(counts);
        kfree(entries);
        kfree(hdr);
        return;
    }
    uint32_t used = 0;

    for (uint32_t i = 0; i < entry_count; i++) {
        uint8_t *entry = entries + (uint64_t)i * entry_size;
        bool present = false;
        for (int j = 0; j < 16; j++) if (entry[j]) { present = true; break; }
        if (!present) continue;

        bool unique_guid_present = false;
        for (int j = 16; j < 32; j++) if (entry[j]) { unique_guid_present = true; break; }

        uint64_t first = rd_le64(entry + 32);
        uint64_t last = rd_le64(entry + 40);
        if (!unique_guid_present || last < first || first < first_usable || last > last_usable) {
            klog(LOG_WARN, "AHCI: invalid GPT partition %d on /dev/%s\n", i + 1, disk->name);
            kfree(starts);
            kfree(counts);
            kfree(entries);
            kfree(hdr);
            return;
        }

        uint64_t count = last - first + 1;
        for (uint32_t j = 0; j < used; j++) {
            if (lba_ranges_overlap(first, count, starts[j], counts[j])) {
                klog(LOG_WARN, "AHCI: overlapping GPT partitions on /dev/%s\n", disk->name);
                kfree(starts);
                kfree(counts);
                kfree(entries);
                kfree(hdr);
                return;
            }
        }
        starts[used] = first;
        counts[used] = count;
        used++;
    }

    for (uint32_t i = 0; i < used; i++) ahci_register_partition(disk, starts[i], counts[i], (int)i + 1);
    kfree(starts);
    kfree(counts);
    kfree(entries);
    kfree(hdr);
}

static bool mbr_type_is_extended(uint8_t type) {
    return type == 0x05 || type == 0x0F || type == 0x85;
}

static bool ahci_mbr_has_overlap(uint64_t starts[4], uint64_t counts[4], int used) {
    for (int i = 0; i < used; i++) {
        for (int j = i + 1; j < used; j++) {
            if (lba_ranges_overlap(starts[i], counts[i], starts[j], counts[j])) return true;
        }
    }
    return false;
}

static bool mbr_entry_basic_valid(const uint8_t *ent) {
    if (!mbr_boot_valid(ent[0])) return false;
    if (ent[4] == 0 || rd_le32(ent + 12) == 0) return mbr_entry_is_empty(ent);
    return true;
}

static void ahci_scan_ebr(struct ahci_disk *disk, uint32_t base_lba, uint32_t base_count, int *part_no) {
    uint32_t ebr_lba = base_lba;
    uint64_t prev_ebr = 0;
    uint8_t *ebr = (uint8_t *)kmalloc(disk->sector_size);
    if (!ebr) return;
    for (int depth = 0; depth < AHCI_MAX_EBR_CHAIN_SCAN && ebr_lba != 0; depth++) {
        if (!lba_range_valid(disk, ebr_lba, 1)) break;
        if (ebr_lba < base_lba || ebr_lba >= (uint64_t)base_lba + base_count) break;
        if (prev_ebr && ebr_lba <= prev_ebr) break;
        prev_ebr = ebr_lba;

        uint64_t ebr_off;
        if (!ahci_size_mul_ok(ebr_lba, disk->sector_size, &ebr_off) ||
            ahci_read_dev(disk, ebr_off, disk->sector_size, ebr) != disk->sector_size) break;
        if (ebr[510] != 0x55 || ebr[511] != 0xAA) break;

        for (int i = 0; i < 4; i++) {
            uint8_t *ent = ebr + 446 + i * 16;
            if (!mbr_entry_basic_valid(ent)) goto out;
            if (i >= 2 && !mbr_entry_is_empty(ent)) goto out;
        }

        uint8_t *logical = ebr + 446;
        uint8_t logical_type = logical[4];
        uint32_t logical_rel = rd_le32(logical + 8);
        uint32_t logical_count = rd_le32(logical + 12);
        if (logical_type != 0 && logical_count != 0 && !mbr_type_is_extended(logical_type)) {
            uint64_t start = (uint64_t)ebr_lba + logical_rel;
            if (logical_rel == 0 || !lba_range_valid(disk, start, logical_count) ||
                start < base_lba || start + logical_count > (uint64_t)base_lba + base_count) goto out;
            if (ahci_register_partition(disk, start, logical_count, *part_no) != 0) goto out;
            (*part_no)++;
        }

        uint8_t *next = ebr + 446 + 16;
        uint8_t next_type = next[4];
        uint32_t next_rel = rd_le32(next + 8);
        uint32_t next_count = rd_le32(next + 12);
        if (next_type == 0 && next_count == 0 && next_rel == 0) break;
        if (!mbr_type_is_extended(next_type) || next_count == 0 || next_rel == 0) goto out;
        if (next_rel >= base_count || u64_add_overflows(next_rel, next_count) || next_rel + next_count > base_count) goto out;
        ebr_lba = base_lba + next_rel;
    }
out:
    kfree(ebr);
}

static void ahci_scan_mbr(struct ahci_disk *disk) {
    uint8_t *mbr = (uint8_t *)kmalloc(disk->sector_size);
    if (!mbr) return;
    if (ahci_read_dev(disk, 0, disk->sector_size, mbr) != disk->sector_size) { kfree(mbr); return; }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) { kfree(mbr); return; }

    uint64_t starts[4];
    uint64_t counts[4];
    int used = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t *ent = mbr + 446 + (i * 16);
        if (!mbr_entry_valid(disk, ent)) {
            klog(LOG_WARN, "AHCI: invalid MBR entry on /dev/%s\n", disk->name);
            kfree(mbr);
            return;
        }
        uint8_t type = ent[4];
        uint32_t start = rd_le32(ent + 8);
        uint32_t count = rd_le32(ent + 12);
        if (type == 0 || count == 0) continue;
        starts[used] = start;
        counts[used] = count;
        used++;
    }
    if (ahci_mbr_has_overlap(starts, counts, used)) {
        klog(LOG_WARN, "AHCI: overlapping MBR partitions on /dev/%s\n", disk->name);
        kfree(mbr);
        return;
    }

    int next_logical = 5;
    for (int i = 0; i < 4; i++) {
        uint8_t *ent = mbr + 446 + (i * 16);
        uint8_t type = ent[4];
        uint32_t start = rd_le32(ent + 8);
        uint32_t count = rd_le32(ent + 12);
        if (type == 0 || count == 0) continue;
        if (mbr_type_is_extended(type)) {
            ahci_scan_ebr(disk, start, count, &next_logical);
            continue;
        }
        ahci_register_partition(disk, start, count, i + 1);
    }
    kfree(mbr);
}

static void ahci_scan_partitions(struct ahci_disk *disk) {
    if (!disk || disk->sectors < 2) return;
    if (ahci_has_gpt(disk)) ahci_scan_gpt(disk);
    else if (ahci_mbr_has_protective(disk)) klog(LOG_WARN, "AHCI: protective MBR without valid GPT on /dev/%s\n", disk->name);
    else ahci_scan_mbr(disk);
}

static int ahci_rebase_port(struct ahci_controller *ctrl, struct ahci_disk *disk) {
    int port = disk->port_no;
    ahci_stop_port(ctrl, port);

    void *dma = kmalloc(AHCI_PORT_DMA_SIZE + 4096);
    if (!dma) return -1;
    disk->port_dma_raw = dma;
    uintptr_t dma_aligned = ((uintptr_t)dma + 4095) & ~4095ULL;
    memset((void *)dma_aligned, 0, AHCI_PORT_DMA_SIZE);

    uint64_t phys = ahci_v2p((void *)dma_aligned);
    if (!ahci_dma_range_supported(ctrl, phys, AHCI_PORT_DMA_SIZE)) {
        kfree(dma);
        disk->port_dma_raw = 0;
        return -1;
    }
    *ahci_port_reg(ctrl, port, AHCI_PxCLB) = (uint32_t)phys;
    *ahci_port_reg(ctrl, port, AHCI_PxCLBU) = (uint32_t)(phys >> 32);
    *ahci_port_reg(ctrl, port, AHCI_PxFB) = (uint32_t)(phys + 1024);
    *ahci_port_reg(ctrl, port, AHCI_PxFBU) = (uint32_t)((phys + 1024) >> 32);

    struct hba_cmd_header *cmdlist = (struct hba_cmd_header *)dma_aligned;
    for (int i = 0; i < 32; i++) {
        uint64_t ctba = phys + 2048 + ((uint64_t)i * 256);
        cmdlist[i].prdtl = 1;
        cmdlist[i].ctba = (uint32_t)ctba;
        cmdlist[i].ctbau = (uint32_t)(ctba >> 32);
    }

    *ahci_port_reg(ctrl, port, AHCI_PxSERR) = 0xFFFFFFFFu;
    *ahci_port_reg(ctrl, port, AHCI_PxIS) = 0xFFFFFFFFu;
    *ahci_port_reg(ctrl, port, AHCI_PxIE) = AHCI_PxIE_DEFAULT;
    return ahci_start_port(ctrl, port);
}

static int ahci_probe_port(struct ahci_controller *ctrl, int port) {
    uint32_t ssts = *ahci_port_reg(ctrl, port, AHCI_PxSSTS);
    uint8_t det = ssts & 0x0F;
    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint32_t sig = *ahci_port_reg(ctrl, port, AHCI_PxSIG);

    if (det != SATA_DEV_PRESENT || ipm != SATA_IPM_ACTIVE) return -1;
    if (sig != AHCI_SIG_ATA) {
        klog(LOG_INFO, "AHCI: ignoring non-ATA device on port %d sig=%x\n", port, sig);
        return -1;
    }
    struct ahci_disk *disk = (struct ahci_disk *)kmalloc(sizeof(*disk));
    if (!disk) return -1;
    memset(disk, 0, sizeof(*disk));
    disk->ctrl = ctrl;
    disk->port_no = port;
    disk->index = ahci_next_disk_index++;
    disk->sector_size = AHCI_LEGACY_SECTOR_SIZE;
    spinlock_init(&disk->lock);
    spinlock_init(&disk->bounce_lock);

    if (ahci_rebase_port(ctrl, disk) != 0) {
        kfree(disk);
        return -1;
    }

    disk->bounce_raw = kmalloc(AHCI_BOUNCE_SIZE + 4096);
    disk->bounce = (uint8_t *)disk->bounce_raw;
    if (!disk->bounce_raw) {
        if (disk->port_dma_raw) kfree(disk->port_dma_raw);
        kfree(disk);
        return -1;
    }
    disk->bounce = (uint8_t *)(((uintptr_t)disk->bounce + 4095) & ~4095ULL);
    if (!ahci_dma_range_supported(ctrl, ahci_v2p(disk->bounce), AHCI_BOUNCE_SIZE)) {
        if (disk->bounce_raw) kfree(disk->bounce_raw);
        if (disk->port_dma_raw) kfree(disk->port_dma_raw);
        kfree(disk);
        return -1;
    }

    if (ahci_identify(disk) != 0) {
        klog(LOG_WARN, "AHCI: IDENTIFY failed on port %d\n", port);
        if (disk->bounce_raw) kfree(disk->bounce_raw);
        if (disk->port_dma_raw) kfree(disk->port_dma_raw);
        kfree(disk);
        return -1;
    }

    if (ahci_make_sd_name(disk->index, disk->name, sizeof(disk->name)) != 0) {
        klog(LOG_ERR, "AHCI: failed to allocate disk name for index %x:%x\n",
             (uint32_t)(disk->index >> 32), (uint32_t)disk->index);
        if (disk->bounce_raw) kfree(disk->bounce_raw);
        if (disk->port_dma_raw) kfree(disk->port_dma_raw);
        kfree(disk);
        return -1;
    }

    uint64_t disk_bytes;
    if (!ahci_size_mul_ok(disk->sectors, disk->sector_size, &disk_bytes) ||
        devfs_register_block(disk->name, &ahci_disk_ops, disk, disk_bytes) != 0) {
        if (disk->bounce_raw) kfree(disk->bounce_raw);
        if (disk->port_dma_raw) kfree(disk->port_dma_raw);
        kfree(disk);
        return -1;
    }

    ctrl->ports[port] = disk;
    klog(LOG_INFO, "AHCI: registered block /dev/%s port=%d sectors=%x:%x sector_size=%d\n", disk->name, port,
         (uint32_t)(disk->sectors >> 32), (uint32_t)disk->sectors, disk->sector_size);
    ahci_scan_partitions(disk);
    return 0;
}

static int ahci_controller_prepare(struct ahci_controller *ctrl) {
    uint32_t cap2 = *ahci_reg(ctrl, AHCI_CAP2);
    *ahci_reg(ctrl, AHCI_GHC) |= AHCI_GHC_AE;

    if ((cap2 & AHCI_CAP2_BOH) != 0) {
        volatile uint32_t *bohc = ahci_reg(ctrl, AHCI_BOHC);
        uint32_t v = *bohc;
        if ((v & AHCI_BOHC_BOS) != 0) {
            *bohc = v | AHCI_BOHC_OOS;
            for (uint32_t i = 0; i < AHCI_RESET_TIMEOUT; i++) {
                v = *bohc;
                if ((v & (AHCI_BOHC_BOS | AHCI_BOHC_BB)) == 0) break;
                asm volatile("pause");
            }
            if ((*bohc & (AHCI_BOHC_BOS | AHCI_BOHC_BB)) != 0) return -1;
        }
    }

    return 0;
}

static int ahci_probe(struct device *dev) {
    struct pci_device *pdev = (struct pci_device *)dev->bus_data;
    struct pci_resource *bar = &pdev->bars[5];
    if (bar->type != PCI_BAR_TYPE_MMIO32 && bar->type != PCI_BAR_TYPE_MMIO64) {
        klog(LOG_ERR, "AHCI: controller has no MMIO ABAR\n");
        return -1;
    }

    struct ahci_controller *ctrl = (struct ahci_controller *)kmalloc(sizeof(*ctrl));
    if (!ctrl) return -1;
    memset(ctrl, 0, sizeof(*ctrl));

    ctrl->abar_phys = bar->base;
    ctrl->abar_size = bar->size ? bar->size : 4096;
    ctrl->abar = (volatile uint8_t *)vmm_mmio_map(ctrl->abar_phys, ctrl->abar_size);
    if (!ctrl->abar) {
        kfree(ctrl);
        return -1;
    }

    pci_enable_bus_mastering(pdev->segment, pdev->bus, pdev->slot, pdev->func);

    if (ahci_controller_prepare(ctrl) != 0) {
        klog(LOG_ERR, "AHCI: BIOS/OS handoff failed\n");
        vmm_mmio_unmap((void *)ctrl->abar, ctrl->abar_size);
        kfree(ctrl);
        return -1;
    }

    ctrl->irq = pci_read8(pdev->segment, pdev->bus, pdev->slot, pdev->func, 0x3C);

    uint32_t ghc = *ahci_reg(ctrl, AHCI_GHC);
    ghc |= AHCI_GHC_AE;
    ghc &= ~AHCI_GHC_IE;
    *ahci_reg(ctrl, AHCI_GHC) = ghc;
    ahci_clear_interrupts(ctrl);

    uint32_t pi = *ahci_reg(ctrl, AHCI_PI);
    uint32_t cap = *ahci_reg(ctrl, AHCI_CAP);
    ctrl->command_slots = (uint8_t)(((cap & AHCI_CAP_NCS_MASK) >> AHCI_CAP_NCS_SHIFT) + 1);
    ctrl->supports_64bit_dma = (cap & AHCI_CAP_S64A) != 0;
    int max_ports = (int)((cap & 0x1F) + 1);
    if (max_ports > 32) max_ports = 32;

    klog(LOG_INFO, "AHCI: controller %04x:%04x ports implemented=%x\n", pdev->vendor_id,
         pdev->device_id, pi);

    int disks_found = 0;
    for (int port = 0; port < max_ports; port++) {
        if ((pi & (1u << port)) && ahci_probe_port(ctrl, port) == 0) disks_found++;
    }

    ahci_clear_interrupts(ctrl);
    if (disks_found > 0) {
        if (ctrl->irq != 0 && ctrl->irq != AHCI_IRQ_NONE) {
            if (request_irq(ctrl->irq, ahci_irq_handler, IRQ_AFFINITY_ALL, ctrl) == 0) {
                ctrl->irq_registered = true;
                *ahci_reg(ctrl, AHCI_GHC) |= AHCI_GHC_IE;
                klog(LOG_INFO, "AHCI: using legacy IRQ %d\n", ctrl->irq);
            } else {
                klog(LOG_WARN, "AHCI: failed to register IRQ %d, falling back to polling\n",
                     ctrl->irq);
            }
        } else {
            klog(LOG_WARN, "AHCI: PCI interrupt line is not routed, falling back to polling\n");
        }
    } else {
        klog(LOG_INFO, "AHCI: no ATA disks found on this controller\n");
    }

    dev->driver_data = ctrl;
    return 0;
}

static void ahci_remove(struct device *dev) {
    struct ahci_controller *ctrl = (struct ahci_controller *)dev->driver_data;
    if (!ctrl) return;
    for (int i = 0; i < 32; i++) {
        if (ctrl->ports[i]) {
            struct ahci_partition *part = ctrl->ports[i]->partitions;
            while (part) {
                struct ahci_partition *next = part->next;
                devfs_unregister(part->name);
                if (ahci_part_count > 0) ahci_part_count--;
                kfree(part);
                part = next;
            }
            devfs_unregister(ctrl->ports[i]->name);
            if (ctrl->ports[i]->bounce_raw) kfree(ctrl->ports[i]->bounce_raw);
            if (ctrl->ports[i]->port_dma_raw) kfree(ctrl->ports[i]->port_dma_raw);
            kfree(ctrl->ports[i]);
        }
    }
    if (ctrl->irq_registered) {
        *ahci_reg(ctrl, AHCI_GHC) &= ~AHCI_GHC_IE;
        free_irq(ctrl->irq, ahci_irq_handler);
    }
    vmm_mmio_unmap((void *)ctrl->abar, ctrl->abar_size);
    kfree(ctrl);
}

static struct pci_device_id ahci_ids[] = {
    {PCI_ANY_ID, PCI_ANY_ID, 0x01, 0x06, PCI_ANY_CLASS},
    {0},
};

static struct driver ahci_driver = {
    .name = "ahci",
    .id_table = ahci_ids,
    .probe = ahci_probe,
    .remove = ahci_remove,
};

static int ahci_init(void) {
    ahci_driver.bus = find_bus("pci");
    if (!ahci_driver.bus) return -1;
    driver_register(&ahci_driver);
    return 0;
}

MODULE_INFO("ahci", ahci_init, NULL, "pci_bus");
