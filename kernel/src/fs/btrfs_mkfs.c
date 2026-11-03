/* In-kernel mkfs.btrfs: creates a single-device btrfs filesystem the way mkfs.btrfs does
 * (nodesize 16 KiB, sectorsize 4 KiB, skinny metadata, extended irefs, no-holes). */
#include "btrfs_internal.h"
#include "mm.h"
#include "string.h"

#define NODESIZE 16384
#define SECTORSIZE 4096
#define SYS_CHUNK_SIZE (4ull << 20)

struct leaf_builder { uint8_t *data; int n; uint32_t data_end; };
static void lb_init(struct leaf_builder *b, uint64_t bytenr, uint64_t owner, const uint8_t *fsid, const uint8_t *ctuuid) {
    b->data = kzalloc(NODESIZE); b->n = 0; b->data_end = NODESIZE - sizeof(struct btrfs_header);
    struct btrfs_header *h = (struct btrfs_header *)b->data;
    memcpy(h->fsid, fsid, 16); memcpy(h->chunk_tree_uuid, ctuuid, 16);
    h->bytenr = bytenr; h->flags = BTRFS_HEADER_FLAG_WRITTEN | BTRFS_HEADER_FLAG_MIXED_BACKREF; h->generation = 1; h->owner = owner; h->level = 0;
}
/* items must be appended in key order */
static void lb_add(struct leaf_builder *b, uint64_t objectid, uint8_t type, uint64_t offset, const void *data, uint32_t size) {
    struct btrfs_item *it = (struct btrfs_item *)(b->data + sizeof(struct btrfs_header)) + b->n;
    b->data_end -= size;
    it->key.objectid = objectid; it->key.type = type; it->key.offset = offset; it->offset = b->data_end; it->size = size;
    if (size) memcpy(b->data + sizeof(struct btrfs_header) + b->data_end, data, size);
    b->n++;
    ((struct btrfs_header *)b->data)->nritems = b->n;
}
static int lb_write(struct leaf_builder *b, struct blockdev *d, uint64_t phys) {
    uint32_t c = ~crc32c(~0u, b->data + BTRFS_CSUM_SIZE, NODESIZE - BTRFS_CSUM_SIZE);
    memcpy(b->data, &c, 4);
    int r = blockdev_write_bytes(d, phys, b->data, NODESIZE);
    kfree(b->data);
    return r;
}
static void random_uuid(uint8_t *u) {
    static uint64_t seed;
    seed ^= rdtsc() + time_unix() * 6364136223846793005ull;
    for (int i = 0; i < 16; i++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; u[i] = seed >> 56; }
    u[6] = (u[6] & 0x0F) | 0x40; u[8] = (u[8] & 0x3F) | 0x80;
}
static void init_times(struct btrfs_inode_item *ii) { uint64_t t = time_unix(); ii->atime.sec = ii->ctime.sec = ii->mtime.sec = ii->otime.sec = t; }

int btrfs_mkfs(struct blockdev *d, const char *label) {
    uint64_t dev_size = d->sectors * d->sector_size;
    if (dev_size < 64ull << 20) return -ENOSPC;
    uint8_t fsid[16], ctuuid[16], devuuid[16], fsuuid[16]; static const uint8_t zero_uuid[16] = { 0 };
    random_uuid(fsid); random_uuid(ctuuid); random_uuid(devuuid); random_uuid(fsuuid);
    /* chunk layout: system 4 MiB @1 MiB, metadata, data (logical == physical for simplicity, like mkfs) */
    uint64_t meta_size = MIN(256ull << 20, ALIGN_DOWN(dev_size / 8, 1 << 20)), data_size = MIN(1ull << 30, ALIGN_DOWN(dev_size / 4, 1 << 20));
    if (meta_size < 8ull << 20) meta_size = 8ull << 20;
    if (data_size < 8ull << 20) data_size = 8ull << 20;
    uint64_t sys_start = 1ull << 20, meta_start = sys_start + SYS_CHUNK_SIZE, data_start = meta_start + meta_size;
    if (data_start + data_size > dev_size) return -ENOSPC;
    /* tree blocks */
    uint64_t chunk_root = sys_start, root_root = meta_start, extent_root = meta_start + NODESIZE, dev_root = meta_start + 2 * NODESIZE,
             fs_root = meta_start + 3 * NODESIZE, csum_root = meta_start + 4 * NODESIZE;
    struct btrfs_dev_item dev = { 0 };
    dev.devid = 1; dev.total_bytes = dev_size; dev.bytes_used = SYS_CHUNK_SIZE + meta_size + data_size;
    dev.io_align = SECTORSIZE; dev.io_width = SECTORSIZE; dev.sector_size = SECTORSIZE; dev.generation = 0;
    memcpy(dev.uuid, devuuid, 16); memcpy(dev.fsid, fsid, 16);
    struct { struct btrfs_chunk c; struct btrfs_stripe s; } PACKED chunks[3];
    uint64_t chunk_logical[3] = { sys_start, meta_start, data_start }, chunk_len[3] = { SYS_CHUNK_SIZE, meta_size, data_size };
    uint64_t chunk_type[3] = { BTRFS_BLOCK_GROUP_SYSTEM, BTRFS_BLOCK_GROUP_METADATA, BTRFS_BLOCK_GROUP_DATA };
    for (int i = 0; i < 3; i++) {
        memset(&chunks[i], 0, sizeof chunks[i]);
        chunks[i].c.length = chunk_len[i]; chunks[i].c.owner = BTRFS_EXTENT_TREE_OBJECTID; chunks[i].c.stripe_len = 65536; chunks[i].c.type = chunk_type[i];
        chunks[i].c.io_align = 65536; chunks[i].c.io_width = 65536; chunks[i].c.sector_size = SECTORSIZE; chunks[i].c.num_stripes = 1; chunks[i].c.sub_stripes = 1;
        chunks[i].s.devid = 1; chunks[i].s.offset = chunk_logical[i]; memcpy(chunks[i].s.dev_uuid, devuuid, 16);
    }
    int r;
    /* chunk tree */
    struct leaf_builder b;
    lb_init(&b, chunk_root, BTRFS_CHUNK_TREE_OBJECTID, fsid, ctuuid);
    lb_add(&b, 1, BTRFS_DEV_ITEM_KEY, 1, &dev, sizeof dev);
    for (int i = 0; i < 3; i++) lb_add(&b, BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_CHUNK_ITEM_KEY, chunk_logical[i], &chunks[i], sizeof chunks[i]);
    if ((r = lb_write(&b, d, chunk_root)) < 0) return r;
    /* root tree */
    struct btrfs_root_item ri;
    #define ROOT_ITEM(var, bytenr_, dirid, uuidp) do { memset(&var, 0, sizeof var); var.inode.generation = 1; var.inode.size = 3; var.inode.nbytes = NODESIZE; var.inode.nlink = 1; var.inode.mode = 040755; var.inode.flags = 1ull << 31; \
        var.generation = 1; var.root_dirid = dirid; var.bytenr = bytenr_; var.bytes_used = NODESIZE; var.refs = 1; var.level = 0; var.generation_v2 = 1; memcpy(var.uuid, uuidp, 16); var.ctransid = 1; var.otransid = 1; init_times(&var.inode); var.ctime.sec = var.otime.sec = time_unix(); } while (0)
    lb_init(&b, root_root, BTRFS_ROOT_TREE_OBJECTID, fsid, ctuuid);
    ROOT_ITEM(ri, extent_root, 0, zero_uuid); lb_add(&b, BTRFS_EXTENT_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0, &ri, sizeof ri);
    ROOT_ITEM(ri, dev_root, 0, zero_uuid); lb_add(&b, BTRFS_DEV_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0, &ri, sizeof ri);
    ROOT_ITEM(ri, fs_root, BTRFS_FIRST_FREE_OBJECTID, fsuuid); lb_add(&b, BTRFS_FS_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0, &ri, sizeof ri);
    { struct btrfs_inode_item dir = { 0 }; dir.generation = 1; dir.transid = 1; dir.size = 0; dir.nlink = 1; dir.mode = 040755; init_times(&dir);
      lb_add(&b, BTRFS_ROOT_TREE_DIR_OBJECTID, BTRFS_INODE_ITEM_KEY, 0, &dir, sizeof dir);
      uint8_t di[sizeof(struct btrfs_dir_item) + 7]; struct btrfs_dir_item *dp = (struct btrfs_dir_item *)di;
      dp->location = (struct btrfs_key){ BTRFS_FS_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, (uint64_t)-1 }; dp->transid = 1; dp->data_len = 0; dp->name_len = 7; dp->type = BTRFS_FT_DIR; memcpy(di + sizeof *dp, "default", 7);
      lb_add(&b, BTRFS_ROOT_TREE_DIR_OBJECTID, BTRFS_DIR_ITEM_KEY, crc32c((uint32_t)~1, "default", 7), di, sizeof di); }
    ROOT_ITEM(ri, csum_root, 0, zero_uuid); lb_add(&b, BTRFS_CSUM_TREE_OBJECTID, BTRFS_ROOT_ITEM_KEY, 0, &ri, sizeof ri);
    if ((r = lb_write(&b, d, root_root)) < 0) return r;
    /* extent tree: block groups + metadata items, in key order */
    lb_init(&b, extent_root, BTRFS_EXTENT_TREE_OBJECTID, fsid, ctuuid);
    struct btrfs_block_group_item bg;
    uint8_t mi[33]; struct btrfs_extent_item *ei = (struct btrfs_extent_item *)mi; ei->refs = 1; ei->generation = 1; ei->flags = BTRFS_EXTENT_FLAG_TREE_BLOCK; mi[24] = BTRFS_TREE_BLOCK_REF_KEY;
    #define META_ITEM(bytenr_, owner_) do { uint64_t o = owner_; memcpy(mi + 25, &o, 8); lb_add(&b, bytenr_, BTRFS_METADATA_ITEM_KEY, 0, mi, 33); } while (0)
    /* key order: (objectid, type): METADATA_ITEM (169) sorts before BLOCK_GROUP_ITEM (192) at the same address */
    META_ITEM(chunk_root, BTRFS_CHUNK_TREE_OBJECTID);
    bg = (struct btrfs_block_group_item){ NODESIZE, BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_BLOCK_GROUP_SYSTEM }; lb_add(&b, sys_start, BTRFS_BLOCK_GROUP_ITEM_KEY, SYS_CHUNK_SIZE, &bg, sizeof bg);
    META_ITEM(root_root, BTRFS_ROOT_TREE_OBJECTID);
    bg = (struct btrfs_block_group_item){ 5 * NODESIZE, BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_BLOCK_GROUP_METADATA }; lb_add(&b, meta_start, BTRFS_BLOCK_GROUP_ITEM_KEY, meta_size, &bg, sizeof bg);
    META_ITEM(extent_root, BTRFS_EXTENT_TREE_OBJECTID); META_ITEM(dev_root, BTRFS_DEV_TREE_OBJECTID);
    META_ITEM(fs_root, BTRFS_FS_TREE_OBJECTID); META_ITEM(csum_root, BTRFS_CSUM_TREE_OBJECTID);
    bg = (struct btrfs_block_group_item){ 0, BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_BLOCK_GROUP_DATA }; lb_add(&b, data_start, BTRFS_BLOCK_GROUP_ITEM_KEY, data_size, &bg, sizeof bg);
    if ((r = lb_write(&b, d, extent_root)) < 0) return r;
    /* dev tree */
    lb_init(&b, dev_root, BTRFS_DEV_TREE_OBJECTID, fsid, ctuuid);
    for (int i = 0; i < 3; i++) { struct btrfs_dev_extent de = { BTRFS_CHUNK_TREE_OBJECTID, BTRFS_FIRST_CHUNK_TREE_OBJECTID, chunk_logical[i], chunk_len[i] }; memcpy(de.chunk_tree_uuid, ctuuid, 16); lb_add(&b, 1, BTRFS_DEV_EXTENT_KEY, chunk_logical[i], &de, sizeof de); }
    if ((r = lb_write(&b, d, dev_root)) < 0) return r;
    /* fs tree: root directory */
    lb_init(&b, fs_root, BTRFS_FS_TREE_OBJECTID, fsid, ctuuid);
    { struct btrfs_inode_item dir = { 0 }; dir.generation = 1; dir.transid = 1; dir.size = 0; dir.nbytes = NODESIZE; dir.nlink = 1; dir.mode = 040755; init_times(&dir);
      lb_add(&b, BTRFS_FIRST_FREE_OBJECTID, BTRFS_INODE_ITEM_KEY, 0, &dir, sizeof dir);
      uint8_t ref[sizeof(struct btrfs_inode_ref) + 2]; struct btrfs_inode_ref *rp = (struct btrfs_inode_ref *)ref; rp->index = 0; rp->name_len = 2; ref[sizeof *rp] = '.'; ref[sizeof *rp + 1] = '.';
      lb_add(&b, BTRFS_FIRST_FREE_OBJECTID, BTRFS_INODE_REF_KEY, BTRFS_FIRST_FREE_OBJECTID, ref, sizeof ref); }
    if ((r = lb_write(&b, d, fs_root)) < 0) return r;
    /* csum tree: empty */
    lb_init(&b, csum_root, BTRFS_CSUM_TREE_OBJECTID, fsid, ctuuid);
    if ((r = lb_write(&b, d, csum_root)) < 0) return r;
    /* superblock */
    struct btrfs_super_block *sb = kzalloc(sizeof *sb);
    memcpy(sb->fsid, fsid, 16); sb->bytenr = BTRFS_SUPER_OFFSET; sb->flags = 1; memcpy(sb->magic, BTRFS_MAGIC, 8);
    sb->generation = 1; sb->root = root_root; sb->chunk_root = chunk_root; sb->total_bytes = dev_size; sb->bytes_used = 6 * NODESIZE;
    sb->root_dir_objectid = BTRFS_ROOT_TREE_DIR_OBJECTID; sb->num_devices = 1;
    sb->sectorsize = SECTORSIZE; sb->nodesize = NODESIZE; sb->leafsize = NODESIZE; sb->stripesize = SECTORSIZE;
    sb->chunk_root_generation = 1; sb->incompat_flags = 0x1 | BTRFS_FEATURE_INCOMPAT_EXTENDED_IREF | BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA | BTRFS_FEATURE_INCOMPAT_NO_HOLES;
    sb->csum_type = 0; sb->dev_item = dev; strlcpy(sb->label, label, sizeof sb->label); sb->cache_generation = (uint64_t)-1;
    /* sys chunk array: key + chunk for the system chunk */
    struct btrfs_key ck = { BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_CHUNK_ITEM_KEY, sys_start };
    memcpy(sb->sys_chunk_array, &ck, sizeof ck); memcpy(sb->sys_chunk_array + sizeof ck, &chunks[0], sizeof chunks[0]);
    sb->sys_chunk_array_size = sizeof ck + sizeof chunks[0];
    uint32_t c = ~crc32c(~0u, (uint8_t *)sb + BTRFS_CSUM_SIZE, sizeof *sb - BTRFS_CSUM_SIZE);
    memcpy(sb->csum, &c, 4);
    /* clear the other superblock mirror slots so stale data is never picked up; the trees must be on the medium before the superblock */
    uint8_t *z = kzalloc(4096);
    if (0x4000000 + 4096 <= dev_size) r = blockdev_write_bytes(d, 0x4000000, z, 4096);
    if (r == 0 && 0x4000000000ull + 4096 <= dev_size) r = blockdev_write_bytes(d, 0x4000000000ull, z, 4096);
    if (r == 0) r = blockdev_flush(d);
    if (r == 0) r = blockdev_write_bytes(d, BTRFS_SUPER_OFFSET, sb, sizeof *sb);
    if (r == 0) r = blockdev_flush(d);
    kfree(z); kfree(sb);
    if (r < 0) { kprintf("btrfs: formatting %s failed (%d)\n", d->name, r); return r; }
    kprintf("btrfs: formatted %s as '%s' (%lu MiB, chunks: system 4 MiB, metadata %lu MiB, data %lu MiB)\n", d->name, label, dev_size >> 20, meta_size >> 20, data_size >> 20);
    return r;
}
