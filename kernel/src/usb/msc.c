/* USB mass storage: Bulk-Only Transport carrying SCSI commands. Every LUN
 * with a medium becomes a block device (/dev/sdX), its filesystems mounted
 * under /mnt; on unplug they are unmounted and further I/O fails. */
#include "usb.h"
#include "blockdev.h"
#include "mm.h"
#include "string.h"

#define CBW_SIG 0x43425355
#define CSW_SIG 0x53425355
#define MAX_LUNS 4

struct cbw { uint32_t sig, tag, len; uint8_t flags, lun, cblen, cb[16]; } PACKED;
struct csw { uint32_t sig, tag, residue; uint8_t status; } PACKED;

struct msc;
struct msc_lun { struct msc *m; int lun; struct blockdev *bd; bool no_sync; };
struct msc {
    struct usb_device *dev;
    struct usb_endpoint *in, *out;
    uint8_t iface;
    uint32_t tag;
    uint32_t moved;              /* data-stage bytes of the last command */
    struct mutex lock;           /* one command at a time; disconnect waits for it */
    bool gone;
    struct cbw *cbw;
    struct csw *csw;
    uint8_t *scratch;            /* replies to small commands */
    int nluns;
    struct msc_lun luns[MAX_LUNS];
};

static void reset_recovery(struct msc *m) {
    usb_control(m->dev, USB_TYPE_CLASS | USB_RECIP_INTF, 0xFF /* Bulk-Only Mass Storage Reset */, 0, m->iface, NULL, 0);
    usb_clear_halt(m->dev, m->in);
    usb_clear_halt(m->dev, m->out);
}

/* 0, -EIO when the device reports the command failed (sense data waits), other -errno on transport errors */
static int bot_cmd(struct msc *m, int lun, const uint8_t *cb, int cblen, void *buf, uint32_t len, bool in, int timeout) {
    if (m->dev->gone) return -ENODEV;
    struct cbw *w = m->cbw;
    memset(w, 0, sizeof *w);
    w->sig = CBW_SIG; w->tag = ++m->tag; w->len = len; w->flags = in ? 0x80 : 0; w->lun = lun; w->cblen = cblen;
    memcpy(w->cb, cb, cblen);
    int r = usb_transfer(m->dev, m->out, w, 31, 5000);
    if (r != 31) { if (r != -ENODEV) reset_recovery(m); return r < 0 ? r : -EIO; }
    m->moved = 0;
    if (len) {
        r = usb_transfer(m->dev, in ? m->in : m->out, buf, len, timeout);
        if (r > 0) m->moved = r;
        if (r == -ENODEV) return r;
        if (r < 0 && r != -EPIPE) { reset_recovery(m); return r; }   /* a stall was cleared: the CSW follows */
    }
    for (int tries = 0; ; tries++) {
        r = usb_transfer(m->dev, m->in, m->csw, 13, 5000);
        if (r == -EPIPE && tries == 0) continue;
        break;
    }
    if (r == -ENODEV) return r;
    if (r != 13 || m->csw->sig != CSW_SIG || m->csw->tag != w->tag || m->csw->status == 2) { reset_recovery(m); return -EIO; }
    return m->csw->status == 0 ? 0 : -EIO;
}

/* sense key << 16 | ASC << 8 | ASCQ, or -errno */
static int request_sense(struct msc *m, int lun) {
    uint8_t cb[6] = { 0x03, 0, 0, 0, 18, 0 };
    memset(m->scratch, 0, 18);
    int r = bot_cmd(m, lun, cb, 6, m->scratch, 18, true, 5000);
    if (r < 0) return r;
    return (m->scratch[2] & 0x0F) << 16 | m->scratch[12] << 8 | m->scratch[13];
}

static int msc_io(struct blockdev *d, uint64_t lba, uint32_t count, void *buf, bool write) {
    struct msc_lun *l = d->priv;
    struct msc *m = l->m;
    uint32_t bs = d->sector_size, max = MAX(65536 / bs, 1u);
    uint8_t *p = buf;
    int r = 0;
    mutex_lock(&m->lock);
    while (count && r == 0) {
        if (m->gone) { r = -ENODEV; break; }
        uint32_t n = MIN(count, max);
        uint8_t cb[16] = { 0 }; int cblen;
        if (lba + n > 0xFFFFFFFFULL) {
            cb[0] = write ? 0x8A : 0x88; cblen = 16;
            for (int i = 0; i < 8; i++) cb[2 + i] = lba >> (56 - 8 * i);
            cb[10] = n >> 24; cb[11] = n >> 16; cb[12] = n >> 8; cb[13] = n;
        } else {
            cb[0] = write ? 0x2A : 0x28; cblen = 10;
            cb[2] = lba >> 24; cb[3] = lba >> 16; cb[4] = lba >> 8; cb[5] = lba;
            cb[7] = n >> 8; cb[8] = n;
        }
        for (int tries = 0; tries < 3; tries++) {
            r = bot_cmd(m, l->lun, cb, cblen, p, n * bs, !write, 20000);
            if (r == 0 && (m->moved != n * bs || m->csw->residue)) {   /* a short READ would hand stale data to the filesystem */
                kprintf("usb-storage: %s: %s moved %u of %u bytes (residue %u)\n", d->name, write ? "write" : "read", m->moved, n * bs, m->csw->residue);
                r = -EIO;
                continue;
            }
            if (r != -EIO) break;
            int sense = request_sense(m, l->lun);
            kprintf("usb-storage: %s %s of %u sectors at %lu failed, sense %06x\n", d->name, write ? "write" : "read", n, lba, sense < 0 ? 0 : sense);
            if (sense >= 0 && (sense >> 16) != 6 && (sense >> 16) != 2) break;   /* only retry unit attention / not ready */
        }
        lba += n; p += n * bs; count -= n;
    }
    mutex_unlock(&m->lock);
    return r < 0 ? -EIO : 0;
}
static int msc_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf) { return msc_io(d, lba, count, buf, false); }
static int msc_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf) { return msc_io(d, lba, count, (void *)buf, true); }
/* SYNCHRONIZE CACHE (10) over the whole LUN. Many sticks have no write cache and reject it: ILLEGAL
 * REQUEST (or a failure without sense data) means unsupported, which counts as success from then on. */
static int msc_flush(struct blockdev *d) {
    struct msc_lun *l = d->priv;
    struct msc *m = l->m;
    if (l->no_sync) return 0;
    int r = 0;
    mutex_lock(&m->lock);
    for (int tries = 0; tries < 3; tries++) {
        if (m->gone) { r = -ENODEV; break; }
        uint8_t cb[10] = { 0x35 };
        r = bot_cmd(m, l->lun, cb, 10, NULL, 0, false, 30000);
        if (r != -EIO) break;
        int sense = request_sense(m, l->lun);
        if (sense < 0) break;
        if ((sense >> 16) == 5 || (sense >> 16) == 0) { l->no_sync = true; r = 0; kprintf("usb-storage: %s: SYNCHRONIZE CACHE not supported (sense %06x)\n", d->name, sense); break; }
        if ((sense >> 16) != 6 && (sense >> 16) != 2) { kprintf("usb-storage: %s: SYNCHRONIZE CACHE failed, sense %06x\n", d->name, sense); break; }   /* only retry unit attention / not ready */
    }
    mutex_unlock(&m->lock);
    return r < 0 ? (r == -ENODEV ? r : -EIO) : 0;
}

static void trim(char *s) { int n = strlen(s); while (n && s[n - 1] == ' ') s[--n] = 0; }

/* INQUIRY, wait for the medium, READ CAPACITY; the caller holds m->lock */
static bool lun_probe(struct msc *m, int lun, char *model, size_t model_len, uint64_t *blocks, uint32_t *block_size) {
    uint8_t *s = m->scratch;
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    memset(s, 0, 36);
    if (bot_cmd(m, lun, inq, 6, s, 36, true, 5000) < 0) return false;
    int type = s[0] & 0x1F;
    if (type != 0x00 && type != 0x0E) { kprintf("usb-storage: LUN %d: device type %d not supported\n", lun, type); return false; }
    char vendor[9], product[17];
    memcpy(vendor, s + 8, 8); vendor[8] = 0; trim(vendor);
    memcpy(product, s + 16, 16); product[16] = 0; trim(product);
    snprintf(model, model_len, "%s%s%s", vendor, vendor[0] ? " " : "", product);

    /* sticks answer "becoming ready" or "unit attention" first */
    bool ready = false;
    for (int i = 0; i < 25 && !ready; i++) {
        uint8_t tur[6] = { 0 };
        int r = bot_cmd(m, lun, tur, 6, NULL, 0, false, 5000);
        if (r == 0) { ready = true; break; }
        if (r != -EIO) return false;
        int sense = request_sense(m, lun);
        if (sense < 0) return false;
        if ((sense & 0xFFFF00) == 0x023A00) { kprintf("usb-storage: LUN %d (%s): no medium\n", lun, model); return false; }
        thread_sleep_ms(200);
    }
    if (!ready) { kprintf("usb-storage: LUN %d (%s) did not become ready\n", lun, model); return false; }

    uint8_t rc[10] = { 0x25 };
    if (bot_cmd(m, lun, rc, 10, s, 8, true, 5000) < 0) return false;
    uint64_t last = (uint32_t)(s[0] << 24 | s[1] << 16 | s[2] << 8 | s[3]);
    uint32_t bs = s[4] << 24 | s[5] << 16 | s[6] << 8 | s[7];
    if (last == 0xFFFFFFFF) {   /* over 2 TiB: READ CAPACITY (16) */
        uint8_t rc16[16] = { 0x9E, 0x10 };
        rc16[13] = 32;
        if (bot_cmd(m, lun, rc16, 16, s, 32, true, 5000) < 0) return false;
        last = 0; for (int i = 0; i < 8; i++) last = last << 8 | s[i];
        bs = s[8] << 24 | s[9] << 16 | s[10] << 8 | s[11];
    }
    if (bs < 512 || bs > 4096 || (bs & (bs - 1))) { kprintf("usb-storage: LUN %d: block size %u not supported\n", lun, bs); return false; }
    *blocks = last + 1; *block_size = bs;
    return true;
}

static void lun_init(struct msc *m, int lun) {
    char model[48]; uint64_t blocks; uint32_t bs;
    mutex_lock(&m->lock);
    bool ok = lun_probe(m, lun, model, sizeof model, &blocks, &bs);
    mutex_unlock(&m->lock);
    if (!ok) return;
    struct msc_lun *l = &m->luns[m->nluns++];
    l->m = m; l->lun = lun;
    struct blockdev *d = kzalloc(sizeof *d);
    blockdev_alloc_name("sd", d->name, sizeof d->name);
    strlcpy(d->model, model, sizeof d->model);
    d->sectors = blocks; d->sector_size = bs;
    d->read = msc_read; d->write = msc_write; d->flush = msc_flush; d->priv = l; d->bus = "USB";
    l->bd = d;
    blockdev_register(d);   /* reads the partition table through msc_io, which takes m->lock */
    storage_mount(d);
}

static bool msc_probe(struct usb_interface *i) {
    if (i->cls != 8 || i->proto != 0x50) return false;   /* Bulk-Only; CBI and UAS are not handled */
    if (i->subcls != 6 && i->subcls != 5 && i->subcls != 2) { kprintf("usb-storage: command set %02x not supported\n", i->subcls); return false; }
    struct usb_endpoint *in = usb_find_ep(i, USB_EP_BULK, true), *out = usb_find_ep(i, USB_EP_BULK, false);
    if (!in || !out) return false;
    struct msc *m = kzalloc(sizeof *m);
    m->dev = i->dev; m->in = in; m->out = out; m->iface = i->num;
    mutex_init(&m->lock);
    m->cbw = kzalloc(sizeof *m->cbw); m->csw = kzalloc(sizeof *m->csw); m->scratch = kzalloc(64);
    i->drvdata = m;
    uint8_t maxlun = 0;
    if (usb_control(m->dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTF, 0xFE /* Get Max LUN */, 0, m->iface, &maxlun, 1) < 1) maxlun = 0;   /* may stall: one LUN */
    kprintf("usb:   interface %d: mass storage, %d LUN%s\n", i->num, maxlun + 1, maxlun ? "s" : "");
    for (int lun = 0; lun < MIN(maxlun + 1, MAX_LUNS); lun++) lun_init(m, lun);
    return true;
}

/* the msc and blockdev structs stay allocated: a filesystem may still point at them */
static void msc_disconnect(struct usb_interface *i) {
    struct msc *m = i->drvdata;
    if (!m) return;
    mutex_lock(&m->lock);
    m->gone = true;
    mutex_unlock(&m->lock);
    for (int k = 0; k < m->nluns; k++) {
        storage_unmount(m->luns[k].bd);
        blockdev_unregister(m->luns[k].bd);
    }
}

const struct usb_driver usb_msc_driver = { "usb-storage", msc_probe, msc_disconnect };
