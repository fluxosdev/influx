/* FAT32: formatter plus a read/write driver with long file names (used for the EFI System Partition). */
#include "fs.h"
#include "blockdev.h"
#include "mm.h"
#include "string.h"
#include "gfx.h"

struct bpb {
    uint8_t jmp[3]; char oem[8]; uint16_t bytes_per_sec; uint8_t sec_per_clus; uint16_t reserved; uint8_t nfats; uint16_t root_entries; uint16_t total16;
    uint8_t media; uint16_t fat16_size; uint16_t spt, heads; uint32_t hidden, total32;
    uint32_t fat_size; uint16_t ext_flags, fs_ver; uint32_t root_cluster; uint16_t fsinfo, backup; uint8_t res[12];
    uint8_t drive, res1, boot_sig; uint32_t vol_id; char label[11]; char fstype[8];
} PACKED;
struct dirent83 { uint8_t name[11]; uint8_t attr, ntres, ctime_tenth; uint16_t ctime, cdate, adate, clus_hi, mtime, mdate, clus_lo; uint32_t size; } PACKED;
struct lfn_ent { uint8_t seq; uint16_t name1[5]; uint8_t attr, type, chksum; uint16_t name2[6]; uint16_t clus; uint16_t name3[2]; } PACKED;
#define ATTR_RO 1
#define ATTR_DIR 0x10
#define ATTR_LFN 0x0F
#define EOC 0x0FFFFFF8
#define FAT_MASK 0x0FFFFFFF

struct fat {
    struct blockdev *dev;
    uint32_t spc, reserved, nfats, fat_size, root_cluster, total_sectors, nclusters, data_start;
    uint32_t *fat;             /* cached FAT (first copy) */
    bool fat_dirty;
    uint32_t next_free;
    char label[12];
    struct mutex lock;
    struct vnode *root;
};
struct fnode { struct vnode vn; struct fat *fs; uint32_t cluster; uint32_t dir_cluster; int dir_index; struct fnode *hnext; bool is_root; };
static const struct vnode_ops fat_ops;

static int read_sectors(struct fat *f, uint32_t lba, uint32_t n, void *buf) { return blockdev_read(f->dev, lba, n, buf); }
static int write_sectors(struct fat *f, uint32_t lba, uint32_t n, const void *buf) { return blockdev_write(f->dev, lba, n, buf); }
static uint32_t cluster_lba(struct fat *f, uint32_t c) { return f->data_start + (c - 2) * f->spc; }
static uint32_t fat_get(struct fat *f, uint32_t c) { return c < f->nclusters + 2 ? f->fat[c] & FAT_MASK : EOC; }
static void fat_set(struct fat *f, uint32_t c, uint32_t v) { f->fat[c] = (f->fat[c] & 0xF0000000) | (v & FAT_MASK); f->fat_dirty = true; }
static void fat_flush(struct fat *f) {
    if (!f->fat_dirty) return;
    for (uint32_t i = 0; i < f->nfats; i++) write_sectors(f, f->reserved + i * f->fat_size, f->fat_size, f->fat);
    /* keep FSInfo's free cluster count accurate */
    uint32_t free_c = 0; for (uint32_t c = 2; c < f->nclusters + 2; c++) if (fat_get(f, c) == 0) free_c++;
    uint8_t sec[512];
    if (read_sectors(f, 1, 1, sec) == 0 && *(uint32_t *)sec == 0x41615252) { *(uint32_t *)(sec + 488) = free_c; *(uint32_t *)(sec + 492) = f->next_free; write_sectors(f, 1, 1, sec); write_sectors(f, 7, 1, sec); }
    if (f->root) { f->root->fs_free = (uint64_t)free_c * f->spc * 512; }
    f->fat_dirty = false;
}
static uint32_t alloc_cluster(struct fat *f, uint32_t prev) {
    for (uint32_t k = 0; k < f->nclusters; k++) {
        uint32_t c = 2 + (f->next_free - 2 + k) % f->nclusters;
        if (fat_get(f, c) == 0) {
            fat_set(f, c, EOC);
            if (prev) fat_set(f, prev, c);
            f->next_free = c + 1;
            return c;
        }
    }
    return 0;
}
static void free_chain(struct fat *f, uint32_t c) { while (c >= 2 && c < EOC) { uint32_t n = fat_get(f, c); fat_set(f, c, 0); c = n; } }
static uint64_t chain_length(struct fat *f, uint32_t c) { uint64_t n = 0; while (c >= 2 && c < EOC && n < f->nclusters) { n++; c = fat_get(f, c); } return n; }

/* ---- names ---- */
static void make_83(const char *name, uint8_t out[11], bool *needs_lfn) {
    memset(out, ' ', 11);
    *needs_lfn = false;
    const char *dot = strrchr(name, '.');
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len = dot ? strlen(dot + 1) : 0;
    bool lossy = base_len > 8 || ext_len > 3 || (dot && dot == name);
    for (size_t i = 0; name[i]; i++) if (name[i] == ' ' || name[i] == '+' || name[i] == ',' || name[i] == ';' || name[i] == '=' || name[i] == '[' || name[i] == ']' || (name[i] == '.' && &name[i] != dot) || islower(name[i])) lossy = true;
    size_t bi = 0;
    for (size_t i = 0; i < base_len && bi < (lossy ? 6 : 8); i++) { char c = toupper(name[i]); if (c == ' ' || c == '.') continue; if (!isalnum(c) && !strchr("$%'-_@~`!(){}^#&", c)) c = '_'; out[bi++] = c; }
    if (bi == 0) { out[0] = '_'; bi = 1; lossy = true; }
    if (lossy) { out[bi++] = '~'; out[bi++] = '1'; }
    for (size_t i = 0; i < ext_len && i < 3; i++) { char c = toupper(dot[1 + i]); out[8 + i] = isalnum(c) ? c : '_'; }
    *needs_lfn = lossy;
}
static uint8_t lfn_checksum(const uint8_t *n) { uint8_t s = 0; for (int i = 0; i < 11; i++) s = ((s & 1) << 7) + (s >> 1) + n[i]; return s; }
static void name_from_83(const uint8_t *n, char *out) {
    int k = 0;
    for (int i = 0; i < 8 && n[i] != ' '; i++) out[k++] = n[i];
    if (n[8] != ' ') { out[k++] = '.'; for (int i = 8; i < 11 && n[i] != ' '; i++) out[k++] = n[i]; }
    out[k] = 0;
    if (out[0] == 0x05) out[0] = (char)0xE5;
}

/* ---- directory iteration ---- */
struct dir_iter { struct fat *f; uint32_t cluster; uint32_t sector_in_cluster; int index_in_sector; uint8_t sec[512]; bool loaded; int global_index; char lfn[260]; int lfn_len; uint8_t lfn_sum; bool lfn_valid; };
static bool dir_iter_init(struct dir_iter *it, struct fat *f, uint32_t cluster) { memset(it, 0, sizeof *it); it->f = f; it->cluster = cluster; it->lfn_len = 0; return true; }
/* returns pointer to the next raw entry (or NULL at end); fills LFN state */
static struct dirent83 *dir_iter_next_raw(struct dir_iter *it) {
    for (;;) {
        if (!it->loaded) {
            if (it->cluster < 2 || it->cluster >= EOC) return NULL;
            if (read_sectors(it->f, cluster_lba(it->f, it->cluster) + it->sector_in_cluster, 1, it->sec) < 0) return NULL;
            it->loaded = true; it->index_in_sector = 0;
        }
        if (it->index_in_sector >= 16) {
            it->loaded = false; it->index_in_sector = 0;
            if (++it->sector_in_cluster >= it->f->spc) { it->sector_in_cluster = 0; it->cluster = fat_get(it->f, it->cluster); }
            continue;
        }
        struct dirent83 *e = (struct dirent83 *)(it->sec + it->index_in_sector * 32);
        it->index_in_sector++; it->global_index++;
        return e;
    }
}
/* next real entry with its long name; returns 0 at end */
static int dir_iter_next(struct dir_iter *it, struct dirent83 *out, char *name, uint32_t *ent_cluster, int *ent_index) {
    it->lfn_len = 0; it->lfn_valid = false;
    uint16_t lfn_u[260]; int lfn_total = 0;
    for (;;) {
        uint32_t cur_cluster = it->cluster; int cur_index = it->sector_in_cluster * 16 + it->index_in_sector;
        struct dirent83 *e = dir_iter_next_raw(it);
        if (!e) return 0;
        if (e->name[0] == 0) return 0;
        if (e->name[0] == 0xE5) { lfn_total = 0; continue; }
        if (e->attr == ATTR_LFN) {
            struct lfn_ent *l = (struct lfn_ent *)e;
            int seq = l->seq & 0x1F; if (seq < 1 || seq > 20) { lfn_total = 0; continue; }
            if (l->seq & 0x40) { lfn_total = seq; it->lfn_sum = l->chksum; memset(lfn_u, 0, sizeof lfn_u); }
            uint16_t part[13]; memcpy(part, l->name1, 10); memcpy(part + 5, l->name2, 12); memcpy(part + 11, l->name3, 4);
            for (int i = 0; i < 13; i++) lfn_u[(seq - 1) * 13 + i] = part[i];
            continue;
        }
        if (e->attr & 0x08) { lfn_total = 0; continue; }   /* volume label */
        *out = *e;
        if (lfn_total && lfn_checksum(e->name) == it->lfn_sum) {
            int k = 0;
            for (int i = 0; i < lfn_total * 13 && lfn_u[i] && lfn_u[i] != 0xFFFF && k < 255; i++) { uint32_t cp = lfn_u[i]; char u8[4]; int n = utf8_encode(cp, u8); if (k + n > 255) break; memcpy(name + k, u8, n); k += n; }
            name[k] = 0;
        } else name_from_83(e->name, name);
        if (ent_cluster) *ent_cluster = cur_cluster;
        if (ent_index) *ent_index = cur_index;
        return 1;
    }
}

/* read/write one raw directory entry by (cluster, index within cluster) */
static int dirent_rw(struct fat *f, uint32_t cluster, int index, struct dirent83 *e, bool write) {
    uint8_t sec[512];
    uint32_t lba = cluster_lba(f, cluster) + index / 16;
    if (read_sectors(f, lba, 1, sec) < 0) return -EIO;
    if (write) { memcpy(sec + (index % 16) * 32, e, 32); return write_sectors(f, lba, 1, sec); }
    memcpy(e, sec + (index % 16) * 32, 32);
    return 0;
}

static void fat_time(uint16_t *date, uint16_t *time) {
    struct rtc_time t; unix_to_datetime(time_unix(), &t);
    *date = ((t.year - 1980) << 9) | (t.month << 5) | t.day;
    *time = (t.hour << 11) | (t.minute << 5) | (t.second / 2);
}
static uint64_t fat_to_unix(uint16_t date, uint16_t time) {
    struct rtc_time t = { 1980 + (date >> 9), (date >> 5) & 15, date & 31, time >> 11, (time >> 5) & 63, (time & 31) * 2 };
    if (t.month < 1) t.month = 1; if (t.day < 1) t.day = 1;
    return rtc_to_unix(&t);
}

/* ---- vnodes ---- */
static struct fnode *make_fnode(struct fat *f, const char *name, struct dirent83 *e, uint32_t dir_cluster, int dir_index) {
    struct fnode *n = kzalloc(sizeof *n);
    n->fs = f;
    strlcpy(n->vn.name, name, sizeof n->vn.name);
    n->vn.type = (e->attr & ATTR_DIR) ? VN_DIR : VN_FILE;
    n->vn.size = e->size; n->vn.mtime = fat_to_unix(e->mdate, e->mtime); n->vn.mode = (e->attr & ATTR_DIR) ? 0755 : 0644;
    n->cluster = ((uint32_t)e->clus_hi << 16) | e->clus_lo; n->vn.ino = n->cluster;
    n->dir_cluster = dir_cluster; n->dir_index = dir_index;
    n->vn.ops = &fat_ops; n->vn.fs = f; n->vn.fs_name = "fat32"; n->vn.refcount = 1;
    return n;
}
static void f_release(struct vnode *v) { struct fnode *n = (struct fnode *)v; if (!n->is_root) kfree(n); }

static struct vnode *f_lookup(struct vnode *dirv, const char *name) {
    struct fnode *d = (struct fnode *)dirv; struct fat *f = d->fs;
    mutex_lock(&f->lock);
    struct dir_iter it; dir_iter_init(&it, f, d->cluster);
    struct dirent83 e; char ename[256]; uint32_t ec; int ei;
    struct vnode *res = NULL;
    while (dir_iter_next(&it, &e, ename, &ec, &ei)) if (!strcasecmp(ename, name) && strcmp(ename, ".") && strcmp(ename, "..")) { res = &make_fnode(f, ename, &e, ec, ei)->vn; break; }
    mutex_unlock(&f->lock);
    return res;
}
static int f_readdir(struct vnode *dirv, int index, struct dirent *out) {
    struct fnode *d = (struct fnode *)dirv; struct fat *f = d->fs;
    mutex_lock(&f->lock);
    struct dir_iter it; dir_iter_init(&it, f, d->cluster);
    struct dirent83 e; char ename[256]; int i = 0, r = -1;
    while (dir_iter_next(&it, &e, ename, NULL, NULL)) {
        if (!strcmp(ename, ".") || !strcmp(ename, "..")) continue;
        if (i++ == index) { strlcpy(out->name, ename, sizeof out->name); out->type = (e.attr & ATTR_DIR) ? VN_DIR : VN_FILE; out->size = e.size; out->mtime = fat_to_unix(e.mdate, e.mtime); out->ino = ((uint32_t)e.clus_hi << 16) | e.clus_lo; r = 0; break; }
    }
    mutex_unlock(&f->lock);
    return r;
}
static int64_t f_read(struct vnode *v, uint64_t off, void *buf, size_t n) {
    struct fnode *fn = (struct fnode *)v; struct fat *f = fn->fs;
    if (off >= v->size) return 0;
    if (off + n > v->size) n = v->size - off;
    mutex_lock(&f->lock);
    uint32_t csize = f->spc * 512;
    uint32_t c = fn->cluster; uint64_t skip = off / csize;
    while (skip-- && c >= 2 && c < EOC) c = fat_get(f, c);
    uint8_t *tmp = kmalloc(csize); size_t done = 0; uint32_t in = off % csize;
    while (done < n && c >= 2 && c < EOC) {
        if (read_sectors(f, cluster_lba(f, c), f->spc, tmp) < 0) break;
        size_t take = MIN(n - done, csize - in);
        memcpy((uint8_t *)buf + done, tmp + in, take);
        done += take; in = 0; c = fat_get(f, c);
    }
    kfree(tmp);
    mutex_unlock(&f->lock);
    return done;
}
static int update_dirent(struct fat *f, struct fnode *fn) {
    if (fn->is_root) return 0;
    struct dirent83 e;
    if (dirent_rw(f, fn->dir_cluster, fn->dir_index, &e, false) < 0) return -EIO;
    e.size = fn->vn.type == VN_DIR ? 0 : (uint32_t)fn->vn.size; e.clus_hi = fn->cluster >> 16; e.clus_lo = fn->cluster & 0xFFFF;
    fat_time(&e.mdate, &e.mtime); e.adate = e.mdate;
    return dirent_rw(f, fn->dir_cluster, fn->dir_index, &e, true);
}
/* whole-file rewrite */
static int write_whole(struct fat *f, struct fnode *fn, const uint8_t *data, uint64_t size) {
    uint32_t csize = f->spc * 512;
    free_chain(f, fn->cluster); fn->cluster = 0;
    uint32_t prev = 0, first = 0;
    uint8_t *tmp = kmalloc(csize);
    for (uint64_t off = 0; off < size; off += csize) {
        uint32_t c = alloc_cluster(f, prev);
        if (!c) { kfree(tmp); fat_flush(f); return -ENOSPC; }
        if (!first) first = c;
        size_t take = MIN((uint64_t)csize, size - off);
        memcpy(tmp, data + off, take); if (take < csize) memset(tmp + take, 0, csize - take);
        if (write_sectors(f, cluster_lba(f, c), f->spc, tmp) < 0) { kfree(tmp); return -EIO; }
        prev = c;
    }
    kfree(tmp);
    fn->cluster = first; fn->vn.size = size; fn->vn.mtime = time_unix();
    fat_flush(f);
    return update_dirent(f, fn);
}
static int64_t f_write(struct vnode *v, uint64_t off, const void *buf, size_t n) {
    struct fnode *fn = (struct fnode *)v; struct fat *f = fn->fs;
    if (n == 0) return 0;
    uint64_t newsize = MAX(v->size, off + n);
    if (newsize > 64ull << 20) return -ENOSPC;
    uint8_t *content = kzalloc(newsize);
    if (v->size) f_read(v, 0, content, v->size);
    memcpy(content + off, buf, n);
    mutex_lock(&f->lock);
    int r = write_whole(f, fn, content, newsize);
    mutex_unlock(&f->lock);
    kfree(content);
    return r < 0 ? r : (int64_t)n;
}
static int f_truncate(struct vnode *v, uint64_t size) {
    struct fnode *fn = (struct fnode *)v; struct fat *f = fn->fs;
    if (size == v->size) return 0;
    uint8_t *content = kzalloc(size ? size : 1);
    if (size && v->size) f_read(v, 0, content, MIN(size, v->size));
    mutex_lock(&f->lock);
    int r = write_whole(f, fn, content, size);
    mutex_unlock(&f->lock);
    kfree(content);
    return r;
}

/* find 'count' consecutive free directory slots, extending the directory if needed; returns cluster/index of the first */
static int dir_find_slots(struct fat *f, uint32_t dir_cluster, int count, uint32_t *out_cluster, int *out_index) {
    uint32_t c = dir_cluster, prev = 0;
    int run = 0; uint32_t run_c = 0; int run_i = 0;
    uint8_t sec[512];
    while (c >= 2 && c < EOC) {
        for (uint32_t s = 0; s < f->spc; s++) {
            if (read_sectors(f, cluster_lba(f, c) + s, 1, sec) < 0) return -EIO;
            for (int i = 0; i < 16; i++) {
                uint8_t first = sec[i * 32];
                if (first == 0 || first == 0xE5) { if (!run) { run_c = c; run_i = s * 16 + i; } if (++run == count) { *out_cluster = run_c; *out_index = run_i; return 0; } }
                else run = 0;
            }
        }
        prev = c; c = fat_get(f, c);
    }
    /* extend */
    uint32_t nc = alloc_cluster(f, prev);
    if (!nc) return -ENOSPC;
    uint8_t *z = kzalloc(f->spc * 512); write_sectors(f, cluster_lba(f, nc), f->spc, z); kfree(z);
    if (run) { /* a run may continue into the new cluster only if it was at the end; keep it simple: start fresh */ }
    *out_cluster = nc; *out_index = 0;
    return count <= (int)(f->spc * 16) ? 0 : -ENOSPC;
}
static int write_slot(struct fat *f, uint32_t *c, int *idx, const void *entry) {
    struct dirent83 e; memcpy(&e, entry, 32);
    int r = dirent_rw(f, *c, *idx, &e, true);
    if (r < 0) return r;
    (*idx)++;
    if (*idx >= (int)(f->spc * 16)) { *idx = 0; *c = fat_get(f, *c); }
    return 0;
}

static struct vnode *f_create(struct vnode *dirv, const char *name, int type) {
    struct fnode *d = (struct fnode *)dirv; struct fat *f = d->fs;
    size_t nl = strlen(name); if (!nl || nl > 255) return NULL;
    mutex_lock(&f->lock);
    uint8_t short_name[11]; bool lfn; make_83(name, short_name, &lfn);
    /* make the short name unique (~1 .. ~9) */
    for (int k = 2; k < 10; k++) {
        struct dir_iter it; dir_iter_init(&it, f, d->cluster); struct dirent83 e; char en[256]; bool clash = false;
        while (dir_iter_next(&it, &e, en, NULL, NULL)) if (!memcmp(e.name, short_name, 11)) { clash = true; break; }
        if (!clash) break;
        uint8_t *t = memchr(short_name, '~', 8); if (t) t[1] = '0' + k;
    }
    int nlfn = lfn || nl > 12 ? (int)((nl + 12) / 13) : 0;
    /* always store an LFN so the exact case is preserved */
    nlfn = (int)((nl + 12) / 13);
    uint32_t sc; int si;
    if (dir_find_slots(f, d->cluster, nlfn + 1, &sc, &si) < 0) { mutex_unlock(&f->lock); return NULL; }
    uint32_t first = 0;
    if (type == VN_DIR) {
        first = alloc_cluster(f, 0);
        if (!first) { mutex_unlock(&f->lock); return NULL; }
        uint8_t *z = kzalloc(f->spc * 512);
        struct dirent83 *dot = (struct dirent83 *)z;
        memset(dot[0].name, ' ', 11); dot[0].name[0] = '.'; dot[0].attr = ATTR_DIR; dot[0].clus_hi = first >> 16; dot[0].clus_lo = first & 0xFFFF;
        memset(dot[1].name, ' ', 11); dot[1].name[0] = '.'; dot[1].name[1] = '.'; dot[1].attr = ATTR_DIR; uint32_t pc = d->is_root ? 0 : d->cluster; dot[1].clus_hi = pc >> 16; dot[1].clus_lo = pc & 0xFFFF;
        fat_time(&dot[0].mdate, &dot[0].mtime); dot[1].mdate = dot[0].mdate; dot[1].mtime = dot[0].mtime;
        write_sectors(f, cluster_lba(f, first), f->spc, z);
        kfree(z);
    }
    uint8_t sum = lfn_checksum(short_name);
    uint16_t uname[260]; int ul = 0; const char *p = name; while (*p && ul < 255) uname[ul++] = (uint16_t)utf8_decode(&p);
    for (int i = nlfn; i >= 1; i--) {
        struct lfn_ent l; memset(&l, 0xFF, sizeof l);
        l.seq = i | (i == nlfn ? 0x40 : 0); l.attr = ATTR_LFN; l.type = 0; l.chksum = sum; l.clus = 0;
        uint16_t part[13]; for (int k = 0; k < 13; k++) { int idx = (i - 1) * 13 + k; part[k] = idx < ul ? uname[idx] : idx == ul ? 0 : 0xFFFF; }
        memcpy(l.name1, part, 10); memcpy(l.name2, part + 5, 12); memcpy(l.name3, part + 11, 4);
        write_slot(f, &sc, &si, &l);
    }
    struct dirent83 e; memset(&e, 0, sizeof e);
    memcpy(e.name, short_name, 11); e.attr = type == VN_DIR ? ATTR_DIR : 0; e.clus_hi = first >> 16; e.clus_lo = first & 0xFFFF; e.size = 0;
    fat_time(&e.cdate, &e.ctime); e.mdate = e.cdate; e.mtime = e.ctime; e.adate = e.cdate;
    uint32_t ec = sc; int ei = si;
    write_slot(f, &sc, &si, &e);
    fat_flush(f);
    struct fnode *n = make_fnode(f, name, &e, ec, ei);
    vnode_ref(&n->vn);
    mutex_unlock(&f->lock);
    return &n->vn;
}
static int f_unlink(struct vnode *dirv, const char *name) {
    struct fnode *d = (struct fnode *)dirv; struct fat *f = d->fs;
    mutex_lock(&f->lock);
    struct dir_iter it; dir_iter_init(&it, f, d->cluster);
    struct dirent83 e; char en[256]; uint32_t ec; int ei; int r = -ENOENT;
    while (dir_iter_next(&it, &e, en, &ec, &ei)) {
        if (strcasecmp(en, name)) continue;
        if (e.attr & ATTR_DIR) {
            uint32_t dc = ((uint32_t)e.clus_hi << 16) | e.clus_lo;
            struct dir_iter it2; dir_iter_init(&it2, f, dc); struct dirent83 e2; char n2[256];
            while (dir_iter_next(&it2, &e2, n2, NULL, NULL)) if (strcmp(n2, ".") && strcmp(n2, "..")) { mutex_unlock(&f->lock); return -ENOTEMPTY; }
        }
        /* delete the entry and the LFN entries directly before it */
        struct dirent83 del; dirent_rw(f, ec, ei, &del, false); del.name[0] = 0xE5; dirent_rw(f, ec, ei, &del, true);
        uint32_t c = ec; int i = ei;
        for (int k = 0; k < 20; k++) {
            if (i == 0) break;   /* LFN entries that cross a cluster boundary are rare; leave them */
            i--;
            struct dirent83 prev; dirent_rw(f, c, i, &prev, false);
            if (prev.attr != ATTR_LFN || prev.name[0] == 0xE5) break;
            prev.name[0] = 0xE5; dirent_rw(f, c, i, &prev, true);
        }
        free_chain(f, ((uint32_t)e.clus_hi << 16) | e.clus_lo);
        fat_flush(f);
        r = 0; break;
    }
    mutex_unlock(&f->lock);
    return r;
}
static int f_sync(struct vnode *v) { struct fnode *n = (struct fnode *)v; mutex_lock(&n->fs->lock); fat_flush(n->fs); mutex_unlock(&n->fs->lock); return 0; }
static const struct vnode_ops fat_ops = { f_read, f_write, f_truncate, f_lookup, f_readdir, f_create, f_unlink, NULL, f_release, f_sync };

struct vnode *fat32_mount(struct blockdev *d) {
    uint8_t *sec = kmalloc(512);
    if (blockdev_read(d, 0, 1, sec) < 0) { kfree(sec); return NULL; }
    struct bpb *b = (struct bpb *)sec;
    if (sec[510] != 0x55 || sec[511] != 0xAA || b->bytes_per_sec != 512 || b->fat16_size != 0 || b->fat_size == 0 || memcmp(b->fstype, "FAT32   ", 8)) { kfree(sec); return NULL; }
    struct fat *f = kzalloc(sizeof *f);
    f->dev = d; f->spc = b->sec_per_clus; f->reserved = b->reserved; f->nfats = b->nfats; f->fat_size = b->fat_size; f->root_cluster = b->root_cluster;
    f->total_sectors = b->total32 ? b->total32 : b->total16;
    f->data_start = f->reserved + f->nfats * f->fat_size;
    f->nclusters = (f->total_sectors - f->data_start) / f->spc;
    memcpy(f->label, b->label, 11); f->label[11] = 0; for (int i = 10; i >= 0 && f->label[i] == ' '; i--) f->label[i] = 0;
    kfree(sec);
    if ((uint64_t)f->fat_size * 512 > 32ull << 20) { kprintf("fat32: %s: FAT too large to cache\n", d->name); kfree(f); return NULL; }
    f->fat = kmalloc(f->fat_size * 512);
    if (blockdev_read(d, f->reserved, f->fat_size, f->fat) < 0) { kfree(f->fat); kfree(f); return NULL; }
    f->next_free = 2;
    mutex_init(&f->lock);
    struct fnode *root = kzalloc(sizeof *root);
    root->fs = f; root->cluster = f->root_cluster; root->is_root = true;
    strlcpy(root->vn.name, f->label[0] ? f->label : d->name, sizeof root->vn.name);
    root->vn.type = VN_DIR; root->vn.ops = &fat_ops; root->vn.fs = f; root->vn.fs_name = "fat32"; root->vn.refcount = 1; root->vn.mode = 0755;
    f->root = &root->vn;
    uint64_t free_c = 0; for (uint32_t c = 2; c < f->nclusters + 2; c++) if (fat_get(f, c) == 0) free_c++;
    root->vn.fs_total = (uint64_t)f->nclusters * f->spc * 512; root->vn.fs_free = free_c * f->spc * 512;
    d->fs_type = "fat32";
    kprintf("fat32: %s: '%s' %lu MiB, %u sectors/cluster\n", d->name, f->label, root->vn.fs_total >> 20, f->spc);
    return &root->vn;
}

/* ---- formatter ---- */
int fat32_format(struct blockdev *d, const char *label) {
    uint64_t total = d->sectors;
    if (total < 70000) return -ENOSPC;    /* FAT32 needs at least 65525 clusters */
    uint32_t spc = total < (512ull << 11) ? 1 : total < (8ull << 21) ? 8 : 32;
    uint32_t reserved = 32, nfats = 2;
    uint32_t clusters = (uint32_t)((total - reserved) / spc);
    uint32_t fat_size = ((uint64_t)(clusters + 2) * 4 + 511) / 512;
    clusters = (uint32_t)((total - reserved - nfats * fat_size) / spc);
    uint8_t *sec = kzalloc(512);
    struct bpb *b = (struct bpb *)sec;
    b->jmp[0] = 0xEB; b->jmp[1] = 0x58; b->jmp[2] = 0x90; memcpy(b->oem, "INFLUX  ", 8);
    b->bytes_per_sec = 512; b->sec_per_clus = spc; b->reserved = reserved; b->nfats = nfats; b->media = 0xF8; b->spt = 63; b->heads = 255;
    b->total32 = (uint32_t)total; b->fat_size = fat_size; b->root_cluster = 2; b->fsinfo = 1; b->backup = 6; b->drive = 0x80; b->boot_sig = 0x29;
    b->vol_id = (uint32_t)(rdtsc() ^ time_unix());
    memset(b->label, ' ', 11); for (int i = 0; i < 11 && label[i]; i++) b->label[i] = toupper(label[i]);
    memcpy(b->fstype, "FAT32   ", 8);
    sec[510] = 0x55; sec[511] = 0xAA;
    int r = blockdev_write(d, 0, 1, sec); if (r == 0) r = blockdev_write(d, 6, 1, sec);
    /* FSInfo */
    memset(sec, 0, 512);
    *(uint32_t *)(sec + 0) = 0x41615252; *(uint32_t *)(sec + 484) = 0x61417272;
    *(uint32_t *)(sec + 488) = clusters - 1; *(uint32_t *)(sec + 492) = 3; sec[510] = 0x55; sec[511] = 0xAA;
    if (r == 0) r = blockdev_write(d, 1, 1, sec); if (r == 0) r = blockdev_write(d, 7, 1, sec);
    /* FATs */
    uint32_t *fat = kzalloc(fat_size * 512);
    fat[0] = 0x0FFFFFF8; fat[1] = 0x0FFFFFFF; fat[2] = 0x0FFFFFFF;
    for (uint32_t i = 0; i < nfats && r == 0; i++) r = blockdev_write(d, reserved + i * fat_size, fat_size, fat);
    kfree(fat);
    /* root directory cluster (with the volume label entry) + the rest of the reserved area */
    uint8_t *z = kzalloc(spc * 512);
    struct dirent83 *lbl = (struct dirent83 *)z;
    memset(lbl->name, ' ', 11); for (int i = 0; i < 11 && label[i]; i++) lbl->name[i] = toupper(label[i]);
    lbl->attr = 0x08; fat_time(&lbl->mdate, &lbl->mtime);
    if (r == 0) r = blockdev_write(d, reserved + nfats * fat_size, spc, z);
    for (uint32_t s = 2; s < reserved && r == 0; s++) if (s != 6 && s != 7) r = blockdev_write(d, s, 1, z);
    kfree(z); kfree(sec);
    return r;
}
