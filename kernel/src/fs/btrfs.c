/* btrfs VFS glue: inodes, directories, file data, create/unlink. */
#include "btrfs_internal.h"
#include "mm.h"
#include "string.h"

#define EXTENT_MAX (1024 * 1024)
#define MAX_INLINE 2048
#define WBUF_MAX (256ull << 20)   /* files are buffered whole in memory while written */
#define WB_GIVE_UP 5              /* background write-back attempts before only sync/close retry */
enum { FLUSH_ALL, FLUSH_IDLE, FLUSH_HEALTHY };   /* sync: everything; background: idle and not backing off; metadata ops: skip failing inodes */
static int flush_all(struct btrfs_fs *fs, int mode);
static void btrfs_register_flusher(struct btrfs_fs *fs);

/* wfails/wretry_at: failed write-backs of the buffered data and when the background flusher may try again */
struct bnode { struct vnode vn; struct btrfs_fs *fs; uint64_t ino; struct bnode *hnext; uint8_t *wbuf; uint64_t wsize, wcap; bool wdirty; uint64_t wdirty_since; uint32_t wfails; uint64_t wretry_at; };
static const struct vnode_ops btrfs_ops;

static struct btrfs_timespec now_ts(void) { uint64_t sec; uint32_t nsec; vfs_now(&sec, &nsec); return (struct btrfs_timespec){ sec, nsec }; }

static int read_inode(struct btrfs_fs *fs, uint64_t ino, struct btrfs_inode_item *out, struct btrfs_path *pp) {
    struct btrfs_key k = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    struct btrfs_path p;
    int r = btrfs_search(fs, &fs->fs_tree, &k, &p);
    if (r != 0) return r < 0 ? r : -ENOENT;
    if (btrfs_path_size(&p) < sizeof *out) return -EIO;
    memcpy(out, btrfs_path_data(&p), sizeof *out);
    if (pp) *pp = p;
    return 0;
}
static int write_inode(struct btrfs_fs *fs, uint64_t ino, const struct btrfs_inode_item *in) {
    struct btrfs_key k = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    struct btrfs_path p;
    if (btrfs_search(fs, &fs->fs_tree, &k, &p) != 0) return -ENOENT;
    memcpy(btrfs_path_data(&p), in, sizeof *in);
    btrfs_mark_dirty(fs, p.blocks[0]);
    return 0;
}
static void fill_vnode(struct bnode *b, const struct btrfs_inode_item *ii) {
    uint32_t fmt = ii->mode & 0170000;
    b->vn.type = fmt == 040000 ? VN_DIR : fmt == 0120000 ? VN_LNK : fmt == 010000 ? VN_FIFO : VN_FILE;
    b->vn.size = b->wbuf ? b->wsize : ii->size;   /* buffered writes are newer than the inode */
    if (!b->wdirty) { b->vn.mtime = ii->mtime.sec; b->vn.mtime_nsec = ii->mtime.nsec; }
    b->vn.ctime = ii->ctime.sec; b->vn.ctime_nsec = ii->ctime.nsec;
    b->vn.mode = ii->mode & 07777; b->vn.ino = b->ino; b->vn.nlink = ii->nlink;
}
static struct bnode *get_bnode(struct btrfs_fs *fs, uint64_t ino, const char *name) {
    uint32_t h = (uint32_t)ino % 256;
    for (struct bnode *b = fs->inode_cache[h]; b; b = b->hnext) if (b->ino == ino) { vnode_ref(&b->vn); return b; }
    struct btrfs_inode_item ii;
    if (read_inode(fs, ino, &ii, NULL) < 0) return NULL;
    struct bnode *b = kzalloc(sizeof *b);
    b->fs = fs; b->ino = ino;
    strlcpy(b->vn.name, name, sizeof b->vn.name);
    b->vn.ops = &btrfs_ops; b->vn.fs = fs; b->vn.fs_name = "btrfs"; b->vn.refcount = 2;   /* one for the cache */
    fill_vnode(b, &ii);
    b->hnext = fs->inode_cache[h]; fs->inode_cache[h] = b;
    return b;
}
static void drop_bnode(struct btrfs_fs *fs, uint64_t ino) {
    uint32_t h = (uint32_t)ino % 256;
    for (struct bnode **pp = (struct bnode **)&fs->inode_cache[h]; *pp; pp = &(*pp)->hnext) if ((*pp)->ino == ino) { struct bnode *b = *pp; *pp = b->hnext; b->hnext = NULL; vnode_unref(&b->vn); return; }
}
static void refresh_vnode(struct bnode *b) { struct btrfs_inode_item ii; if (read_inode(b->fs, b->ino, &ii, NULL) == 0) fill_vnode(b, &ii); }
static void update_space(struct btrfs_fs *fs) { if (fs->root_vn) { fs->root_vn->fs_total = fs->sb.total_bytes; fs->root_vn->fs_free = fs->sb.total_bytes > fs->sb.bytes_used ? fs->sb.total_bytes - fs->sb.bytes_used : 0; } }

/* ---- directories ---- */
static uint64_t name_hash(const char *name, size_t len) { return crc32c((uint32_t)~1, name, len); }

/* find a dir item entry; returns the location objectid and file type */
static int dir_lookup(struct btrfs_fs *fs, uint64_t dir, const char *name, uint64_t *ino, uint8_t *ftype) {
    size_t nl = strlen(name);
    struct btrfs_key k = { dir, BTRFS_DIR_ITEM_KEY, name_hash(name, nl) };
    struct btrfs_path p;
    if (btrfs_search(fs, &fs->fs_tree, &k, &p) != 0) return -ENOENT;
    uint8_t *d = btrfs_path_data(&p), *end = d + btrfs_path_size(&p);
    while (d + sizeof(struct btrfs_dir_item) <= end) {
        struct btrfs_dir_item *di = (struct btrfs_dir_item *)d;
        const char *n = (const char *)(di + 1);
        if ((uint8_t *)n + di->name_len + di->data_len > end) return -EIO;
        if (di->name_len == nl && !memcmp(n, name, nl)) {
            if (di->location.type != BTRFS_INODE_ITEM_KEY) return -ENOENT;   /* subvolume references are not supported */
            *ino = di->location.objectid; if (ftype) *ftype = di->type; return 0;
        }
        d += sizeof(struct btrfs_dir_item) + di->name_len + di->data_len;
    }
    return -ENOENT;
}

static struct vnode *b_lookup(struct vnode *dirv, const char *name) {
    struct bnode *dir = (struct bnode *)dirv; struct btrfs_fs *fs = dir->fs;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    uint64_t ino; struct bnode *b = NULL;
    if (dir_lookup(fs, dir->ino, name, &ino, NULL) == 0) b = get_bnode(fs, ino, name);
    mutex_unlock(&fs->lock);
    return b ? &b->vn : NULL;
}

static int b_readdir(struct vnode *dirv, int index, struct dirent *out) {
    struct bnode *dir = (struct bnode *)dirv; struct btrfs_fs *fs = dir->fs;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    struct btrfs_key k = { dir->ino, BTRFS_DIR_INDEX_KEY, 0 };
    struct btrfs_path p;
    int r = btrfs_search(fs, &fs->fs_tree, &k, &p);
    if (r < 0) { mutex_unlock(&fs->lock); return -1; }
    int i = 0;
    while (!btrfs_path_normalize(fs, &p)) {
        struct btrfs_key *ik = btrfs_path_key(&p);
        if (ik->objectid != dir->ino || ik->type != BTRFS_DIR_INDEX_KEY) break;
        struct btrfs_dir_item *di = btrfs_path_data(&p);
        if (btrfs_path_size(&p) >= sizeof *di && sizeof *di + di->name_len <= btrfs_path_size(&p) && di->location.type == BTRFS_INODE_ITEM_KEY) {
            if (i == index) {
                size_t nl = MIN(di->name_len, NAME_MAX);
                memcpy(out->name, di + 1, nl); out->name[nl] = 0;
                out->type = di->type == BTRFS_FT_DIR ? VN_DIR : di->type == BTRFS_FT_SYMLINK ? VN_LNK : di->type == BTRFS_FT_FIFO ? VN_FIFO : VN_FILE;
                out->ino = di->location.objectid;
                struct btrfs_inode_item ii;
                if (read_inode(fs, di->location.objectid, &ii, NULL) == 0) { out->size = ii.size; out->mtime = ii.mtime.sec; } else { out->size = 0; out->mtime = 0; }
                mutex_unlock(&fs->lock);
                return 0;
            }
            i++;
        }
        if (btrfs_next_item(fs, &p)) break;
    }
    mutex_unlock(&fs->lock);
    return -1;
}

/* ---- file data ---- */
static int64_t read_data(struct btrfs_fs *fs, uint64_t ino, uint64_t off, void *buf, size_t n) {
    uint8_t *out = buf; size_t done = 0;
    struct btrfs_key k = { ino, BTRFS_EXTENT_DATA_KEY, off };
    struct btrfs_path p;
    int r = btrfs_search(fs, &fs->fs_tree, &k, &p);
    if (r < 0) return r;
    if (r == 1) {
        /* the extent covering 'off' may start before it */
        if (btrfs_prev_item(fs, &p) == 0) { struct btrfs_key *pk = btrfs_path_key(&p); if (pk->objectid != ino || pk->type != BTRFS_EXTENT_DATA_KEY) btrfs_next_item(fs, &p); }
    }
    while (done < n) {
        if (btrfs_path_normalize(fs, &p)) break;
        struct btrfs_key *ik = btrfs_path_key(&p);
        if (ik->objectid != ino || ik->type != BTRFS_EXTENT_DATA_KEY) break;
        uint64_t pos = off + done;
        struct btrfs_file_extent_item *fe = btrfs_path_data(&p);
        uint64_t estart = ik->offset;
        if (estart > pos) {   /* hole */
            size_t z = MIN(n - done, estart - pos);
            memset(out + done, 0, z); done += z; continue;
        }
        uint32_t isz = btrfs_path_size(&p);
        if (isz < BTRFS_FILE_EXTENT_INLINE_DATA_START || (fe->type != 0 && isz < sizeof *fe)) return -EIO;
        if (fe->compression || fe->encryption) return -EIO;
        uint64_t elen;
        if (fe->type == 0) {
            elen = btrfs_path_size(&p) - BTRFS_FILE_EXTENT_INLINE_DATA_START;
            if (pos < estart + elen) { size_t c = MIN(n - done, estart + elen - pos); memcpy(out + done, (uint8_t *)fe + BTRFS_FILE_EXTENT_INLINE_DATA_START + (pos - estart), c); done += c; }
            else { if (btrfs_next_item(fs, &p)) break; continue; }
        } else {
            elen = fe->num_bytes;
            if (pos >= estart + elen) { if (btrfs_next_item(fs, &p)) break; continue; }
            size_t c = MIN(n - done, estart + elen - pos);
            if (fe->disk_bytenr == 0 || fe->type == 2) memset(out + done, 0, c);
            else {
                uint64_t logical = fe->disk_bytenr + fe->offset + (pos - estart);
                uint64_t phys = btrfs_logical_to_phys(fs, logical, 0);
                if (!phys) return -EIO;
                if (blockdev_read_bytes(fs->dev, phys, out + done, c) < 0) return -EIO;
            }
            done += c;
        }
        if (done < n && btrfs_next_item(fs, &p)) break;
    }
    return done;
}

/* ---- write buffering: whole-file in-memory copies flushed on close/sync ---- */
static int rewrite_file(struct btrfs_fs *fs, struct bnode *b, const uint8_t *content, uint64_t size);
static int load_wbuf(struct btrfs_fs *fs, struct bnode *b) {
    if (b->wbuf) return 0;
    struct btrfs_inode_item ii;
    int r = read_inode(fs, b->ino, &ii, NULL);
    if (r < 0) return r;
    if (ii.size > WBUF_MAX) return -EFBIG;
    b->wcap = MIN(MAX(ii.size + 4096, 16384), WBUF_MAX);
    b->wbuf = kmalloc_try(b->wcap);
    if (!b->wbuf && ii.size) b->wbuf = kmalloc_try(b->wcap = ii.size);
    if (!b->wbuf) { b->wcap = 0; return -ENOMEM; }
    b->wsize = ii.size;
    if (ii.size) { int64_t got = read_data(fs, b->ino, 0, b->wbuf, ii.size); if (got < 0) { kfree(b->wbuf); b->wbuf = NULL; b->wcap = 0; return (int)got; } if ((uint64_t)got < ii.size) memset(b->wbuf + got, 0, ii.size - got); }
    return 0;
}
/* make room for 'need' (<= WBUF_MAX) bytes: doubling while small, +25% once large, never past WBUF_MAX */
static int grow_wbuf(struct bnode *b, uint64_t need) {
    if (need <= b->wcap) return 0;
    uint64_t nc = b->wcap < (16ull << 20) ? b->wcap * 2 : b->wcap + b->wcap / 4;
    nc = MIN(ALIGN_UP(MAX(nc, need), 4096), MAX(WBUF_MAX, need));
    uint8_t *nb = krealloc_try(b->wbuf, nc);
    if (!nb && nc > need) nb = krealloc_try(b->wbuf, nc = need);
    if (!nb) return -ENOMEM;   /* the old buffer is intact */
    b->wbuf = nb; b->wcap = nc;
    return 0;
}
static int flush_inode(struct btrfs_fs *fs, struct bnode *b) {
    if (!b->wdirty) return 0;
    int r = rewrite_file(fs, b, b->wbuf, b->wsize);
    if (r < 0) {   /* keep the buffer: the old contents are intact on disk and sync/close retry */
        b->wfails++;
        b->wretry_at = system_ticks + (2000ull << MIN(b->wfails, 6u));
        if (b->wfails == 1 || b->wfails == WB_GIVE_UP) kprintf("btrfs: %s: writing back inode %lu failed (%d)%s\n", fs->dev->name, b->ino, r, b->wfails == WB_GIVE_UP ? "; background write-back stopped, data stays buffered until the next sync" : "");
        return r;
    }
    b->wdirty = false; b->wfails = 0;
    kfree(b->wbuf); b->wbuf = NULL; b->wcap = 0;
    return r;
}
static int flush_all(struct btrfs_fs *fs, int mode) {
    int r = 0;
    for (int i = 0; i < 256; i++) for (struct bnode *b = fs->inode_cache[i]; b; b = b->hnext) {
        if (!b->wdirty) continue;
        if (mode == FLUSH_IDLE && (system_ticks - b->wdirty_since <= 2000 || b->wfails >= WB_GIVE_UP || (b->wfails && system_ticks < b->wretry_at))) continue;
        if (mode == FLUSH_HEALTHY && b->wfails) continue;
        int e = flush_inode(fs, b); if (e < 0) r = e;
    }
    return r;
}

static int64_t b_read(struct vnode *v, uint64_t off, void *buf, size_t n) {
    struct bnode *b = (struct bnode *)v; struct btrfs_fs *fs = b->fs;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    if (b->wbuf) {
        if (off >= b->wsize) { mutex_unlock(&fs->lock); return 0; }
        if (off + n > b->wsize) n = b->wsize - off;
        memcpy(buf, b->wbuf + off, n);
        mutex_unlock(&fs->lock);
        return (int64_t)n;
    }
    struct btrfs_inode_item ii;
    if (read_inode(fs, b->ino, &ii, NULL) < 0) { mutex_unlock(&fs->lock); return -EIO; }
    if (off >= ii.size) { mutex_unlock(&fs->lock); return 0; }
    if (off + n > ii.size) n = ii.size - off;
    memset(buf, 0, n);
    int64_t r = read_data(fs, b->ino, off, buf, n);
    mutex_unlock(&fs->lock);
    if (r < 0) return r;
    return (int64_t)n;   /* holes/NO_HOLES gaps read as zeros */
}

/* header of the EXTENT_DATA item at p, copied before the item is deleted or rewritten */
static struct btrfs_file_extent_item extent_head(struct btrfs_path *p) {
    struct btrfs_file_extent_item fe = { 0 }; memcpy(&fe, btrfs_path_data(p), MIN(sizeof fe, btrfs_path_size(p)));
    return fe;
}
/* release the data reference held by a (now removed or rewritten) EXTENT_DATA item at file_off */
static void drop_extent_ref(struct btrfs_fs *fs, uint64_t ino, uint64_t file_off, const struct btrfs_file_extent_item *fe) {
    if (fe->type != 0 && fe->disk_bytenr) btrfs_free_data_extent(fs, fe->disk_bytenr, fe->disk_num_bytes, fs->fs_tree.objectid, ino, file_off - fe->offset);
}

/* free every data extent of an inode and delete its EXTENT_DATA items */
static int remove_extents(struct btrfs_fs *fs, uint64_t ino) {
    for (int guard = 0; guard < 100000; guard++) {
        struct btrfs_key k = { ino, BTRFS_EXTENT_DATA_KEY, 0 };
        struct btrfs_path p;
        int r = btrfs_search(fs, &fs->fs_tree, &k, &p);
        if (r < 0) return r;
        if (btrfs_path_normalize(fs, &p)) return 0;
        struct btrfs_key *ik = btrfs_path_key(&p);
        if (ik->objectid != ino || ik->type != BTRFS_EXTENT_DATA_KEY) return 0;
        struct btrfs_file_extent_item fe = extent_head(&p);
        uint64_t file_off = ik->offset;
        btrfs_delete_item(fs, &p);
        drop_extent_ref(fs, ino, file_off, &fe);
    }
    return 0;
}

/* Switch the inode over to new EXTENT_DATA items (sorted by offset) so that a failure keeps the old ones:
 * 1: insert the items whose key is free and grow old items whose key gets reused. Only this step can run
 *    out of space, and it is undone by deleting/shrinking those items again, which cannot.
 * 2: rewrite the reused items in place (shrinking is in place too), 3: delete the old items nothing replaced.
 * Old data extents are released only in 2 and 3, once the switch can no longer fail. */
struct new_extent { uint64_t off; const void *item; uint32_t size; };
#define KEY_WAS_FREE ((uint32_t)-1)
static int replace_extents(struct btrfs_fs *fs, uint64_t ino, const struct new_extent *ne, int n) {
    uint32_t *old = kmalloc_try(n * sizeof *old);   /* size of the old item at ne[i].off, or KEY_WAS_FREE */
    if (!old) return -ENOMEM;
    int i, r = 0;
    for (i = 0; i < n; i++) {
        struct btrfs_key k = { ino, BTRFS_EXTENT_DATA_KEY, ne[i].off };
        struct btrfs_path p;
        int s = btrfs_search(fs, &fs->fs_tree, &k, &p);
        if (s < 0) { r = s; break; }
        if (s == 1) { old[i] = KEY_WAS_FREE; if ((r = btrfs_insert_item(fs, &fs->fs_tree, &k, ne[i].item, ne[i].size, NULL)) < 0) break; continue; }
        old[i] = btrfs_path_size(&p);
        /* a shorter old item (inline) grows now: in place when its leaf has room, otherwise
         * btrfs_resize_item moves it and puts it back if that fails */
        if (old[i] < ne[i].size && (r = btrfs_resize_item(fs, &p, ne[i].size)) < 0) break;
    }
    if (r < 0) {
        for (int j = 0; j < i; j++) {
            struct btrfs_key k = { ino, BTRFS_EXTENT_DATA_KEY, ne[j].off };
            struct btrfs_path p;
            if (btrfs_search(fs, &fs->fs_tree, &k, &p) != 0) continue;
            if (old[j] == KEY_WAS_FREE) btrfs_delete_item(fs, &p);
            else if (btrfs_path_size(&p) > old[j]) btrfs_resize_item(fs, &p, old[j]);   /* keeps the head: the old item again */
        }
        kfree(old);
        return r;
    }
    for (i = 0; i < n; i++) {
        if (old[i] == KEY_WAS_FREE) continue;
        struct btrfs_key k = { ino, BTRFS_EXTENT_DATA_KEY, ne[i].off };
        struct btrfs_path p;
        if (btrfs_search(fs, &fs->fs_tree, &k, &p) != 0 || btrfs_path_size(&p) < ne[i].size) continue;   /* found (and grown) in step 1: cannot happen */
        struct btrfs_file_extent_item fe = extent_head(&p);
        if (btrfs_path_size(&p) > ne[i].size) btrfs_resize_item(fs, &p, ne[i].size);
        memcpy(btrfs_path_data(&p), ne[i].item, ne[i].size);
        btrfs_mark_dirty(fs, p.blocks[0]);
        drop_extent_ref(fs, ino, ne[i].off, &fe);
    }
    kfree(old);
    uint64_t from = 0; int j = 0;
    for (int guard = 0; guard < 1000000; guard++) {
        struct btrfs_key k = { ino, BTRFS_EXTENT_DATA_KEY, from };
        struct btrfs_path p;
        if (btrfs_search(fs, &fs->fs_tree, &k, &p) < 0 || btrfs_path_normalize(fs, &p)) break;
        struct btrfs_key *ik = btrfs_path_key(&p);
        if (ik->objectid != ino || ik->type != BTRFS_EXTENT_DATA_KEY) break;
        uint64_t off = ik->offset;
        while (j < n && ne[j].off < off) j++;
        if (j == n || ne[j].off != off) {
            struct btrfs_file_extent_item fe = extent_head(&p);
            btrfs_delete_item(fs, &p);
            drop_extent_ref(fs, ino, off, &fe);
        }
        if (off == (uint64_t)-1) break;
        from = off + 1;
    }
    return 0;
}

/* replace the inode's data. Regular extents are written and recorded first, and replace_extents only
 * drops the old items once the new ones are in, so ENOSPC/EIO leaves the old contents in place. */
static int write_content(struct btrfs_fs *fs, uint64_t ino, const uint8_t *data, uint64_t size, bool nodatasum, uint64_t *nbytes_out) {
    uint64_t nbytes = 0, root = fs->fs_tree.objectid;
    *nbytes_out = 0;
    if (size == 0) return remove_extents(fs, ino);
    if (size <= MAX_INLINE && size < fs->sectorsize) {
        uint32_t isz = BTRFS_FILE_EXTENT_INLINE_DATA_START + size;
        uint8_t *item = kmalloc(isz);
        struct btrfs_file_extent_item *fe = (struct btrfs_file_extent_item *)item;
        memset(fe, 0, BTRFS_FILE_EXTENT_INLINE_DATA_START);
        fe->generation = fs->transid; fe->ram_bytes = size; fe->type = 0;
        memcpy(item + BTRFS_FILE_EXTENT_INLINE_DATA_START, data, size);
        struct new_extent ne = { 0, item, isz };
        int r = replace_extents(fs, ino, &ne, 1);
        kfree(item);
        if (r < 0) return r;
        *nbytes_out = size;
        return 0;
    }
    #define PIECE_ALEN(i) ALIGN_UP(MIN(size - (i) * EXTENT_MAX, (uint64_t)EXTENT_MAX), fs->sectorsize)
    uint64_t npieces = (size + EXTENT_MAX - 1) / EXTENT_MAX, i;
    uint64_t *ext = kmalloc_try(npieces * sizeof *ext);
    uint8_t *tmp = kmalloc_try(EXTENT_MAX);
    struct btrfs_file_extent_item *fes = kmalloc_try(npieces * sizeof *fes);
    struct new_extent *ne = kmalloc_try(npieces * sizeof *ne);
    if (!ext || !tmp || !fes || !ne) { kfree(ext); kfree(tmp); kfree(fes); kfree(ne); return -ENOMEM; }
    int r = 0;
    for (i = 0; i < npieces; i++) {   /* 1: allocate, write and record the new extents */
        uint64_t off = i * EXTENT_MAX, len = MIN(size - off, (uint64_t)EXTENT_MAX), alen = PIECE_ALEN(i);
        uint64_t bytenr = btrfs_alloc_data(fs, alen);
        if (!bytenr) { r = -ENOSPC; break; }
        memcpy(tmp, data + off, len); if (alen > len) memset(tmp + len, 0, alen - len);
        if ((r = btrfs_write_logical(fs, bytenr, tmp, alen)) < 0) break;
        if ((r = btrfs_record_data_extent(fs, bytenr, alen, root, ino, off)) < 0) break;
        ext[i] = bytenr;
        if (!nodatasum && (r = btrfs_csum_insert(fs, bytenr, tmp, alen)) < 0) { i++; break; }
    }
    kfree(tmp);
    if (r == 0) {   /* 2: point the file at the new extents in place of the old ones */
        for (uint64_t j = 0; j < npieces; j++) {
            uint64_t alen = PIECE_ALEN(j);
            struct btrfs_file_extent_item *fe = &fes[j];
            memset(fe, 0, sizeof *fe);
            fe->generation = fs->transid; fe->ram_bytes = alen; fe->type = 1; fe->disk_bytenr = ext[j]; fe->disk_num_bytes = alen; fe->offset = 0; fe->num_bytes = alen;
            ne[j] = (struct new_extent){ j * EXTENT_MAX, fe, sizeof *fe };
            nbytes += alen;
        }
        r = replace_extents(fs, ino, ne, (int)npieces);
    }
    if (r < 0) { for (uint64_t j = 0; j < i; j++) btrfs_free_data_extent(fs, ext[j], PIECE_ALEN(j), root, ino, j * EXTENT_MAX); nbytes = 0; }   /* the file still uses the old ones */
    #undef PIECE_ALEN
    kfree(ext); kfree(fes); kfree(ne);
    *nbytes_out = nbytes;
    return r;
}

/* rewrite the whole file with new content (simple and always consistent) */
static int rewrite_file(struct btrfs_fs *fs, struct bnode *b, const uint8_t *content, uint64_t size) {
    struct btrfs_inode_item ii;
    int r = read_inode(fs, b->ino, &ii, NULL);
    if (r < 0) return r;
    btrfs_begin(fs);
    uint64_t nbytes = 0;
    r = write_content(fs, b->ino, content, size, (ii.flags & BTRFS_INODE_NODATASUM) != 0, &nbytes);
    if (r < 0) return r;
    ii.size = size; ii.nbytes = nbytes; ii.transid = fs->transid; ii.sequence++;
    ii.mtime.sec = b->vn.mtime; ii.mtime.nsec = b->vn.mtime_nsec;   /* when it was written, not when it was flushed */
    ii.ctime = now_ts();
    write_inode(fs, b->ino, &ii);
    fill_vnode(b, &ii);
    r = btrfs_commit(fs);
    update_space(fs);
    return r;
}

/* fs->lock held and the whole-file buffer loaded. */
static int64_t write_buffered(struct bnode *b, uint64_t off, const void *buf, size_t n) {
    if (off > WBUF_MAX || n > WBUF_MAX - off) return -EFBIG;
    uint64_t newsize = MAX(b->wsize, off + n);
    int r = grow_wbuf(b, newsize);
    if (r < 0) return r;
    if (off > b->wsize) memset(b->wbuf + b->wsize, 0, off - b->wsize);
    memcpy(b->wbuf + off, buf, n);
    b->wsize = newsize;
    if (!b->wdirty) { b->wdirty = true; b->wdirty_since = system_ticks; }
    b->vn.size = newsize; vnode_touch(&b->vn);
    return (int64_t)n;
}

static int64_t b_write(struct vnode *v, uint64_t off, const void *buf, size_t n) {
    struct bnode *b = (struct bnode *)v; struct btrfs_fs *fs = b->fs;
    if (fs->readonly) return -EROFS;
    if (n == 0) return 0;
    if (off > WBUF_MAX || n > WBUF_MAX - off) return -EFBIG;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    int64_t r = load_wbuf(fs, b);
    if (r == 0) r = write_buffered(b, off, buf, n);
    mutex_unlock(&fs->lock);
    return r;
}

static int64_t b_append(struct vnode *v, const void *buf, size_t n, uint64_t *end) {
    struct bnode *b = (struct bnode *)v; struct btrfs_fs *fs = b->fs;
    if (fs->readonly) return -EROFS;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    int64_t r = n ? load_wbuf(fs, b) : 0;
    if (r == 0) {
        /* Choose EOF under the same lock as the write, after loading any
         * existing data. Another appender cannot reuse this offset. */
        uint64_t off = n ? b->wsize : v->size;
        r = n ? write_buffered(b, off, buf, n) : 0;
        *end = r > 0 ? off + r : off;
    }
    mutex_unlock(&fs->lock);
    return r;
}

static int b_truncate(struct vnode *v, uint64_t size) {
    struct bnode *b = (struct bnode *)v; struct btrfs_fs *fs = b->fs;
    if (fs->readonly) return -EROFS;
    if (size > WBUF_MAX) return -EFBIG;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    int r = load_wbuf(fs, b);
    if (r < 0) { mutex_unlock(&fs->lock); return r; }
    if ((r = grow_wbuf(b, size)) < 0) { mutex_unlock(&fs->lock); return r; }
    if (size > b->wsize) memset(b->wbuf + b->wsize, 0, size - b->wsize);
    if (size != b->wsize) { b->wsize = size; if (!b->wdirty) { b->wdirty = true; b->wdirty_since = system_ticks; } b->vn.size = size; }
    vnode_touch(&b->vn);
    mutex_unlock(&fs->lock);
    return 0;
}

/* ---- create / unlink ---- */
static uint64_t next_dir_index(struct btrfs_fs *fs, uint64_t dir) {
    struct btrfs_key k = { dir, BTRFS_DIR_INDEX_KEY, (uint64_t)-1 };
    struct btrfs_path p;
    if (btrfs_search(fs, &fs->fs_tree, &k, &p) < 0) return 2;
    if (btrfs_prev_item(fs, &p)) return 2;
    struct btrfs_key *pk = btrfs_path_key(&p);
    if (pk->objectid == dir && pk->type == BTRFS_DIR_INDEX_KEY) return pk->offset + 1;
    return 2;
}
static int add_dir_entry(struct btrfs_fs *fs, uint64_t dir, const char *name, uint64_t ino, uint8_t ftype, uint64_t index) {
    size_t nl = strlen(name);
    uint32_t esz = sizeof(struct btrfs_dir_item) + nl;
    uint8_t *entry = kmalloc(esz);
    struct btrfs_dir_item *di = (struct btrfs_dir_item *)entry;
    di->location = (struct btrfs_key){ ino, BTRFS_INODE_ITEM_KEY, 0 };
    di->transid = fs->transid; di->data_len = 0; di->name_len = nl; di->type = ftype;
    memcpy(entry + sizeof *di, name, nl);
    /* DIR_ITEM: append to an existing hash bucket if present */
    struct btrfs_key k = { dir, BTRFS_DIR_ITEM_KEY, name_hash(name, nl) };
    struct btrfs_path p;
    int r = btrfs_search(fs, &fs->fs_tree, &k, &p);
    if (r == 0) {
        uint32_t old = btrfs_path_size(&p);
        r = btrfs_resize_item(fs, &p, old + esz);
        if (r == 0) { memcpy((uint8_t *)btrfs_path_data(&p) + old, entry, esz); btrfs_mark_dirty(fs, p.blocks[0]); }
    } else if (r == 1) r = btrfs_insert_item(fs, &fs->fs_tree, &k, entry, esz, NULL);
    if (r < 0) { kfree(entry); return r; }
    struct btrfs_key ik = { dir, BTRFS_DIR_INDEX_KEY, index };
    r = btrfs_insert_item(fs, &fs->fs_tree, &ik, entry, esz, NULL);
    kfree(entry);
    if (r < 0) return r;
    /* INODE_REF */
    uint32_t rsz = sizeof(struct btrfs_inode_ref) + nl;
    uint8_t *ref = kmalloc(rsz);
    struct btrfs_inode_ref *ir = (struct btrfs_inode_ref *)ref; ir->index = index; ir->name_len = nl; memcpy(ref + sizeof *ir, name, nl);
    struct btrfs_key rk = { ino, BTRFS_INODE_REF_KEY, dir };
    r = btrfs_search(fs, &fs->fs_tree, &rk, &p);
    if (r == 0) {   /* a hard link with another name in the same directory: one item holds both refs */
        uint32_t old = btrfs_path_size(&p);
        r = btrfs_resize_item(fs, &p, old + rsz);
        if (r == 0) { memcpy((uint8_t *)btrfs_path_data(&p) + old, ref, rsz); btrfs_mark_dirty(fs, p.blocks[0]); }
    } else if (r == 1) r = btrfs_insert_item(fs, &fs->fs_tree, &rk, ref, rsz, NULL);
    kfree(ref);
    return r;
}
static void touch_dir(struct btrfs_fs *fs, uint64_t dir, int64_t size_delta) {
    struct btrfs_inode_item ii;
    if (read_inode(fs, dir, &ii, NULL) < 0) return;
    ii.size += size_delta; ii.mtime = ii.ctime = now_ts(); ii.transid = fs->transid; ii.sequence++;
    write_inode(fs, dir, &ii);
}

static void undo_create(struct btrfs_fs *fs, uint64_t dir, const char *name, uint64_t ino, uint64_t index);
static struct vnode *b_create(struct vnode *dirv, const char *name, int type, uint32_t mode) {
    struct bnode *dir = (struct bnode *)dirv; struct btrfs_fs *fs = dir->fs;
    if (fs->readonly) return NULL;
    size_t nl = strlen(name);
    if (nl == 0 || nl > 255) return NULL;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    flush_all(fs, FLUSH_HEALTHY);
    uint64_t existing;
    if (dir_lookup(fs, dir->ino, name, &existing, NULL) == 0) { mutex_unlock(&fs->lock); return NULL; }
    btrfs_begin(fs);
    uint64_t ino = fs->next_ino++;
    struct btrfs_inode_item ii = { 0 };
    ii.generation = fs->transid; ii.transid = fs->transid; ii.nlink = 1; ii.uid = 0; ii.gid = 0;
    if (type != VN_DIR && type != VN_LNK && type != VN_FILE && type != VN_FIFO) { mutex_unlock(&fs->lock); return NULL; }
    ii.mode = type == VN_DIR ? (040000 | (mode & 07777)) : type == VN_LNK ? (0120000 | 0777) : type == VN_FIFO ? (010000 | (mode & 07777)) : (0100000 | (mode & 07777));
    ii.atime = ii.ctime = ii.mtime = ii.otime = now_ts();
    struct btrfs_key k = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    uint64_t index = next_dir_index(fs, dir->ino);
    int r = btrfs_insert_item(fs, &fs->fs_tree, &k, &ii, sizeof ii, NULL);
    if (r == 0) {
        r = add_dir_entry(fs, dir->ino, name, ino, type == VN_DIR ? BTRFS_FT_DIR : type == VN_LNK ? BTRFS_FT_SYMLINK : type == VN_FIFO ? BTRFS_FT_FIFO : BTRFS_FT_REG_FILE, index);
        if (r == 0) touch_dir(fs, dir->ino, 2 * nl);
        else undo_create(fs, dir->ino, name, ino, index);   /* the commit below must not write a half-made entry */
    }
    /* a failed commit keeps the blocks dirty for the next one, and the next sync/close reports it */
    int cr = btrfs_commit(fs);
    if (cr < 0) kprintf("btrfs: commit after creating %s failed (%d); will retry\n", name, cr);
    update_space(fs);
    refresh_vnode(dir);
    struct bnode *b = r == 0 ? get_bnode(fs, ino, name) : NULL;
    mutex_unlock(&fs->lock);
    return b ? &b->vn : NULL;
}

static int delete_key(struct btrfs_fs *fs, const struct btrfs_key *k) {
    struct btrfs_path p;
    int r = btrfs_search(fs, &fs->fs_tree, k, &p);
    if (r != 0) return r < 0 ? r : -ENOENT;
    return btrfs_delete_item(fs, &p);
}
/* remove the entry 'name' pointing at 'ino' from its DIR_ITEM hash bucket */
static int remove_dir_item_entry(struct btrfs_fs *fs, uint64_t dir, const char *name, uint64_t ino) {
    size_t nl = strlen(name);
    struct btrfs_key k = { dir, BTRFS_DIR_ITEM_KEY, name_hash(name, nl) };
    struct btrfs_path p;
    if (btrfs_search(fs, &fs->fs_tree, &k, &p) != 0) return -ENOENT;
    uint8_t *d = btrfs_path_data(&p); uint32_t size = btrfs_path_size(&p), pos = 0;
    while (pos + sizeof(struct btrfs_dir_item) <= size) {
        struct btrfs_dir_item *di = (struct btrfs_dir_item *)(d + pos);
        uint32_t esz = sizeof *di + di->name_len + di->data_len;
        if (pos + esz > size) return -EIO;
        if (di->name_len == nl && !memcmp(di + 1, name, nl) && di->location.objectid == ino) {
            if (esz >= size) return btrfs_delete_item(fs, &p);
            memmove(d + pos, d + pos + esz, size - pos - esz);
            btrfs_mark_dirty(fs, p.blocks[0]);
            return btrfs_resize_item(fs, &p, size - esz);
        }
        pos += esz;
    }
    return -ENOENT;
}
/* take back a create that failed half way. Only deletions and shrinks, which need no new space; the
 * index and dir entry are matched by inode so nothing that existed before is touched. */
static void undo_create(struct btrfs_fs *fs, uint64_t dir, const char *name, uint64_t ino, uint64_t index) {
    struct btrfs_key xk = { dir, BTRFS_DIR_INDEX_KEY, index }, rk = { ino, BTRFS_INODE_REF_KEY, dir }, ik = { ino, BTRFS_INODE_ITEM_KEY, 0 };
    struct btrfs_path p;
    if (btrfs_search(fs, &fs->fs_tree, &xk, &p) == 0 && btrfs_path_size(&p) >= sizeof(struct btrfs_dir_item) && ((struct btrfs_dir_item *)btrfs_path_data(&p))->location.objectid == ino) btrfs_delete_item(fs, &p);
    remove_dir_item_entry(fs, dir, name, ino);
    delete_key(fs, &rk);
    delete_key(fs, &ik);
}

/* removes the entry dir/name -> ino (DIR_ITEM, DIR_INDEX and INODE_REF), leaving the inode itself */
static void remove_entry(struct btrfs_fs *fs, uint64_t dir, const char *name, uint64_t ino) {
    size_t nl = strlen(name);
    /* find the index via INODE_REF */
    uint64_t index = 0; bool have_index = false;
    struct btrfs_key rk = { ino, BTRFS_INODE_REF_KEY, dir };
    struct btrfs_path p;
    if (btrfs_search(fs, &fs->fs_tree, &rk, &p) == 0) {
        uint8_t *d = btrfs_path_data(&p), *end = d + btrfs_path_size(&p);
        while (d + sizeof(struct btrfs_inode_ref) <= end) {
            struct btrfs_inode_ref *ir = (struct btrfs_inode_ref *)d;
            if (d + sizeof *ir + ir->name_len > end) break;
            if (ir->name_len == nl && !memcmp(ir + 1, name, nl)) { index = ir->index; have_index = true; break; }
            d += sizeof *ir + ir->name_len;
        }
        /* remove this ref (whole item if it was the only one) */
        if (have_index) {
            uint32_t size = btrfs_path_size(&p); uint8_t *base = btrfs_path_data(&p);
            uint32_t pos = (uint32_t)(d - base), esz = sizeof(struct btrfs_inode_ref) + (uint32_t)nl;
            if (esz >= size) btrfs_delete_item(fs, &p);
            else { memmove(base + pos, base + pos + esz, size - pos - esz); btrfs_mark_dirty(fs, p.blocks[0]); btrfs_resize_item(fs, &p, size - esz); }
        }
    }
    if (!have_index) {
        /* scan DIR_INDEX entries for the name */
        struct btrfs_key k = { dir, BTRFS_DIR_INDEX_KEY, 0 };
        if (btrfs_search(fs, &fs->fs_tree, &k, &p) >= 0) while (!btrfs_path_normalize(fs, &p)) {
            struct btrfs_key *ik = btrfs_path_key(&p);
            if (ik->objectid != dir || ik->type != BTRFS_DIR_INDEX_KEY) break;
            struct btrfs_dir_item *di = btrfs_path_data(&p);
            if (btrfs_path_size(&p) >= sizeof *di + nl && di->name_len == nl && !memcmp(di + 1, name, nl) && di->location.objectid == ino) { index = ik->offset; have_index = true; break; }
            if (btrfs_next_item(fs, &p)) break;
        }
    }
    remove_dir_item_entry(fs, dir, name, ino);
    if (have_index) { struct btrfs_key ik = { dir, BTRFS_DIR_INDEX_KEY, index }; delete_key(fs, &ik); }
}

/* one name fewer for ino: with the last one go the inode, its data and its cached vnode */
static void drop_link(struct btrfs_fs *fs, uint64_t ino) {
    struct btrfs_inode_item ii;
    if (read_inode(fs, ino, &ii, NULL) < 0) return;
    if (ii.nlink > 1) {
        ii.nlink--; ii.ctime = now_ts(); ii.transid = fs->transid; ii.sequence++;
        write_inode(fs, ino, &ii);
        uint32_t h = (uint32_t)ino % 256;
        for (struct bnode *b = fs->inode_cache[h]; b; b = b->hnext) if (b->ino == ino) fill_vnode(b, &ii);
        return;
    }
    remove_extents(fs, ino);
    /* xattrs and any leftovers */
    for (int guard = 0; guard < 1000; guard++) {
        struct btrfs_key k = { ino, 0, 0 }; struct btrfs_path q;
        if (btrfs_search(fs, &fs->fs_tree, &k, &q) < 0 || btrfs_path_normalize(fs, &q)) break;
        if (btrfs_path_key(&q)->objectid != ino) break;
        btrfs_delete_item(fs, &q);
    }
    { uint32_t h = (uint32_t)ino % 256; for (struct bnode *db = fs->inode_cache[h]; db; db = db->hnext) if (db->ino == ino) { db->wdirty = false; kfree(db->wbuf); db->wbuf = NULL; } }
    drop_bnode(fs, ino);
}

static int b_unlink(struct vnode *dirv, const char *name) {
    struct bnode *dir = (struct bnode *)dirv; struct btrfs_fs *fs = dir->fs;
    if (fs->readonly) return -EROFS;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    flush_all(fs, FLUSH_HEALTHY);
    uint64_t ino; uint8_t ftype;
    int r = dir_lookup(fs, dir->ino, name, &ino, &ftype);
    if (r < 0) { mutex_unlock(&fs->lock); return r; }
    btrfs_begin(fs);
    remove_entry(fs, dir->ino, name, ino);
    drop_link(fs, ino);
    touch_dir(fs, dir->ino, -2 * (int64_t)strlen(name));
    r = btrfs_commit(fs);
    update_space(fs);
    refresh_vnode(dir);
    mutex_unlock(&fs->lock);
    return r < 0 ? r : 0;
}

static uint8_t ftype_of(uint32_t mode) { uint32_t f = mode & 0170000; return f == 040000 ? BTRFS_FT_DIR : f == 0120000 ? BTRFS_FT_SYMLINK : f == 010000 ? BTRFS_FT_FIFO : BTRFS_FT_REG_FILE; }

static int b_link(struct vnode *dirv, const char *name, struct vnode *target) {
    struct bnode *dir = (struct bnode *)dirv, *tb = (struct bnode *)target; struct btrfs_fs *fs = dir->fs;
    if (fs->readonly) return -EROFS;
    size_t nl = strlen(name);
    if (nl == 0 || nl > 255) return -ENAMETOOLONG;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    flush_all(fs, FLUSH_HEALTHY);
    uint64_t existing;
    struct btrfs_inode_item ii;
    int r = 0;
    if (dir_lookup(fs, dir->ino, name, &existing, NULL) == 0) r = -EEXIST;
    else if (read_inode(fs, tb->ino, &ii, NULL) < 0) r = -ENOENT;
    else if (ii.nlink >= 65000) r = -31;   /* EMLINK */
    if (r < 0) { mutex_unlock(&fs->lock); return r; }
    btrfs_begin(fs);
    r = add_dir_entry(fs, dir->ino, name, tb->ino, ftype_of(ii.mode), next_dir_index(fs, dir->ino));
    if (r < 0) remove_entry(fs, dir->ino, name, tb->ino);   /* whatever part of the entry got in */
    else {
        ii.nlink++; ii.ctime = now_ts(); ii.transid = fs->transid; ii.sequence++;
        write_inode(fs, tb->ino, &ii);
        fill_vnode(tb, &ii);
        touch_dir(fs, dir->ino, 2 * (int64_t)nl);
    }
    int cr = btrfs_commit(fs);
    update_space(fs);
    refresh_vnode(dir);
    mutex_unlock(&fs->lock);
    return r < 0 ? r : cr < 0 ? cr : 0;
}

/* the vfs has checked the replacement (types, emptiness, not the same file) */
static int b_rename(struct vnode *odirv, const char *oname, struct vnode *ndirv, const char *nname) {
    struct bnode *od = (struct bnode *)odirv, *nd = (struct bnode *)ndirv; struct btrfs_fs *fs = od->fs;
    if (fs->readonly) return -EROFS;
    size_t nl = strlen(nname);
    if (nl == 0 || nl > 255) return -ENAMETOOLONG;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    flush_all(fs, FLUSH_HEALTHY);
    uint64_t ino, tino; uint8_t ftype, tftype = BTRFS_FT_REG_FILE;
    int r = dir_lookup(fs, od->ino, oname, &ino, &ftype);
    if (r < 0) { mutex_unlock(&fs->lock); return r; }
    bool replace = dir_lookup(fs, nd->ino, nname, &tino, &tftype) == 0;
    if (replace && tino == ino) { mutex_unlock(&fs->lock); return 0; }
    btrfs_begin(fs);
    /* the new entry first: if it can't be added (no space), nothing has changed */
    uint64_t index = next_dir_index(fs, nd->ino);
    if (replace) {   /* the replaced name's entries go first: the new DIR_ITEM may share their hash bucket */
        remove_entry(fs, nd->ino, nname, tino);
        touch_dir(fs, nd->ino, -2 * (int64_t)nl);
    }
    r = add_dir_entry(fs, nd->ino, nname, ino, ftype, index);
    if (r < 0) {
        remove_entry(fs, nd->ino, nname, ino);
        if (replace && add_dir_entry(fs, nd->ino, nname, tino, tftype, index) == 0) touch_dir(fs, nd->ino, 2 * (int64_t)nl);
        btrfs_commit(fs);
        mutex_unlock(&fs->lock);
        return r;
    }
    touch_dir(fs, nd->ino, 2 * (int64_t)nl);
    remove_entry(fs, od->ino, oname, ino);
    touch_dir(fs, od->ino, -2 * (int64_t)strlen(oname));
    if (replace) drop_link(fs, tino);
    struct btrfs_inode_item ii;
    if (read_inode(fs, ino, &ii, NULL) == 0) {
        ii.ctime = now_ts(); ii.transid = fs->transid; ii.sequence++;
        write_inode(fs, ino, &ii);
        uint32_t h = (uint32_t)ino % 256;
        for (struct bnode *b = fs->inode_cache[h]; b; b = b->hnext) if (b->ino == ino) { fill_vnode(b, &ii); strlcpy(b->vn.name, nname, sizeof b->vn.name); }
    }
    r = btrfs_commit(fs);
    update_space(fs);
    refresh_vnode(od); if (nd != od) refresh_vnode(nd);
    mutex_unlock(&fs->lock);
    return r < 0 ? r : 0;
}

static int b_setattr(struct vnode *v, const struct vattr *a) {
    struct bnode *b = (struct bnode *)v; struct btrfs_fs *fs = b->fs;
    if (fs->readonly) return -EROFS;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    struct btrfs_inode_item ii;
    int r = read_inode(fs, b->ino, &ii, NULL);
    if (r < 0) { mutex_unlock(&fs->lock); return r; }
    btrfs_begin(fs);
    if (a->valid & VA_MODE) ii.mode = (ii.mode & 0170000) | (a->mode & 07777);
    if (a->valid & VA_MTIME) {
        ii.mtime.sec = a->mtime; ii.mtime.nsec = a->mtime_nsec;
        b->vn.mtime = a->mtime; b->vn.mtime_nsec = a->mtime_nsec;   /* a later flush of buffered data keeps it */
    }
    ii.ctime = now_ts(); ii.transid = fs->transid; ii.sequence++;
    write_inode(fs, b->ino, &ii);
    fill_vnode(b, &ii);
    if (a->valid & VA_MTIME) { b->vn.mtime = a->mtime; b->vn.mtime_nsec = a->mtime_nsec; }
    r = btrfs_commit(fs);
    mutex_unlock(&fs->lock);
    return r < 0 ? r : 0;
}

static int b_sync(struct vnode *v) {
    struct bnode *b = (struct bnode *)v; struct btrfs_fs *fs = b->fs;
    mutex_lock(&fs->lock); btrfs_op_begin(fs);
    int r = flush_inode(fs, b);
    if (v == fs->root_vn) { int e = flush_all(fs, FLUSH_ALL); if (e < 0) r = e; }
    int c = btrfs_commit(fs); if (c < 0) r = c;
    mutex_unlock(&fs->lock);
    return r;
}
static void b_release(struct vnode *v) { /* inodes stay cached */ }

static const struct vnode_ops btrfs_ops = { b_read, b_write, b_truncate, b_lookup, b_readdir, b_create, b_unlink, NULL, b_release, b_sync, b_append, NULL, b_setattr, b_link, b_rename };

static uint64_t find_highest_ino(struct btrfs_fs *fs) {
    struct btrfs_key k = { BTRFS_LAST_FREE_OBJECTID, 0, 0 };
    struct btrfs_path p;
    if (btrfs_search(fs, &fs->fs_tree, &k, &p) < 0) return BTRFS_FIRST_FREE_OBJECTID;
    if (btrfs_prev_item(fs, &p)) { if (btrfs_path_normalize(fs, &p)) return BTRFS_FIRST_FREE_OBJECTID; }
    uint64_t id = btrfs_path_key(&p)->objectid;
    return id >= BTRFS_FIRST_FREE_OBJECTID && id < BTRFS_LAST_FREE_OBJECTID ? id : BTRFS_FIRST_FREE_OBJECTID;
}

struct vnode *btrfs_mount(struct blockdev *dev) {
    struct btrfs_fs *fs = btrfs_open(dev);
    if (!fs) return NULL;
    fs->next_ino = find_highest_ino(fs) + 1;
    struct bnode *root = get_bnode(fs, BTRFS_FIRST_FREE_OBJECTID, fs->sb.label[0] ? fs->sb.label : dev->name);
    if (!root) { kprintf("btrfs: %s: root directory inode missing\n", dev->name); return NULL; }
    root->vn.type = VN_DIR;
    fs->root_vn = &root->vn;
    update_space(fs);
    dev->fs_type = "btrfs";
    btrfs_register_flusher(fs);
    return &root->vn;
}

/* background flusher: writes back dirty files that have been idle for a while. Volumes leave the
 * list on btrfs_unmount, or by themselves once their disk is unplugged. */
#define MAX_FLUSH 16
static struct btrfs_fs *flush_list[MAX_FLUSH];
static struct mutex flush_lock = MUTEX_INIT;   /* taken before fs->lock, never while holding it */
static bool dev_gone(struct blockdev *d) { for (; d; d = d->parent) if (d->removed) return true; return false; }
static void flusher_thread(void *arg) {
    for (;;) {
        thread_sleep_ms(1000);
        mutex_lock(&flush_lock);
        for (int i = 0; i < MAX_FLUSH; i++) {
            struct btrfs_fs *fs = flush_list[i];
            if (!fs) continue;
            if (dev_gone(fs->dev)) { flush_list[i] = NULL; continue; }
            mutex_lock(&fs->lock); btrfs_op_begin(fs); flush_all(fs, FLUSH_IDLE); mutex_unlock(&fs->lock);
        }
        mutex_unlock(&flush_lock);
    }
}
static void btrfs_register_flusher(struct btrfs_fs *fs) {
    static bool started;
    mutex_lock(&flush_lock);
    int i, free_slot = -1;
    for (i = 0; i < MAX_FLUSH && flush_list[i] != fs; i++) if (!flush_list[i] && free_slot < 0) free_slot = i;
    if (i == MAX_FLUSH) { if (free_slot >= 0) flush_list[free_slot] = fs; else kprintf("btrfs: %s: no background flusher slot; files are written back on close/sync only\n", fs->dev->name); }
    if (!started) { started = true; thread_create(kernel_process, "btrfs-flush", flusher_thread, NULL); }
    mutex_unlock(&flush_lock);
}
/* after vfs_unmount (or a failed vfs_mount): final write-back, then leave the flusher. Returns the write-back error. */
int btrfs_unmount(struct vnode *root) {
    if (!root || !root->fs_name || strcmp(root->fs_name, "btrfs")) return 0;
    struct btrfs_fs *fs = root->fs;
    int r = fs->readonly || dev_gone(fs->dev) ? 0 : b_sync(root);
    mutex_lock(&flush_lock);
    for (int i = 0; i < MAX_FLUSH; i++) if (flush_list[i] == fs) flush_list[i] = NULL;
    mutex_unlock(&flush_lock);
    return r;
}
