/*
 * ext2/ext3/ext4, read-only: Linux volumes (a dual-boot system's disks, USB
 * drives formatted on Linux) can be browsed and copied from.
 *
 * Covers what mkfs.ext4 produces by default: 32- and 64-bit group
 * descriptors, extent trees (ext4) and the classic direct/indirect block map
 * (ext2/3), directories read linearly (hashed directories keep their entries
 * in ordinary blocks too), fast and slow symbolic links, and inline data.
 * Journals are not replayed: a volume that was not unmounted cleanly is
 * still read, as it is on disk.
 */
#include "fs.h"
#include "blockdev.h"
#include "mm.h"
#include "string.h"

struct ext4_super {
    uint32_t inodes_count, blocks_count_lo, r_blocks_count_lo, free_blocks_count_lo, free_inodes_count, first_data_block;
    uint32_t log_block_size, log_cluster_size, blocks_per_group, clusters_per_group, inodes_per_group, mtime, wtime;
    uint16_t mnt_count, max_mnt_count, magic, state, errors, minor_rev_level;
    uint32_t lastcheck, checkinterval, creator_os, rev_level;
    uint16_t def_resuid, def_resgid;
    uint32_t first_ino; uint16_t inode_size, block_group_nr;
    uint32_t feature_compat, feature_incompat, feature_ro_compat;
    uint8_t uuid[16]; char volume_name[16]; char last_mounted[64];
    uint32_t algorithm_usage_bitmap; uint8_t prealloc_blocks, prealloc_dir_blocks; uint16_t reserved_gdt_blocks;
    uint8_t journal_uuid[16]; uint32_t journal_inum, journal_dev, last_orphan, hash_seed[4];
    uint8_t def_hash_version, jnl_backup_type; uint16_t desc_size;
    uint32_t default_mount_opts, first_meta_bg, mkfs_time, jnl_blocks[17];
    uint32_t blocks_count_hi, r_blocks_count_hi, free_blocks_count_hi;
} PACKED;
#define EXT4_MAGIC 0xEF53
#define INCOMPAT_FILETYPE 0x2
#define INCOMPAT_EXTENTS 0x40
#define INCOMPAT_64BIT 0x80
#define INCOMPAT_INLINE_DATA 0x8000
#define INCOMPAT_SUPPORTED (0x2 | 0x4 /* recover */ | 0x8 /* journal_dev */ | 0x10 /* meta_bg */ | 0x40 | 0x80 | 0x200 /* flex_bg */ | 0x400 /* ea_inode */ | 0x1000 /* dirdata */ | 0x2000 /* csum_seed */ | 0x4000 /* largedir */ | 0x8000 | 0x10000 /* encrypt: names unreadable */ | 0x20000 /* casefold */)

struct ext4_inode {
    uint16_t mode, uid; uint32_t size_lo, atime, ctime, mtime, dtime; uint16_t gid, links_count;
    uint32_t blocks_lo, flags, osd1; uint32_t block[15]; uint32_t generation, file_acl_lo, size_high;
} PACKED;
#define EXT4_EXTENTS_FL 0x80000
#define EXT4_INLINE_DATA_FL 0x10000000

struct extent_header { uint16_t magic, entries, max, depth; uint32_t generation; } PACKED;
struct extent_idx { uint32_t block; uint32_t leaf_lo; uint16_t leaf_hi, unused; } PACKED;
struct extent { uint32_t block; uint16_t len, start_hi; uint32_t start_lo; } PACKED;

struct ext4_fs {
    struct blockdev *dev;
    uint32_t block_size, inode_size, inodes_per_group, groups, desc_size, first_data_block;
    uint64_t gdt_block;
    uint32_t incompat;
    struct mutex lock;
};
struct enode { struct vnode vn; struct ext4_fs *fs; uint32_t ino; struct ext4_inode in; uint8_t inode_raw[256]; };

static const struct vnode_ops ext4_ops;

static int read_block(struct ext4_fs *fs, uint64_t blk, void *buf) {
    return blockdev_read_bytes(fs->dev, blk * fs->block_size, buf, fs->block_size);
}

static int read_inode(struct ext4_fs *fs, uint32_t ino, struct enode *n) {
    if (!ino || ino > fs->inodes_per_group * fs->groups) return -EIO;
    uint32_t g = (ino - 1) / fs->inodes_per_group, idx = (ino - 1) % fs->inodes_per_group;
    uint8_t desc[64] = { 0 };
    if (blockdev_read_bytes(fs->dev, fs->gdt_block * fs->block_size + (uint64_t)g * fs->desc_size, desc, fs->desc_size) < 0) return -EIO;
    uint64_t table = *(uint32_t *)(desc + 8);
    if (fs->desc_size >= 64) table |= (uint64_t)*(uint32_t *)(desc + 0x28) << 32;
    size_t isz = MIN(fs->inode_size, (uint32_t)sizeof n->inode_raw);
    if (blockdev_read_bytes(fs->dev, table * fs->block_size + (uint64_t)idx * fs->inode_size, n->inode_raw, isz) < 0) return -EIO;
    memcpy(&n->in, n->inode_raw, sizeof n->in);
    n->ino = ino;
    return 0;
}

static uint64_t inode_size(const struct ext4_inode *in) { return in->size_lo | ((uint64_t)in->size_high << 32); }

/* physical block of logical block lb (0 = a hole) */
static uint64_t map_block(struct ext4_fs *fs, struct enode *n, uint64_t lb, uint8_t *tmp) {
    if (n->in.flags & EXT4_EXTENTS_FL) {
        const uint8_t *node = (const uint8_t *)n->in.block;
        size_t node_size = sizeof n->in.block;
        for (int depth = 0; depth < 6; depth++) {
            const struct extent_header *h = (const void *)node;
            if (h->magic != 0xF30A || (size_t)(h->entries + 1) * 12 > node_size) return 0;
            if (h->depth == 0) {
                const struct extent *e = (const void *)(h + 1);
                for (int i = 0; i < h->entries; i++) {
                    uint32_t len = e[i].len > 32768 ? e[i].len - 32768 : e[i].len;   /* > 32768: preallocated, reads as zeros */
                    if (lb >= e[i].block && lb < (uint64_t)e[i].block + len)
                        return e[i].len > 32768 ? 0 : (((uint64_t)e[i].start_hi << 32) | e[i].start_lo) + (lb - e[i].block);
                }
                return 0;
            }
            const struct extent_idx *ix = (const void *)(h + 1);
            int pick = -1;
            for (int i = 0; i < h->entries; i++) if (ix[i].block <= lb) pick = i;
            if (pick < 0) return 0;
            uint64_t child = ((uint64_t)ix[pick].leaf_hi << 32) | ix[pick].leaf_lo;
            if (read_block(fs, child, tmp) < 0) return 0;
            node = tmp; node_size = fs->block_size;
        }
        return 0;
    }
    /* direct, single, double and triple indirect blocks */
    uint32_t per = fs->block_size / 4;
    if (lb < 12) return n->in.block[lb];
    lb -= 12;
    uint32_t start; int levels;
    if (lb < per) { start = n->in.block[12]; levels = 1; }
    else if ((lb -= per) < (uint64_t)per * per) { start = n->in.block[13]; levels = 2; }
    else { lb -= (uint64_t)per * per; start = n->in.block[14]; levels = 3; }
    uint64_t blk = start;
    for (int l = levels - 1; l >= 0 && blk; l--) {
        if (read_block(fs, blk, tmp) < 0) return 0;
        uint64_t div = 1; for (int k = 0; k < l; k++) div *= per;
        blk = ((uint32_t *)tmp)[(lb / div) % per];
    }
    return blk;
}

static int64_t e_read(struct vnode *v, uint64_t off, void *buf, size_t len) {
    struct enode *n = (struct enode *)v;
    struct ext4_fs *fs = n->fs;
    uint64_t size = inode_size(&n->in);
    if (off >= size) return 0;
    len = MIN((uint64_t)len, size - off);
    /* fast symlinks and inline data live in the inode itself */
    bool in_inode = (v->type == VN_LNK && size < 60 && !(n->in.flags & EXT4_EXTENTS_FL)) || (n->in.flags & EXT4_INLINE_DATA_FL);
    if (in_inode) {
        size_t cap = sizeof n->in.block;
        if (off >= cap) return 0;
        len = MIN(len, cap - off);
        memcpy(buf, (const uint8_t *)n->in.block + off, len);
        return (int64_t)len;
    }
    uint8_t *blk = kmalloc_try(fs->block_size), *tmp = kmalloc_try(fs->block_size);
    if (!blk || !tmp) { kfree(blk); kfree(tmp); return -ENOMEM; }
    size_t done = 0;
    mutex_lock(&fs->lock);
    while (done < len) {
        uint64_t pos = off + done, lb = pos / fs->block_size;
        size_t in_blk = pos % fs->block_size, k = MIN(len - done, (size_t)fs->block_size - in_blk);
        uint64_t pb = map_block(fs, n, lb, tmp);
        if (!pb) memset((uint8_t *)buf + done, 0, k);   /* a hole */
        else if (read_block(fs, pb, blk) < 0) { mutex_unlock(&fs->lock); kfree(blk); kfree(tmp); return done ? (int64_t)done : -EIO; }
        else memcpy((uint8_t *)buf + done, blk + in_blk, k);
        done += k;
    }
    mutex_unlock(&fs->lock);
    kfree(blk); kfree(tmp);
    return (int64_t)done;
}

static int type_of(uint16_t mode) {
    switch (mode & 0170000) { case 0040000: return VN_DIR; case 0120000: return VN_LNK; case 0020000: return VN_DEV; case 0060000: return VN_BLK; }
    return VN_FILE;
}

static struct enode *make_node(struct ext4_fs *fs, uint32_t ino, const char *name) {
    struct enode *n = kzalloc_try(sizeof *n);
    if (!n) return NULL;
    if (read_inode(fs, ino, n) < 0) { kfree(n); return NULL; }
    n->fs = fs;
    strlcpy(n->vn.name, name, sizeof n->vn.name);
    n->vn.type = type_of(n->in.mode);
    n->vn.size = n->vn.type == VN_DIR ? 0 : inode_size(&n->in);
    n->vn.mtime = n->in.mtime; n->vn.mode = n->in.mode & 07777; n->vn.ino = ino;
    n->vn.ops = &ext4_ops; n->vn.fs = fs; n->vn.fs_name = "ext4"; n->vn.refcount = 1;
    return n;
}

/* walks the directory's entries; cb returns true to stop. Entry type comes from the dirent when the
 * volume has the filetype feature. */
typedef bool (*dirent_cb)(void *ctx, uint32_t ino, const char *name, size_t nl, uint8_t ftype);
static int iterate_dir(struct enode *d, dirent_cb cb, void *ctx) {
    struct ext4_fs *fs = d->fs;
    uint64_t size = inode_size(&d->in);
    if (d->in.flags & EXT4_INLINE_DATA_FL) return -ENOTSUP;
    uint8_t *blk = kmalloc_try(fs->block_size);
    if (!blk) return -ENOMEM;
    for (uint64_t off = 0; off < size; off += fs->block_size) {
        if (e_read(&d->vn, off, blk, fs->block_size) <= 0) break;
        for (uint32_t p = 0; p + 8 <= fs->block_size;) {
            uint32_t ino = *(uint32_t *)(blk + p);
            uint16_t rec = *(uint16_t *)(blk + p + 4);
            uint8_t nl = blk[p + 6], ft = blk[p + 7];
            if (rec < 8 || p + rec > fs->block_size) break;
            if (!(fs->incompat & INCOMPAT_FILETYPE)) ft = 0;
            if (ino && nl && 8u + nl <= rec && cb(ctx, ino, (const char *)blk + p + 8, nl, ft)) { kfree(blk); return 1; }
            p += rec;
        }
    }
    kfree(blk);
    return 0;
}

struct find_ctx { const char *name; size_t nl; uint32_t ino; };
static bool find_cb(void *c, uint32_t ino, const char *name, size_t nl, uint8_t ft) {
    struct find_ctx *f = c;
    if (nl == f->nl && !memcmp(name, f->name, nl)) { f->ino = ino; return true; }
    return false;
}
static struct vnode *e_lookup(struct vnode *dir, const char *name) {
    struct enode *d = (struct enode *)dir;
    struct find_ctx f = { name, strlen(name), 0 };
    if (iterate_dir(d, find_cb, &f) != 1) return NULL;
    struct enode *n = make_node(d->fs, f.ino, name);
    if (n) n->vn.parent = dir;
    return n ? &n->vn : NULL;
}

struct list_ctx { int want, at; struct dirent *out; struct ext4_fs *fs; bool found; };
static bool list_cb(void *c, uint32_t ino, const char *name, size_t nl, uint8_t ft) {
    struct list_ctx *l = c;
    if ((nl == 1 && name[0] == '.') || (nl == 2 && name[0] == '.' && name[1] == '.')) return false;
    if (l->at++ != l->want) return false;
    size_t k = MIN((size_t)nl, (size_t)NAME_MAX);
    memcpy(l->out->name, name, k); l->out->name[k] = 0;
    l->out->ino = ino;
    struct enode tmp;
    if (read_inode(l->fs, ino, &tmp) == 0) {
        l->out->type = type_of(tmp.in.mode);
        l->out->size = l->out->type == VN_DIR ? 0 : inode_size(&tmp.in);
        l->out->mtime = tmp.in.mtime;
    } else { l->out->type = ft == 2 ? VN_DIR : ft == 7 ? VN_LNK : VN_FILE; l->out->size = 0; l->out->mtime = 0; }
    l->found = true;
    return true;
}
static int e_readdir(struct vnode *dir, int index, struct dirent *out) {
    struct enode *d = (struct enode *)dir;
    struct list_ctx l = { index, 0, out, d->fs, false };
    iterate_dir(d, list_cb, &l);
    return l.found ? 0 : -1;
}

static void e_release(struct vnode *v) { kfree(v); }

static const struct vnode_ops ext4_ops = { e_read, NULL, NULL, e_lookup, e_readdir, NULL, NULL, NULL, e_release, NULL, NULL, NULL };

struct vnode *ext4_mount(struct blockdev *d) {
    struct ext4_super *sb = kmalloc(sizeof *sb);
    if (blockdev_read_bytes(d, 1024, sb, sizeof *sb) < 0 || sb->magic != EXT4_MAGIC) { kfree(sb); return NULL; }
    uint32_t bs = 1024u << sb->log_block_size;
    if (sb->log_block_size > 6 || !sb->inodes_per_group || !sb->blocks_per_group || sb->rev_level > 1) { kfree(sb); return NULL; }
    if (sb->feature_incompat & ~INCOMPAT_SUPPORTED) { kprintf("ext4: %s: unsupported features %x\n", d->name, sb->feature_incompat & ~INCOMPAT_SUPPORTED); kfree(sb); return NULL; }
    struct ext4_fs *fs = kzalloc(sizeof *fs);
    fs->dev = d; fs->block_size = bs;
    fs->inode_size = sb->rev_level ? sb->inode_size : 128;
    fs->inodes_per_group = sb->inodes_per_group;
    fs->first_data_block = sb->first_data_block;
    uint64_t blocks = sb->blocks_count_lo | ((sb->feature_incompat & INCOMPAT_64BIT) ? (uint64_t)sb->blocks_count_hi << 32 : 0);
    fs->groups = (uint32_t)((blocks - sb->first_data_block + sb->blocks_per_group - 1) / sb->blocks_per_group);
    fs->desc_size = (sb->feature_incompat & INCOMPAT_64BIT) && sb->desc_size >= 64 ? sb->desc_size : 32;
    fs->gdt_block = sb->first_data_block + 1;
    fs->incompat = sb->feature_incompat;
    mutex_init(&fs->lock);
    if (fs->inode_size < 128 || fs->inode_size > bs || blocks * bs > (d->sectors + 1) * d->sector_size + bs) { kprintf("ext4: %s: bad geometry\n", d->name); kfree(fs); kfree(sb); return NULL; }
    struct enode *root = make_node(fs, 2, "");
    if (!root || root->vn.type != VN_DIR) { kprintf("ext4: %s: no root directory\n", d->name); kfree(root); kfree(fs); kfree(sb); return NULL; }
    char label[17]; memcpy(label, sb->volume_name, 16); label[16] = 0;
    strlcpy(root->vn.name, label[0] ? label : d->name, sizeof root->vn.name);
    uint64_t free_blocks = sb->free_blocks_count_lo | ((sb->feature_incompat & INCOMPAT_64BIT) ? (uint64_t)sb->free_blocks_count_hi << 32 : 0);
    root->vn.fs_total = blocks * bs; root->vn.fs_free = free_blocks * bs;
    d->fs_type = "ext4";
    kprintf("ext4: %s: '%s' %lu MiB, %u-byte blocks%s%s (read-only)\n", d->name, label, (blocks * bs) >> 20, bs,
            sb->feature_incompat & INCOMPAT_EXTENTS ? ", extents" : "", sb->feature_incompat & INCOMPAT_64BIT ? ", 64-bit" : "");
    kfree(sb);
    return &root->vn;
}
