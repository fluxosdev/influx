#include "blockdev.h"
#include "fs.h"
#include "mm.h"
#include "string.h"
#include "sched.h"
#include "io.h"

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
/* for the panic handler (other CPUs halted, interrupts off): skips the lock, and gives up if a
 * transfer was in flight when the system stopped, since the controller may be mid-command */
int blockdev_write_panic(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf) {
    uint64_t off; struct blockdev *root = resolve(d, lba, count, &off);
    if (!root) return -ENODEV;
    if (io_lock.locked) return -EBUSY;
    return root->write(root, off, count, buf);
}
int blockdev_flush_panic(struct blockdev *d) {
    uint64_t off; struct blockdev *root = resolve(d, 0, 0, &off);
    if (!root || io_lock.locked) return -EBUSY;
    return root->flush ? root->flush(root) : 0;
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
        p->esp = !memcmp(e->type, GPT_TYPE_ESP, 16);
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

int blockdev_ptable(struct blockdev *d) {
    if (d->sectors < 3) return PTABLE_NONE;
    struct gpt_header h; uint8_t *entries;
    const char *why = gpt_read(d, 1, &h, &entries);
    if (!why) { kfree(entries); return PTABLE_GPT; }
    uint8_t *mbr = kmalloc(d->sector_size);
    bool mbr_ok = blockdev_read(d, 0, 1, mbr) == 0 && mbr[510] == 0x55 && mbr[511] == 0xAA, prot = false, any = false;
    for (int i = 0; mbr_ok && i < 4; i++) { uint8_t t = mbr[446 + i * 16 + 4]; if (t == 0xEE) prot = true; else if (t && *(uint32_t *)(mbr + 446 + i * 16 + 12)) any = true; }
    kfree(mbr);
    if (why == GPT_NOSIG && !prot) return any ? PTABLE_MBR : PTABLE_NONE;
    if (!gpt_read(d, d->sectors - 1, &h, &entries)) { kfree(entries); return PTABLE_GPT; }
    return PTABLE_NONE;
}

/* ---- GPT editing ---- */
const uint8_t GPT_TYPE_BASIC_DATA[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
const uint8_t GPT_TYPE_LINUX_FS[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };
const uint8_t GPT_TYPE_ESP[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };

static void random_guid(uint8_t *g) {
    static uint64_t seed;
    seed ^= rdtsc() ^ (time_unix() << 20);
    for (int i = 0; i < 16; i++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; g[i] = seed >> 56; }
    g[7] = (g[7] & 0x0F) | 0x40; g[8] = (g[8] & 0x3F) | 0x80;   /* version 4, RFC 4122 variant (GPT stores the first three fields little-endian) */
}

/* the primary or, failing that, the backup GPT; *entries is kmalloc'd */
static int gpt_load(struct blockdev *d, struct gpt_header *h, uint8_t **entries) {
    if (d->sectors < 3) return -EINVAL;
    if (!gpt_read(d, 1, h, entries)) return 0;
    if (!gpt_read(d, d->sectors - 1, h, entries)) return 0;
    return -EINVAL;
}

/* writes the entry array and both headers (primary at LBA 1, backup in the last sector) from h's
 * geometry; the backup header goes first so that a torn update leaves at least one valid copy */
static int gpt_commit(struct blockdev *d, const struct gpt_header *h, const uint8_t *entries) {
    uint32_t ss = d->sector_size;
    uint64_t bytes = (uint64_t)h->nparts * h->part_size, nsec = (bytes + ss - 1) / ss, last = d->sectors - 1;
    if (d->sectors < 2 * nsec + 4 || h->first_usable < 2 + nsec || h->last_usable >= last - nsec || h->first_usable > h->last_usable) return -EINVAL;
    uint8_t *arr = kzalloc(nsec * ss);
    memcpy(arr, entries, bytes);
    struct gpt_header p = *h;
    p.hdr_size = 92; p.res = 0; p.parts_crc = crc32_ieee(arr, bytes);
    p.cur_lba = 1; p.backup_lba = last; p.part_lba = 2;
    struct gpt_header b = p; b.cur_lba = last; b.backup_lba = 1; b.part_lba = last - nsec;
    p.crc = 0; p.crc = crc32_ieee(&p, 92);
    b.crc = 0; b.crc = crc32_ieee(&b, 92);
    uint8_t *sec = kzalloc(ss);
    int r = blockdev_write(d, b.part_lba, nsec, arr);
    if (r == 0) { memcpy(sec, &b, sizeof b); r = blockdev_write(d, last, 1, sec); }
    if (r == 0) r = blockdev_flush(d);
    if (r == 0) r = blockdev_write(d, 2, nsec, arr);
    if (r == 0) { memset(sec, 0, ss); memcpy(sec, &p, sizeof p); r = blockdev_write(d, 1, 1, sec); }
    if (r == 0) r = blockdev_flush(d);
    kfree(sec); kfree(arr);
    return r;
}

int gpt_usable_range(struct blockdev *d, uint64_t *first, uint64_t *last) {
    struct gpt_header h; uint8_t *e;
    int r = gpt_load(d, &h, &e);
    if (r < 0) return r;
    kfree(e);
    *first = h.first_usable; *last = MIN(h.last_usable, d->sectors - 1);
    return 0;
}

int gpt_create(struct blockdev *d) {
    uint32_t ss = d->sector_size;
    if (ss < 512 || d->sectors < 4096) return -ENOSPC;
    struct gpt_header h; memset(&h, 0, sizeof h);
    uint64_t nsec = (128 * 128 + ss - 1) / ss;
    memcpy(h.sig, "EFI PART", 8); h.rev = 0x00010000; h.nparts = 128; h.part_size = 128;
    h.first_usable = 2 + nsec; h.last_usable = d->sectors - 2 - nsec;
    random_guid(h.guid);
    /* protective MBR: no boot code, one 0xEE entry over the whole disk */
    uint8_t *mbr = kzalloc(ss);
    uint8_t *e = mbr + 446;
    e[1] = 0x00; e[2] = 0x02; e[3] = 0x00; e[4] = 0xEE; e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;
    *(uint32_t *)(e + 8) = 1; *(uint32_t *)(e + 12) = (uint32_t)MIN(d->sectors - 1, 0xFFFFFFFFull);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    int r = blockdev_write(d, 0, 1, mbr);
    kfree(mbr);
    uint8_t *entries = kzalloc(128 * 128);
    if (r == 0) r = gpt_commit(d, &h, entries);
    kfree(entries);
    kprintf("blockdev: %s: new GPT partition table%s\n", d->name, r ? " failed" : "");
    return r;
}

int gpt_add_partition(struct blockdev *d, uint64_t first, uint64_t last, const uint8_t type[16], const char *name) {
    struct gpt_header h; uint8_t *ents;
    int r = gpt_load(d, &h, &ents);
    if (r < 0) return r;
    if (first > last || first < h.first_usable || last > h.last_usable) { kfree(ents); return -EINVAL; }
    int slot = -1;
    for (uint32_t i = 0; i < h.nparts; i++) {
        struct gpt_entry *e = (struct gpt_entry *)(ents + i * h.part_size);
        bool empty = true; for (int k = 0; k < 16; k++) if (e->type[k]) empty = false;
        if (empty) { if (slot < 0 && i < 128) slot = i; continue; }   /* the scan only reads the first 128 entries */
        if (first <= e->last && e->first <= last) { kfree(ents); return -EEXIST; }
    }
    if (slot < 0) { kfree(ents); return -ENOSPC; }
    struct gpt_entry *e = (struct gpt_entry *)(ents + slot * h.part_size);
    memset(e, 0, h.part_size);
    memcpy(e->type, type, 16); random_guid(e->guid); e->first = first; e->last = last;
    for (int i = 0; name && name[i] && i < 36; i++) e->name[i] = (uint8_t)name[i];
    r = gpt_commit(d, &h, ents);
    kfree(ents);
    kprintf("blockdev: %s: added partition LBA %lu-%lu%s\n", d->name, first, last, r ? " failed" : "");
    return r;
}

int gpt_delete_partition(struct blockdev *d, uint64_t first) {
    struct gpt_header h; uint8_t *ents;
    int r = gpt_load(d, &h, &ents);
    if (r < 0) return r;
    r = -ENOENT;
    for (uint32_t i = 0; i < h.nparts; i++) {
        struct gpt_entry *e = (struct gpt_entry *)(ents + i * h.part_size);
        bool empty = true; for (int k = 0; k < 16; k++) if (e->type[k]) empty = false;
        if (!empty && e->first == first) { memset(e, 0, h.part_size); r = 0; break; }
    }
    if (r == 0) r = gpt_commit(d, &h, ents);
    kfree(ents);
    kprintf("blockdev: %s: deleted the partition at LBA %lu%s\n", d->name, first, r ? " failed" : "");
    return r;
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
