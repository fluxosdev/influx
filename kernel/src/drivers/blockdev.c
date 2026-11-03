#include "blockdev.h"
#include "fs.h"
#include "mm.h"
#include "string.h"
#include "sched.h"

static struct blockdev *devs[32];
static int ndevs;
static struct mutex io_lock = MUTEX_INIT;
static struct mutex devs_lock = MUTEX_INIT;   /* devs[] / ndevs (hotplug vs. lookups) */

/* translate to the whole disk; partitions were checked against the parent's size when created */
static struct blockdev *resolve(struct blockdev *d, uint64_t lba, uint32_t count, uint64_t *off) {
    if (lba > d->sectors || count > d->sectors - lba) return NULL;
    struct blockdev *root = d; *off = lba;
    for (;;) { if (root->removed) return NULL; if (!root->parent) return root; *off += root->start_lba; root = root->parent; }
}
int blockdev_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf) {
    uint64_t off; struct blockdev *root = resolve(d, lba, count, &off);
    if (!root) return lba > d->sectors || count > d->sectors - lba ? -EINVAL : -ENODEV;
    mutex_lock(&io_lock);
    int r = root->read(root, off, count, buf);
    mutex_unlock(&io_lock);
    return r;
}
int blockdev_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf) {
    uint64_t off; struct blockdev *root = resolve(d, lba, count, &off);
    if (!root) return lba > d->sectors || count > d->sectors - lba ? -EINVAL : -ENODEV;
    mutex_lock(&io_lock);
    int r = root->write(root, off, count, buf);
    mutex_unlock(&io_lock);
    return r;
}
/* drives acknowledge writes from a volatile cache: filesystems call this before and after their commit
 * point (superblock) so that it cannot reach the medium ahead of the blocks it points to */
int blockdev_flush(struct blockdev *d) {
    uint64_t off; struct blockdev *root = resolve(d, 0, 0, &off);
    if (!root) return -ENODEV;
    if (!root->flush) return 0;
    mutex_lock(&io_lock);
    int r = root->flush(root);
    mutex_unlock(&io_lock);
    return r;
}

/* byte-granular access with a bounce buffer, batched in 64 KiB chunks */
int blockdev_read_bytes(struct blockdev *d, uint64_t off, void *buf, size_t n) {
    uint32_t ss = d->sector_size;
    uint8_t *out = buf;
    uint8_t *bounce = kmalloc(65536);
    while (n) {
        uint64_t lba = off / ss; uint32_t skip = off % ss;
        uint32_t want = MIN(n + skip, 65536);
        uint32_t cnt = (want + ss - 1) / ss;
        int r = blockdev_read(d, lba, cnt, bounce);
        if (r < 0) { kfree(bounce); return r; }
        size_t take = MIN(n, (size_t)cnt * ss - skip);
        memcpy(out, bounce + skip, take);
        out += take; off += take; n -= take;
    }
    kfree(bounce);
    return 0;
}
int blockdev_write_bytes(struct blockdev *d, uint64_t off, const void *buf, size_t n) {
    uint32_t ss = d->sector_size;
    const uint8_t *in = buf;
    uint8_t *bounce = kmalloc(65536);
    while (n) {
        uint64_t lba = off / ss; uint32_t skip = off % ss;
        uint32_t want = MIN(n + skip, 65536);
        uint32_t cnt = (want + ss - 1) / ss;
        size_t take = MIN(n, (size_t)cnt * ss - skip);
        bool partial = skip || take < (size_t)cnt * ss;
        if (partial) { int r = blockdev_read(d, lba, cnt, bounce); if (r < 0) { kfree(bounce); return r; } }
        memcpy(bounce + skip, in, take);
        int r = blockdev_write(d, lba, cnt, bounce);
        if (r < 0) { kfree(bounce); return r; }
        in += take; off += take; n -= take;
    }
    kfree(bounce);
    return 0;
}

/* devfs node ops */
static int64_t blk_vn_read(struct vnode *v, uint64_t off, void *buf, size_t n) {
    struct blockdev *d = v->priv; uint64_t size = d->sectors * d->sector_size;
    if (off >= size) return 0; if (off + n > size) n = size - off;
    int r = blockdev_read_bytes(d, off, buf, n); return r < 0 ? r : (int64_t)n;
}
static int64_t blk_vn_write(struct vnode *v, uint64_t off, const void *buf, size_t n) {
    struct blockdev *d = v->priv; uint64_t size = d->sectors * d->sector_size;
    if (off >= size) return -ENOSPC; if (off + n > size) n = size - off;
    int r = blockdev_write_bytes(d, off, buf, n); return r < 0 ? r : (int64_t)n;
}
static int blk_vn_sync(struct vnode *v) { return blockdev_flush(v->priv); }
static const struct vnode_ops blk_ops = { blk_vn_read, blk_vn_write, NULL, NULL, NULL, NULL, NULL, NULL, NULL, blk_vn_sync };

struct gpt_header { char sig[8]; uint32_t rev, hdr_size, crc, res; uint64_t cur_lba, backup_lba, first_usable, last_usable; uint8_t guid[16]; uint64_t part_lba; uint32_t nparts, part_size, parts_crc; } PACKED;
struct gpt_entry { uint8_t type[16], guid[16]; uint64_t first, last, attrs; uint16_t name[36]; } PACKED;

static void add_dev(struct blockdev *d) {
    mutex_lock(&devs_lock);
    if (ndevs >= 32) { mutex_unlock(&devs_lock); return; }
    devs[ndevs++] = d;
    mutex_unlock(&devs_lock);
    d->vn = devfs_add(d->name, VN_BLK, &blk_ops, d, d->sectors * d->sector_size);
    char sz[32]; uint64_t bytes = d->sectors * d->sector_size;
    if (bytes >= 1024ULL * 1024 * 1024) snprintf(sz, sizeof sz, "%lu.%lu GiB", bytes >> 30, ((bytes >> 20) % 1024) / 103); else snprintf(sz, sizeof sz, "%lu MiB", bytes >> 20);
    kprintf("blockdev: /dev/%s %s%s%s (%s)\n", d->name, d->parent ? "partition of " : "", d->parent ? d->parent->name : d->model, d->parent ? "" : "", sz);
}

static const uint8_t GUID_ESP[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };

uint32_t crc32_ieee(const void *data, size_t n);
struct prange { uint64_t first, last; };
static bool overlaps(const struct prange *r, int n, uint64_t first, uint64_t last) { for (int i = 0; i < n; i++) if (first <= r[i].last && r[i].first <= last) return true; return false; }
static struct blockdev *new_part(struct blockdev *d, int num, uint64_t first, uint64_t count) {
    struct blockdev *p = kzalloc(sizeof *p);
    snprintf(p->name, sizeof p->name, "%s%s%d", d->name, isdigit(d->name[strlen(d->name) - 1]) ? "p" : "", num);
    p->parent = d; p->start_lba = first; p->sectors = count; p->sector_size = d->sector_size;
    p->read = d->read; p->write = d->write; p->flush = d->flush; p->bus = d->bus;
    return p;
}

/* read and verify the GPT header at 'lba' and its entry array (*entries, caller frees). NULL when valid, else why not */
static const char GPT_NOSIG[] = "no signature";
static const char *gpt_read(struct blockdev *d, uint64_t lba, struct gpt_header *h, uint8_t **entries) {
    uint32_t ss = d->sector_size;
    uint8_t *sec = kmalloc(ss);
    const char *why = NULL;
    *entries = NULL;
    if (blockdev_read(d, lba, 1, sec) < 0) { kfree(sec); return "unreadable"; }
    memcpy(h, sec, sizeof *h);
    if (memcmp(h->sig, "EFI PART", 8)) why = GPT_NOSIG;
    else if (h->hdr_size < sizeof *h || h->hdr_size > ss) why = "bad header size";
    else { ((struct gpt_header *)sec)->crc = 0; if (crc32_ieee(sec, h->hdr_size) != h->crc) why = "header CRC mismatch"; }
    kfree(sec);
    if (why) return why;
    if (h->cur_lba != lba) return "header location mismatch";
    if (h->part_size < sizeof(struct gpt_entry) || h->part_size > 1024 || (h->part_size & (h->part_size - 1)) || !h->nparts || h->nparts > 1024) return "bad entry size/count";
    if (h->first_usable > h->last_usable) return "bad usable range";
    uint64_t bytes = (uint64_t)h->nparts * h->part_size, nsec = (bytes + ss - 1) / ss;
    if (h->part_lba < 2 || h->part_lba >= d->sectors || nsec > d->sectors - h->part_lba) return "entry array out of range";
    uint8_t *e = kmalloc_try(nsec * ss);
    if (!e) return "out of memory";
    if (blockdev_read(d, h->part_lba, nsec, e) < 0) { kfree(e); return "entry array unreadable"; }
    if (crc32_ieee(e, bytes) != h->parts_crc) { kfree(e); return "entry array CRC mismatch"; }
    *entries = e;
    return NULL;
}

static void scan_gpt(struct blockdev *d, const struct gpt_header *h, const uint8_t *entries) {
    uint32_t n = MIN(h->nparts, 128u), psz = h->part_size;
    struct prange used[128]; int nused = 0, idx = 1;
    for (uint32_t i = 0; i < n; i++) {
        const struct gpt_entry *e = (const struct gpt_entry *)(entries + i * psz);
        bool empty = true; for (int k = 0; k < 16; k++) if (e->type[k]) empty = false;
        if (empty) continue;
        if (e->last < e->first || e->first < h->first_usable || e->last > h->last_usable || e->last >= d->sectors) { kprintf("blockdev: %s: GPT entry %u (LBA %lu-%lu) out of range, ignored\n", d->name, i, e->first, e->last); continue; }
        if (overlaps(used, nused, e->first, e->last)) { kprintf("blockdev: %s: GPT entry %u overlaps another partition, ignored\n", d->name, i); continue; }
        used[nused++] = (struct prange){ e->first, e->last };
        struct blockdev *p = new_part(d, idx++, e->first, e->last - e->first + 1);
        p->esp = !memcmp(e->type, GUID_ESP, 16);
        for (int k = 0; k < 36 && e->name[k] && k < 47; k++) p->model[k] = (char)e->name[k];
        add_dev(p);
    }
}
static void scan_mbr(struct blockdev *d, const uint8_t *sec) {
    struct prange used[4]; int nused = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec + 446 + i * 16;
        uint32_t start = *(const uint32_t *)(e + 8), count = *(const uint32_t *)(e + 12);
        if (!e[4] || !count || e[4] == 0xEE) continue;
        if (!start || (uint64_t)start + count > d->sectors) { kprintf("blockdev: %s: MBR entry %d out of range, ignored\n", d->name, i); continue; }
        if (overlaps(used, nused, start, (uint64_t)start + count - 1)) { kprintf("blockdev: %s: MBR entry %d overlaps another partition, ignored\n", d->name, i); continue; }
        used[nused++] = (struct prange){ start, (uint64_t)start + count - 1 };
        struct blockdev *p = new_part(d, i + 1, start, count);
        p->esp = e[4] == 0xEF;
        snprintf(p->model, sizeof p->model, "MBR type 0x%02x", e[4]);
        add_dev(p);
    }
}

/* GPT (primary, else the backup in the last sector), else MBR. A disk whose GPT is damaged beyond
 * both copies is left without partitions rather than falling back to a possibly stale MBR. */
static void scan_partitions(struct blockdev *d) {
    if (d->sectors < 3) return;
    struct gpt_header h; uint8_t *entries;
    const char *why = gpt_read(d, 1, &h, &entries);
    if (why) {
        uint8_t *mbr = kmalloc(d->sector_size);
        bool mbr_ok = blockdev_read(d, 0, 1, mbr) == 0 && mbr[510] == 0x55 && mbr[511] == 0xAA, prot = false;
        for (int i = 0; mbr_ok && i < 4; i++) if (mbr[446 + i * 16 + 4] == 0xEE) prot = true;
        if (why == GPT_NOSIG && !prot) { if (mbr_ok) scan_mbr(d, mbr); kfree(mbr); return; }
        kfree(mbr);
        const char *bwhy = gpt_read(d, d->sectors - 1, &h, &entries);
        if (bwhy) { kprintf("blockdev: %s: GPT unusable (primary: %s, backup: %s), partitions ignored\n", d->name, why, bwhy); return; }
        kprintf("blockdev: %s: primary GPT invalid (%s), using the backup\n", d->name, why);
    }
    scan_gpt(d, &h, entries);
    kfree(entries);
}

void blockdev_register(struct blockdev *d) {
    add_dev(d);
    scan_partitions(d);
}
/* Old partition structs are not freed (a mounted fs, /dev node or open file may
 * still hold them); they are marked removed so their I/O fails with -ENODEV. */
void blockdev_rescan(struct blockdev *d) {
    mutex_lock(&devs_lock);
    for (int i = 0; i < ndevs; ) {
        if (devs[i]->parent == d) {
            char p[32]; snprintf(p, sizeof p, "/dev/%s", devs[i]->name); vfs_unlink(p);
            devs[i]->removed = true;
            for (int j = i; j < ndevs - 1; j++) devs[j] = devs[j + 1];
            ndevs--;
        } else i++;
    }
    mutex_unlock(&devs_lock);
    scan_partitions(d);
}
/* The structs are not freed: a filesystem that was mounted from the disk may
 * still hold them, and fails its I/O with -ENODEV from here on. */
void blockdev_unregister(struct blockdev *d) {
    d->removed = true;
    mutex_lock(&devs_lock);
    for (int i = 0; i < ndevs; ) {
        if (devs[i] == d || devs[i]->parent == d) {
            char p[32]; snprintf(p, sizeof p, "/dev/%s", devs[i]->name); vfs_unlink(p);
            kprintf("blockdev: /dev/%s removed\n", devs[i]->name);
            devs[i]->removed = true;
            for (int j = i; j < ndevs - 1; j++) devs[j] = devs[j + 1];
            ndevs--;
        } else i++;
    }
    mutex_unlock(&devs_lock);
}
void blockdev_alloc_name(const char *prefix, char *out, size_t n) {
    for (char c = 'a'; c <= 'z'; c++) {
        snprintf(out, n, "%s%c", prefix, c);
        if (!blockdev_find(out)) return;
    }
}
/* structs handed out are never freed, so they stay valid after the lock drops; blockdev_at may return NULL if the list shrank */
int blockdev_count(void) { return ndevs; }
struct blockdev *blockdev_at(int i) { mutex_lock(&devs_lock); struct blockdev *d = i >= 0 && i < ndevs ? devs[i] : NULL; mutex_unlock(&devs_lock); return d; }
struct blockdev *blockdev_find(const char *name) { mutex_lock(&devs_lock); struct blockdev *d = NULL; for (int i = 0; i < ndevs; i++) if (!strcmp(devs[i]->name, name)) { d = devs[i]; break; } mutex_unlock(&devs_lock); return d; }
