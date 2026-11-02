#include "blockdev.h"
#include "fs.h"
#include "mm.h"
#include "string.h"
#include "sched.h"

static struct blockdev *devs[32];
static int ndevs;
static struct mutex io_lock = MUTEX_INIT;

int blockdev_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf) {
    if (lba + count > d->sectors) return -EINVAL;
    struct blockdev *root = d; uint64_t off = lba;
    while (root->parent) { off += root->start_lba; root = root->parent; }
    mutex_lock(&io_lock);
    int r = root->read(root, off, count, buf);
    mutex_unlock(&io_lock);
    return r;
}
int blockdev_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf) {
    if (lba + count > d->sectors) return -EINVAL;
    struct blockdev *root = d; uint64_t off = lba;
    while (root->parent) { off += root->start_lba; root = root->parent; }
    mutex_lock(&io_lock);
    int r = root->write(root, off, count, buf);
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
static const struct vnode_ops blk_ops = { blk_vn_read, blk_vn_write, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

struct gpt_header { char sig[8]; uint32_t rev, hdr_size, crc, res; uint64_t cur_lba, backup_lba, first_usable, last_usable; uint8_t guid[16]; uint64_t part_lba; uint32_t nparts, part_size, parts_crc; } PACKED;
struct gpt_entry { uint8_t type[16], guid[16]; uint64_t first, last, attrs; uint16_t name[36]; } PACKED;

static void add_dev(struct blockdev *d) {
    if (ndevs >= 32) return;
    devs[ndevs++] = d;
    d->vn = devfs_add(d->name, VN_BLK, &blk_ops, d, d->sectors * d->sector_size);
    char sz[32]; uint64_t bytes = d->sectors * d->sector_size;
    if (bytes >= 1024ULL * 1024 * 1024) snprintf(sz, sizeof sz, "%lu.%lu GiB", bytes >> 30, ((bytes >> 20) % 1024) / 103); else snprintf(sz, sizeof sz, "%lu MiB", bytes >> 20);
    kprintf("blockdev: /dev/%s %s%s%s (%s)\n", d->name, d->parent ? "partition of " : "", d->parent ? d->parent->name : d->model, d->parent ? "" : "", sz);
}

static void scan_partitions(struct blockdev *d) {
    uint8_t *sec = kmalloc(d->sector_size * 2);
    if (blockdev_read(d, 1, 1, sec) == 0 && !memcmp(sec, "EFI PART", 8)) {
        struct gpt_header *h = (struct gpt_header *)sec;
        uint32_t n = MIN(h->nparts, 128u), psz = h->part_size; uint64_t plba = h->part_lba;
        uint8_t *entries = kmalloc(n * psz);
        if (blockdev_read(d, plba, (n * psz + d->sector_size - 1) / d->sector_size, entries) == 0) {
            int idx = 1;
            for (uint32_t i = 0; i < n; i++) {
                struct gpt_entry *e = (struct gpt_entry *)(entries + i * psz);
                bool empty = true; for (int k = 0; k < 16; k++) if (e->type[k]) empty = false;
                if (empty || e->last < e->first) continue;
                struct blockdev *p = kzalloc(sizeof *p);
                snprintf(p->name, sizeof p->name, "%s%s%d", d->name, isdigit(d->name[strlen(d->name) - 1]) ? "p" : "", idx++);
                p->parent = d; p->start_lba = e->first; p->sectors = e->last - e->first + 1; p->sector_size = d->sector_size;
                p->read = d->read; p->write = d->write; p->bus = d->bus;
                for (int k = 0; k < 36 && e->name[k] && k < 47; k++) p->model[k] = (char)e->name[k];
                add_dev(p);
            }
        }
        kfree(entries);
    } else if (blockdev_read(d, 0, 1, sec) == 0 && sec[510] == 0x55 && sec[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            uint8_t *e = sec + 446 + i * 16;
            uint32_t start = *(uint32_t *)(e + 8), count = *(uint32_t *)(e + 12);
            if (!e[4] || !count || e[4] == 0xEE) continue;
            struct blockdev *p = kzalloc(sizeof *p);
            snprintf(p->name, sizeof p->name, "%s%s%d", d->name, isdigit(d->name[strlen(d->name) - 1]) ? "p" : "", i + 1);
            p->parent = d; p->start_lba = start; p->sectors = count; p->sector_size = d->sector_size;
            p->read = d->read; p->write = d->write; p->bus = d->bus;
            snprintf(p->model, sizeof p->model, "MBR type 0x%02x", e[4]);
            add_dev(p);
        }
    }
    kfree(sec);
}

void blockdev_register(struct blockdev *d) {
    add_dev(d);
    scan_partitions(d);
}
void blockdev_rescan(struct blockdev *d) {
    for (int i = 0; i < ndevs; ) {
        if (devs[i]->parent == d) {
            char p[32]; snprintf(p, sizeof p, "/dev/%s", devs[i]->name); vfs_unlink(p);
            kfree(devs[i]);
            for (int j = i; j < ndevs - 1; j++) devs[j] = devs[j + 1];
            ndevs--;
        } else i++;
    }
    scan_partitions(d);
}
int blockdev_count(void) { return ndevs; }
struct blockdev *blockdev_at(int i) { return i >= 0 && i < ndevs ? devs[i] : NULL; }
struct blockdev *blockdev_find(const char *name) { for (int i = 0; i < ndevs; i++) if (!strcmp(devs[i]->name, name)) return devs[i]; return NULL; }
