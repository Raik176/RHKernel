#include "usb_core.h"

#include "mod/device.h"
#include "mod/heap.h"
#include "mod/logging.h"
#include "mod/module.h"
#include "mod/scheduler.h"
#include "smp/lock.h"
#include "string.h"

#define USB_CLASS_MASS_STORAGE 0x08u
#define USB_MSC_SUBCLASS_UFI   0x04u
#define USB_MSC_SUBCLASS_SFF8070I 0x05u
#define USB_MSC_SUBCLASS_SCSI  0x06u
#define USB_MSC_PROTOCOL_CBI   0x00u
#define USB_MSC_PROTOCOL_CBI_NO_INT 0x01u
#define USB_MSC_PROTOCOL_BOT   0x50u
#define USB_MSC_PROTOCOL_UAS   0x62u

#define USB_MSC_REQ_RESET       0xffu
#define USB_MSC_REQ_GET_MAX_LUN 0xfeu
#define USB_FEATURE_ENDPOINT_HALT 0u

#define USB_MSC_CBW_SIGNATURE 0x43425355u
#define USB_MSC_CSW_SIGNATURE 0x53425355u
#define USB_MSC_CBW_LEN 31u
#define USB_MSC_CSW_LEN 13u
#define USB_MSC_CDB_MAX 16u
#define USB_MSC_IO_MAX 16384u
#define USB_MSC_TIMEOUT_MS 5000u
#define USB_MSC_READY_RETRIES 20u
#define USB_MSC_NAME_MAX 32u
#define USB_MSC_PART_NAME_MAX 40u
#define USB_MSC_MAX_GPT_PARTS 256u
#define USB_MSC_GPT_ENTRY_MAX 1024u
#define USB_MSC_UUID_STR_LEN 37u

#define SCSI_TEST_UNIT_READY 0x00u
#define SCSI_REQUEST_SENSE   0x03u
#define SCSI_INQUIRY         0x12u
#define SCSI_READ_CAPACITY10 0x25u
#define SCSI_READ10          0x28u
#define SCSI_WRITE10         0x2au
#define SCSI_SERVICE_ACTION_IN16 0x9eu
#define SCSI_READ_FORMAT_CAPACITIES 0x23u
#define SCSI_READ16          0x88u
#define SCSI_WRITE16         0x8au
#define SCSI_SAI_READ_CAPACITY16 0x10u

#define USB_MSC_TRANSPORT_BOT 1u
#define USB_MSC_TRANSPORT_CBI 2u
#define USB_MSC_TRANSPORT_UAS 3u

#define UAS_IU_COMMAND 0x01u
#define UAS_IU_STATUS  0x03u
#define UAS_STATUS_GOOD 0x00u

#define USB_MSC_CBI_ADSC 0x00u

struct usb_msc_cbw {
    uint32_t sig;
    uint32_t tag;
    uint32_t data_len;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_len;
    uint8_t cb[16];
} __attribute__((packed));

struct usb_msc_csw {
    uint32_t sig;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} __attribute__((packed));

struct usb_msc_disk;

struct usb_msc_partition {
    struct usb_msc_disk *disk;
    uint64_t start_lba;
    uint64_t sectors;
    char name[USB_MSC_PART_NAME_MAX];
    char scheme[16];
    char type[32];
    struct usb_msc_partition *next;
};

struct usb_msc_disk {
    struct usb_interface *intf;
    struct usb_device *dev;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint8_t intr_in;
    uint8_t uas_cmd;
    uint8_t uas_status;
    uint8_t uas_data_in;
    uint8_t uas_data_out;
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;
    uint16_t intr_in_mps;
    uint16_t uas_cmd_mps;
    uint16_t uas_status_mps;
    uint16_t uas_data_in_mps;
    uint16_t uas_data_out_mps;
    uint8_t lun;
    uint8_t max_lun;
    uint32_t tag;
    uint64_t sectors;
    uint32_t sector_size;
    uint8_t use_16;
    uint8_t transport;
    uint8_t subclass;
    uint8_t protocol;
    volatile uint32_t busy;
    volatile uint32_t removed;
    char name[USB_MSC_NAME_MAX];
    struct usb_msc_partition *parts;
    struct usb_msc_disk *next;
};

static struct usb_msc_disk *disks;
static uint64_t next_disk_index;
static spinlock_t msc_lock;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static uint32_t rd32_be(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3]; }
static uint64_t rd64_be(const uint8_t *p) { return ((uint64_t)rd32_be(p) << 32) | rd32_be(p + 4); }

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }
static void wr32_be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void wr64_be(uint8_t *p, uint64_t v) { wr32_be(p, (uint32_t)(v >> 32)); wr32_be(p + 4, (uint32_t)v); }

static int size_mul_ok(uint64_t a, uint64_t b, uint64_t *out) {
    if (a && b > UINT64_MAX / a) return 0;
    if (out) *out = a * b;
    return 1;
}

static int lba_range_valid(struct usb_msc_disk *d, uint64_t start, uint64_t count) {
    if (!d || count == 0 || start >= d->sectors) return 0;
    return count <= d->sectors - start;
}

static int disk_byte_size(struct usb_msc_disk *d, uint64_t *out) {
    if (!d || !out || d->sector_size == 0) return 0;
    return size_mul_ok(d->sectors, d->sector_size, out);
}

static int lba_to_offset(struct usb_msc_disk *d, uint64_t lba, uint64_t add, uint64_t *out) {
    uint64_t base;
    if (!d || !out || !size_mul_ok(lba, d->sector_size, &base)) return 0;
    if (add > UINT64_MAX - base) return 0;
    *out = base + add;
    return 1;
}

static uint16_t ep_mps(const struct usb_endpoint_descriptor *ep) {
    return ep ? (uint16_t)(ep->wMaxPacketSize & 0x07ffu) : 0;
}

static int disk_name(uint64_t index, char *out, size_t out_size) {
    char suffix[USB_MSC_NAME_MAX - 4];
    size_t n = 0;
    if (!out || out_size < 5) return -1;
    for (;;) {
        if (n >= sizeof(suffix)) return -1;
        suffix[n++] = (char)('a' + (index % 26));
        if (index < 26) break;
        index = (index / 26) - 1;
    }
    if (3 + n + 1 > out_size) return -1;
    memcpy(out, "usb", 3);
    for (size_t i = 0; i < n; i++) out[3 + i] = suffix[n - 1 - i];
    out[3 + n] = 0;
    return 0;
}

static void lock_disk(struct usb_msc_disk *d) {
    while (__sync_lock_test_and_set(&d->busy, 1u)) kernel_yield();
}

static void unlock_disk(struct usb_msc_disk *d) {
    __sync_lock_release(&d->busy);
}

static int clear_halt(struct usb_msc_disk *d, uint8_t ep) {
    return usb_control_transfer(d->dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                                USB_REQ_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, ep, NULL, 0, 1000);
}

static void bot_reset(struct usb_msc_disk *d) {
    (void)usb_control_transfer(d->dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                               USB_MSC_REQ_RESET, 0, d->intf->desc.bInterfaceNumber, NULL, 0, 1000);
    clear_halt(d, d->bulk_in);
    clear_halt(d, d->bulk_out);
}

static int read_csw(struct usb_msc_disk *d, uint32_t tag) {
    struct usb_msc_csw csw;
    uint32_t actual = 0;
    memset(&csw, 0, sizeof(csw));
    if (usb_bulk_transfer(d->dev, d->bulk_in, d->bulk_in_mps, &csw, sizeof(csw), &actual, USB_MSC_TIMEOUT_MS) != 0 ||
        actual != sizeof(csw)) {
        bot_reset(d);
        return -1;
    }
    if (csw.sig != USB_MSC_CSW_SIGNATURE || csw.tag != tag || csw.status > 2) {
        bot_reset(d);
        return -1;
    }
    return csw.status == 0 ? 0 : -1;
}

static int bot_scsi_cmd(struct usb_msc_disk *d, const uint8_t *cdb, uint8_t cdb_len, void *data,
                         uint32_t len, int dir_in, uint32_t *actual_out) {
    if (actual_out) *actual_out = 0;
    if (!d || !cdb || cdb_len == 0 || cdb_len > USB_MSC_CDB_MAX) return -1;
    if (len && !data) return -1;

    struct usb_msc_cbw cbw;
    memset(&cbw, 0, sizeof(cbw));
    uint32_t tag = ++d->tag;
    if (tag == 0) tag = ++d->tag;
    cbw.sig = USB_MSC_CBW_SIGNATURE;
    cbw.tag = tag;
    cbw.data_len = len;
    cbw.flags = dir_in ? USB_DIR_IN : USB_DIR_OUT;
    cbw.lun = d->lun;
    cbw.cb_len = cdb_len;
    memcpy(cbw.cb, cdb, cdb_len);

    uint32_t actual = 0;
    if (usb_bulk_transfer(d->dev, d->bulk_out, d->bulk_out_mps, &cbw, USB_MSC_CBW_LEN, &actual, USB_MSC_TIMEOUT_MS) != 0 ||
        actual != USB_MSC_CBW_LEN) {
        bot_reset(d);
        return -1;
    }

    uint32_t data_actual = 0;
    if (len) {
        uint8_t ep = dir_in ? d->bulk_in : d->bulk_out;
        uint16_t mps = dir_in ? d->bulk_in_mps : d->bulk_out_mps;
        if (usb_bulk_transfer(d->dev, ep, mps, data, len, &data_actual, USB_MSC_TIMEOUT_MS) != 0) {
            bot_reset(d);
            return -1;
        }
    }

    if (read_csw(d, tag) != 0) return -1;
    if (actual_out) *actual_out = data_actual;
    return 0;
}


static int cbi_scsi_cmd(struct usb_msc_disk *d, const uint8_t *cdb, uint8_t cdb_len, void *data,
                        uint32_t len, int dir_in, uint32_t *actual_out) {
    if (actual_out) *actual_out = 0;
    if (!d || !cdb || cdb_len == 0 || cdb_len > USB_MSC_CDB_MAX) return -1;
    if (len && !data) return -1;
    if (usb_control_transfer(d->dev, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                             USB_MSC_CBI_ADSC, 0, d->intf->desc.bInterfaceNumber,
                             (void *)cdb, cdb_len, USB_MSC_TIMEOUT_MS) != 0) return -1;
    uint32_t data_actual = 0;
    if (len) {
        uint8_t ep = dir_in ? d->bulk_in : d->bulk_out;
        uint16_t mps = dir_in ? d->bulk_in_mps : d->bulk_out_mps;
        if (usb_bulk_transfer(d->dev, ep, mps, data, len, &data_actual, USB_MSC_TIMEOUT_MS) != 0) return -1;
    }
    if (d->protocol == USB_MSC_PROTOCOL_CBI && d->intr_in) {
        uint8_t status[2];
        uint32_t got = 0;
        memset(status, 0, sizeof(status));
        if (usb_interrupt_transfer(d->dev, d->intr_in, d->intr_in_mps, status, sizeof(status), &got, USB_MSC_TIMEOUT_MS) != 0 ||
            got < sizeof(status) || status[0] != 0) return -1;
    }
    if (actual_out) *actual_out = data_actual;
    return 0;
}

struct uas_command_iu {
    uint8_t iu_id;
    uint8_t rsvd1;
    uint16_t tag;
    uint8_t prio_attr;
    uint8_t rsvd5;
    uint8_t len;
    uint8_t rsvd7;
    uint8_t lun[8];
    uint8_t cdb[16];
} __attribute__((packed));

struct uas_status_iu {
    uint8_t iu_id;
    uint8_t rsvd1;
    uint16_t tag;
    uint16_t qualifier;
    uint8_t status;
    uint8_t rsvd7;
    uint16_t sense_len;
} __attribute__((packed));

static int uas_scsi_cmd(struct usb_msc_disk *d, const uint8_t *cdb, uint8_t cdb_len, void *data,
                        uint32_t len, int dir_in, uint32_t *actual_out) {
    if (actual_out) *actual_out = 0;
    if (!d || !cdb || cdb_len == 0 || cdb_len > USB_MSC_CDB_MAX) return -1;
    if (len && !data) return -1;
    struct uas_command_iu cmd;
    memset(&cmd, 0, sizeof(cmd));
    uint16_t tag = (uint16_t)(++d->tag & 0xffffu);
    if (tag == 0) tag = (uint16_t)(++d->tag & 0xffffu);
    cmd.iu_id = UAS_IU_COMMAND;
    cmd.tag = (uint16_t)((tag >> 8) | (tag << 8));
    cmd.len = cdb_len > 16 ? 0 : cdb_len;
    cmd.lun[1] = d->lun;
    memcpy(cmd.cdb, cdb, cdb_len);

    uint32_t actual = 0;
    if (usb_bulk_transfer(d->dev, d->uas_cmd, d->uas_cmd_mps, &cmd, sizeof(cmd), &actual, USB_MSC_TIMEOUT_MS) != 0 ||
        actual != sizeof(cmd)) return -1;
    uint32_t data_actual = 0;
    if (len) {
        uint8_t ep = dir_in ? d->uas_data_in : d->uas_data_out;
        uint16_t mps = dir_in ? d->uas_data_in_mps : d->uas_data_out_mps;
        if (!ep || usb_bulk_transfer(d->dev, ep, mps, data, len, &data_actual, USB_MSC_TIMEOUT_MS) != 0) return -1;
    }
    struct uas_status_iu st;
    memset(&st, 0, sizeof(st));
    actual = 0;
    if (usb_bulk_transfer(d->dev, d->uas_status, d->uas_status_mps, &st, sizeof(st), &actual, USB_MSC_TIMEOUT_MS) != 0 ||
        actual < 8 || st.iu_id != UAS_IU_STATUS) return -1;
    uint16_t st_tag = (uint16_t)((st.tag >> 8) | (st.tag << 8));
    if (st_tag != tag || st.status != UAS_STATUS_GOOD) return -1;
    if (actual_out) *actual_out = data_actual;
    return 0;
}

static int scsi_cmd(struct usb_msc_disk *d, const uint8_t *cdb, uint8_t cdb_len, void *data,
                    uint32_t len, int dir_in, uint32_t *actual_out) {
    if (!d) return -1;
    if (d->transport == USB_MSC_TRANSPORT_BOT) return bot_scsi_cmd(d, cdb, cdb_len, data, len, dir_in, actual_out);
    if (d->transport == USB_MSC_TRANSPORT_CBI) return cbi_scsi_cmd(d, cdb, cdb_len, data, len, dir_in, actual_out);
    if (d->transport == USB_MSC_TRANSPORT_UAS) return uas_scsi_cmd(d, cdb, cdb_len, data, len, dir_in, actual_out);
    return -1;
}

static void request_sense(struct usb_msc_disk *d) {
    uint8_t cdb[6];
    uint8_t sense[18];
    memset(cdb, 0, sizeof(cdb));
    memset(sense, 0, sizeof(sense));
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = sizeof(sense);
    (void)scsi_cmd(d, cdb, sizeof(cdb), sense, sizeof(sense), 1, NULL);
}

static int test_ready(struct usb_msc_disk *d) {
    uint8_t cdb[6];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_TEST_UNIT_READY;
    if (scsi_cmd(d, cdb, sizeof(cdb), NULL, 0, 0, NULL) == 0) return 0;
    request_sense(d);
    return -1;
}

static int read_capacity16(struct usb_msc_disk *d) {
    uint8_t cdb[16];
    uint8_t buf[32];
    uint32_t actual = 0;
    memset(cdb, 0, sizeof(cdb));
    memset(buf, 0, sizeof(buf));
    cdb[0] = SCSI_SERVICE_ACTION_IN16;
    cdb[1] = SCSI_SAI_READ_CAPACITY16;
    wr32_be(cdb + 10, sizeof(buf));
    if (scsi_cmd(d, cdb, sizeof(cdb), buf, sizeof(buf), 1, &actual) != 0 || actual < 12) return -1;
    uint64_t last = rd64_be(buf);
    uint32_t block = rd32_be(buf + 8);
    if (block == 0 || block > USB_MSC_IO_MAX || last == UINT64_MAX) return -1;
    d->sectors = last + 1;
    d->sector_size = block;
    d->use_16 = 1;
    return 0;
}


static int read_format_capacities(struct usb_msc_disk *d) {
    uint8_t cdb[10];
    uint8_t buf[252];
    uint32_t actual = 0;
    memset(cdb, 0, sizeof(cdb));
    memset(buf, 0, sizeof(buf));
    cdb[0] = SCSI_READ_FORMAT_CAPACITIES;
    cdb[7] = (uint8_t)(sizeof(buf) >> 8);
    cdb[8] = (uint8_t)sizeof(buf);
    if (scsi_cmd(d, cdb, sizeof(cdb), buf, sizeof(buf), 1, &actual) != 0 || actual < 12) return -1;
    uint32_t list_len = rd32_be(buf) & 0xffu;
    if (list_len < 8 || 4u + list_len > actual) return -1;
    uint8_t *cap = buf + 4;
    uint32_t blocks = rd32_be(cap);
    uint32_t block = ((uint32_t)cap[5] << 16) | ((uint32_t)cap[6] << 8) | cap[7];
    if (blocks == 0 || block < 512 || block > USB_MSC_IO_MAX) return -1;
    d->sectors = blocks;
    d->sector_size = block;
    d->use_16 = 0;
    return 0;
}

static int read_capacity10(struct usb_msc_disk *d) {
    uint8_t cdb[10];
    uint8_t buf[8];
    uint32_t actual = 0;
    memset(cdb, 0, sizeof(cdb));
    memset(buf, 0, sizeof(buf));
    cdb[0] = SCSI_READ_CAPACITY10;
    if (scsi_cmd(d, cdb, sizeof(cdb), buf, sizeof(buf), 1, &actual) != 0 || actual != sizeof(buf)) return -1;
    uint32_t last = rd32_be(buf);
    uint32_t block = rd32_be(buf + 4);
    if (block == 0 || block > USB_MSC_IO_MAX || last == UINT32_MAX) return -1;
    d->sectors = (uint64_t)last + 1u;
    d->sector_size = block;
    d->use_16 = 0;
    return 0;
}

static int scsi_rw(struct usb_msc_disk *d, uint64_t lba, uint32_t sectors, int write, void *buf) {
    if (!d || !buf || sectors == 0 || sectors > USB_MSC_IO_MAX / d->sector_size) return -1;
    if (!lba_range_valid(d, lba, sectors)) return -1;
    uint32_t bytes = sectors * d->sector_size;
    if (d->use_16 || lba > UINT32_MAX || sectors > 0xffffu) {
        uint8_t cdb[16];
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = write ? SCSI_WRITE16 : SCSI_READ16;
        wr64_be(cdb + 2, lba);
        wr32_be(cdb + 10, sectors);
        uint32_t actual = 0;
        if (scsi_cmd(d, cdb, sizeof(cdb), buf, bytes, !write, &actual) != 0) return -1;
        return write || actual == bytes ? 0 : -1;
    }
    uint8_t cdb[10];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = write ? SCSI_WRITE10 : SCSI_READ10;
    wr32_be(cdb + 2, (uint32_t)lba);
    cdb[7] = (uint8_t)(sectors >> 8);
    cdb[8] = (uint8_t)sectors;
    uint32_t actual = 0;
    if (scsi_cmd(d, cdb, sizeof(cdb), buf, bytes, !write, &actual) != 0) return -1;
    return write || actual == bytes ? 0 : -1;
}

static uint64_t disk_io(struct usb_msc_disk *d, uint64_t offset, uint64_t size, uint8_t *buffer, int write) {
    if (!d || !buffer || size == 0 || d->removed) return 0;
    uint64_t disk_bytes;
    if (!disk_byte_size(d, &disk_bytes) || offset >= disk_bytes) return 0;
    if (size > disk_bytes - offset) size = disk_bytes - offset;

    const uint32_t sector_size = d->sector_size;
    const uint32_t max_sectors = USB_MSC_IO_MAX / sector_size;
    if (max_sectors == 0) return 0;
    uint8_t *scratch = NULL;
    if ((offset % sector_size) || (size % sector_size)) {
        scratch = (uint8_t *)kmalloc(sector_size);
        if (!scratch) return 0;
    }

    lock_disk(d);
    if (d->removed) { unlock_disk(d); if (scratch) kfree(scratch); return 0; }
    uint64_t done = 0;
    while (done < size) {
        uint64_t abs = offset + done;
        uint64_t lba = abs / sector_size;
        uint32_t in_sector = (uint32_t)(abs % sector_size);
        uint64_t left = size - done;
        uint32_t sectors;
        if (in_sector || left < sector_size) {
            sectors = 1;
        } else {
            uint64_t whole = left / sector_size;
            sectors = whole < max_sectors ? (uint32_t)whole : max_sectors;
            if (sectors == 0) sectors = 1;
        }
        uint64_t chunk = (uint64_t)sectors * sector_size - in_sector;
        if (chunk > left) chunk = left;

        if (!in_sector && chunk == (uint64_t)sectors * sector_size) {
            if (scsi_rw(d, lba, sectors, write, buffer + done) != 0) break;
            done += chunk;
            continue;
        }

        if (write || in_sector || chunk != sector_size) {
            if (scsi_rw(d, lba, 1, 0, scratch) != 0) break;
        }
        if (write) {
            memcpy(scratch + in_sector, buffer + done, chunk);
            if (scsi_rw(d, lba, 1, 1, scratch) != 0) break;
        } else {
            memcpy(buffer + done, scratch + in_sector, chunk);
        }
        done += chunk;
    }
    unlock_disk(d);
    if (scratch) kfree(scratch);
    return done;
}

static uint64_t disk_read(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return disk_io((struct usb_msc_disk *)priv, offset, size, buffer, 0);
}

static uint64_t disk_write(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return disk_io((struct usb_msc_disk *)priv, offset, size, buffer, 1);
}

static void msc_copy_text(char *dst, uint64_t cap, const char *src) {
    if (!dst || !cap) return;
    uint64_t i = 0;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int disk_info(void *priv, struct device_info *out) {
    struct usb_msc_disk *d = (struct usb_msc_disk *)priv;
    if (!d || !out || !d->sector_size) return -1;
    out->version = DEVICE_INFO_VERSION;
    out->kind = DEVICE_INFO_KIND_BLOCK;
    out->logical_block_size = d->sector_size;
    out->physical_block_size = d->sector_size;
    out->block_count = d->sectors;
    if (!disk_byte_size(d, &out->size_bytes)) return -1;
    out->max_size_bytes = out->size_bytes;
    out->max_transfer_bytes = USB_MSC_IO_MAX;
    out->flags = DEVICE_INFO_FLAG_REMOVABLE;
    msc_copy_text(out->driver, sizeof(out->driver), "usb_mass_storage");
    msc_copy_text(out->media_type, sizeof(out->media_type), "usb");
    msc_copy_text(out->type, sizeof(out->type), "disk");
    return 0;
}

static int part_info(void *priv, struct device_info *out) {
    struct usb_msc_partition *p = (struct usb_msc_partition *)priv;
    if (!p || !p->disk || !out || !p->disk->sector_size) return -1;
    out->version = DEVICE_INFO_VERSION;
    out->kind = DEVICE_INFO_KIND_PARTITION;
    out->logical_block_size = p->disk->sector_size;
    out->physical_block_size = p->disk->sector_size;
    out->block_count = p->sectors;
    if (!size_mul_ok(p->sectors, p->disk->sector_size, &out->size_bytes)) return -1;
    out->max_size_bytes = out->size_bytes;
    out->max_transfer_bytes = USB_MSC_IO_MAX;
    out->start_lba = p->start_lba;
    if (!disk_byte_size(p->disk, &out->parent_size_bytes)) return -1;
    out->flags = DEVICE_INFO_FLAG_REMOVABLE;
    msc_copy_text(out->driver, sizeof(out->driver), "usb_mass_storage");
    msc_copy_text(out->media_type, sizeof(out->media_type), "usb-partition");
    msc_copy_text(out->scheme, sizeof(out->scheme), p->scheme[0] ? p->scheme : "mbr");
    msc_copy_text(out->type, sizeof(out->type), p->type[0] ? p->type : "partition");
    msc_copy_text(out->parent, sizeof(out->parent), p->disk->name);
    return 0;
}

static struct device_ops disk_ops = {.read = disk_read, .write = disk_write, .info = disk_info};

static uint64_t part_io(struct usb_msc_partition *p, uint64_t offset, uint64_t size, uint8_t *buffer, int write) {
    if (!p || !p->disk || !buffer || p->disk->removed) return 0;
    uint64_t bytes;
    if (!size_mul_ok(p->sectors, p->disk->sector_size, &bytes) || offset >= bytes) return 0;
    if (size > bytes - offset) size = bytes - offset;
    uint64_t disk_offset;
    if (!lba_to_offset(p->disk, p->start_lba, offset, &disk_offset)) return 0;
    return disk_io(p->disk, disk_offset, size, buffer, write);
}

static uint64_t part_read(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return part_io((struct usb_msc_partition *)priv, offset, size, buffer, 0);
}

static uint64_t part_write(void *priv, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return part_io((struct usb_msc_partition *)priv, offset, size, buffer, 1);
}

static struct device_ops part_ops = {.read = part_read, .write = part_write, .info = part_info};

static int register_partition(struct usb_msc_disk *d, uint64_t start, uint64_t count, uint32_t number) {
    if (!lba_range_valid(d, start, count) || number == 0) return -1;
    struct usb_msc_partition *p = (struct usb_msc_partition *)kzalloc(sizeof(*p));
    if (!p) return -1;
    p->disk = d;
    p->start_lba = start;
    p->sectors = count;
    strncpy(p->scheme, "mbr", sizeof(p->scheme) - 1);
    strncpy(p->type, "primary", sizeof(p->type) - 1);
    snprintf(p->name, sizeof(p->name), "%s%u", d->name, number);
    uint64_t bytes;
    if (!size_mul_ok(count, d->sector_size, &bytes) || devfs_register_block(p->name, &part_ops, p, bytes) != 0) {
        kfree(p);
        return -1;
    }
    p->next = d->parts;
    d->parts = p;
    klog(LOG_INFO, "usb_msc: registered /dev/%s start=%x:%x sectors=%x:%x\n", p->name,
         (uint32_t)(start >> 32), (uint32_t)start, (uint32_t)(count >> 32), (uint32_t)count);
    return 0;
}

static int protective_mbr(const uint8_t *mbr) {
    for (uint32_t off = 446; off < 446 + 4 * 16; off += 16) if (mbr[off + 4] == 0xee) return 1;
    return 0;
}

static void scan_mbr(struct usb_msc_disk *d, const uint8_t *mbr) {
    for (uint32_t i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 446 + i * 16;
        uint8_t type = e[4];
        uint32_t start = rd32(e + 8);
        uint32_t count = rd32(e + 12);
        if (type == 0 || count == 0) continue;
        if (type == 0xee) continue;
        (void)register_partition(d, start, count, i + 1);
    }
}

static int uuid_zero(const uint8_t *u) {
    return (rd64(u) | rd64(u + 8)) == 0;
}

static void scan_gpt(struct usb_msc_disk *d) {
    uint8_t hdr[512];
    if (d->sector_size > sizeof(hdr)) return;
    if (scsi_rw(d, 1, 1, 0, hdr) != 0) return;
    if (memcmp(hdr, "EFI PART", 8) != 0) return;
    uint32_t header_size = rd32(hdr + 12);
    uint32_t entry_count = rd32(hdr + 80);
    uint32_t entry_size = rd32(hdr + 84);
    uint64_t entries_lba = rd64(hdr + 72);
    if (header_size < 92 || header_size > d->sector_size || entry_size < 128 || entry_size > USB_MSC_GPT_ENTRY_MAX) return;
    if (entry_count > USB_MSC_MAX_GPT_PARTS) entry_count = USB_MSC_MAX_GPT_PARTS;
    uint8_t *buf = (uint8_t *)kmalloc(d->sector_size);
    if (!buf) return;
    uint32_t per_sector = d->sector_size / entry_size;
    if (per_sector == 0) { kfree(buf); return; }
    uint32_t number = 1;
    for (uint32_t base = 0; base < entry_count; base += per_sector) {
        if (scsi_rw(d, entries_lba + base / per_sector, 1, 0, buf) != 0) break;
        for (uint32_t j = 0; j < per_sector && base + j < entry_count; j++) {
            uint8_t *e = buf + j * entry_size;
            if (uuid_zero(e)) { number++; continue; }
            uint64_t first = rd64(e + 32);
            uint64_t last = rd64(e + 40);
            if (last >= first) (void)register_partition(d, first, last - first + 1, number);
            number++;
        }
    }
    kfree(buf);
}

static void scan_partitions(struct usb_msc_disk *d) {
    if (!d || d->sector_size < 512 || d->sector_size > USB_MSC_IO_MAX) return;
    uint8_t *mbr = (uint8_t *)kmalloc(d->sector_size);
    if (!mbr) return;
    lock_disk(d);
    if (scsi_rw(d, 0, 1, 0, mbr) == 0 && mbr[510] == 0x55 && mbr[511] == 0xaa) {
        if (protective_mbr(mbr)) scan_gpt(d);
        else scan_mbr(d, mbr);
    }
    unlock_disk(d);
    kfree(mbr);
}

static int probe_device(struct usb_msc_disk *d) {
    uint8_t cdb[16];
    uint8_t inquiry[36];
    uint8_t max_lun = 0;
    memset(cdb, 0, sizeof(cdb));
    memset(inquiry, 0, sizeof(inquiry));

    if (usb_control_transfer(d->dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                             USB_MSC_REQ_GET_MAX_LUN, 0, d->intf->desc.bInterfaceNumber, &max_lun, 1, 1000) != 0)
        max_lun = 0;
    d->lun = 0;
    d->max_lun = max_lun;

    cdb[0] = SCSI_INQUIRY;
    cdb[4] = sizeof(inquiry);
    uint32_t actual = 0;
    if (scsi_cmd(d, cdb, 6, inquiry, sizeof(inquiry), 1, &actual) != 0 || actual < 36) return -1;
    if ((inquiry[0] & 0x1fu) != 0x00u) return -1;

    for (uint32_t i = 0; i < USB_MSC_READY_RETRIES; i++) {
        if (test_ready(d) == 0) break;
        kernel_sleep_ticks(10);
        if (i + 1 == USB_MSC_READY_RETRIES) return -1;
    }

    if (read_capacity16(d) != 0 && read_capacity10(d) != 0 && read_format_capacities(d) != 0) return -1;
    if (d->sectors == 0 || d->sector_size < 512 || d->sector_size > USB_MSC_IO_MAX) return -1;
    return 0;
}

static int subclass_supported(uint8_t sc) {
    return sc == USB_MSC_SUBCLASS_SCSI || sc == USB_MSC_SUBCLASS_UFI || sc == USB_MSC_SUBCLASS_SFF8070I;
}

static int protocol_supported(uint8_t p) {
    return p == USB_MSC_PROTOCOL_BOT || p == USB_MSC_PROTOCOL_CBI ||
           p == USB_MSC_PROTOCOL_CBI_NO_INT || p == USB_MSC_PROTOCOL_UAS;
}

static int msc_match(struct usb_interface *intf) {
    if (!intf) return 0;
    return intf->desc.bInterfaceClass == USB_CLASS_MASS_STORAGE &&
           subclass_supported(intf->desc.bInterfaceSubClass) &&
           protocol_supported(intf->desc.bInterfaceProtocol);
}


static void assign_bot_or_cbi_ep(struct usb_msc_disk *d, const struct usb_endpoint_descriptor *ep) {
    if ((ep->bmAttributes & 3u) == USB_ENDPOINT_XFER_BULK) {
        if ((ep->bEndpointAddress & USB_DIR_IN) && !d->bulk_in) {
            d->bulk_in = ep->bEndpointAddress;
            d->bulk_in_mps = ep_mps(ep);
        } else if (!(ep->bEndpointAddress & USB_DIR_IN) && !d->bulk_out) {
            d->bulk_out = ep->bEndpointAddress;
            d->bulk_out_mps = ep_mps(ep);
        }
    } else if ((ep->bmAttributes & 3u) == USB_ENDPOINT_XFER_INTR && (ep->bEndpointAddress & USB_DIR_IN) && !d->intr_in) {
        d->intr_in = ep->bEndpointAddress;
        d->intr_in_mps = ep_mps(ep);
    }
}

static void assign_uas_ep(struct usb_msc_disk *d, const struct usb_endpoint_descriptor *ep) {
    if ((ep->bmAttributes & 3u) != USB_ENDPOINT_XFER_BULK) return;
    if (ep->bEndpointAddress & USB_DIR_IN) {
        if (!d->uas_status) {
            d->uas_status = ep->bEndpointAddress;
            d->uas_status_mps = ep_mps(ep);
        } else if (!d->uas_data_in) {
            d->uas_data_in = ep->bEndpointAddress;
            d->uas_data_in_mps = ep_mps(ep);
        }
    } else {
        if (!d->uas_cmd) {
            d->uas_cmd = ep->bEndpointAddress;
            d->uas_cmd_mps = ep_mps(ep);
        } else if (!d->uas_data_out) {
            d->uas_data_out = ep->bEndpointAddress;
            d->uas_data_out_mps = ep_mps(ep);
        }
    }
}

static int msc_bind(struct usb_interface *intf) {
    if (!intf || !intf->dev) return -1;
    struct usb_msc_disk *d = (struct usb_msc_disk *)kzalloc(sizeof(*d));
    if (!d) return -1;
    d->intf = intf;
    d->dev = intf->dev;
    d->tag = 1;

    d->subclass = intf->desc.bInterfaceSubClass;
    d->protocol = intf->desc.bInterfaceProtocol;
    d->transport = d->protocol == USB_MSC_PROTOCOL_UAS ? USB_MSC_TRANSPORT_UAS :
                   d->protocol == USB_MSC_PROTOCOL_BOT ? USB_MSC_TRANSPORT_BOT : USB_MSC_TRANSPORT_CBI;

    for (uint32_t i = 0; i < intf->endpoint_count; i++) {
        struct usb_endpoint_descriptor *ep = &intf->endpoints[i];
        if (d->transport == USB_MSC_TRANSPORT_UAS) assign_uas_ep(d, ep);
        else assign_bot_or_cbi_ep(d, ep);
    }
    if (d->transport == USB_MSC_TRANSPORT_UAS) {
        if (!d->uas_cmd || !d->uas_status || !d->uas_data_in || !d->uas_data_out ||
            !d->uas_cmd_mps || !d->uas_status_mps || !d->uas_data_in_mps || !d->uas_data_out_mps) goto fail;
    } else {
        if (!d->bulk_in || !d->bulk_out || d->bulk_in_mps == 0 || d->bulk_out_mps == 0) goto fail;
        if (d->protocol == USB_MSC_PROTOCOL_CBI && (!d->intr_in || d->intr_in_mps == 0)) goto fail;
    }

    uint64_t idx;
    uint64_t flags;
    spinlock_acquire(&msc_lock, &flags);
    idx = next_disk_index++;
    spinlock_release(&msc_lock, flags);
    if (disk_name(idx, d->name, sizeof(d->name)) != 0) goto fail;

    if (probe_device(d) != 0) goto fail;

    uint64_t bytes;
    if (!size_mul_ok(d->sectors, d->sector_size, &bytes)) goto fail;
    if (devfs_register_block(d->name, &disk_ops, d, bytes) != 0) goto fail;

    spinlock_acquire(&msc_lock, &flags);
    d->next = disks;
    disks = d;
    intf->driver_private = d;
    spinlock_release(&msc_lock, flags);

    scan_partitions(d);
    klog(LOG_INFO, "usb_msc: registered /dev/%s sectors=%x:%x sector_size=%u lun_max=%u transport=%u subclass=%u\n", d->name,
         (uint32_t)(d->sectors >> 32), (uint32_t)d->sectors, d->sector_size, d->max_lun, d->transport, d->subclass);
    return 0;

fail:
    kfree(d);
    return -1;
}

static void free_partitions(struct usb_msc_disk *d) {
    while (d && d->parts) {
        struct usb_msc_partition *p = d->parts;
        d->parts = p->next;
        kfree(p);
    }
}

static void msc_unbind(struct usb_interface *intf) {
    if (!intf) return;
    struct usb_msc_disk *d = (struct usb_msc_disk *)intf->driver_private;
    if (!d) return;
    d->removed = 1;
    devfs_unregister(d->name);
    for (struct usb_msc_partition *p = d->parts; p; p = p->next) devfs_unregister(p->name);
    while (d->busy) kernel_yield();

    uint64_t flags;
    spinlock_acquire(&msc_lock, &flags);
    struct usb_msc_disk **pp = &disks;
    while (*pp) {
        if (*pp == d) {
            *pp = d->next;
            break;
        }
        pp = &(*pp)->next;
    }
    intf->driver_private = NULL;
    spinlock_release(&msc_lock, flags);
    free_partitions(d);
    kfree(d);
}

static struct usb_driver msc_driver = {
    .name = "usb_mass_storage",
    .match = msc_match,
    .bind = msc_bind,
    .unbind = msc_unbind,
};

static int usb_msc_init(void) {
    spinlock_init(&msc_lock);
    return usb_driver_register(&msc_driver);
}

static void usb_msc_exit(void) {
    usb_driver_unregister(&msc_driver);
}

MODULE_INFO("usb_mass_storage", usb_msc_init, 0, usb_msc_exit, "usb_core");
