#pragma once
#include "btrfs.h"
#include "blockdev.h"
#include "fs.h"
#include "sched.h"

#define MAX_CHUNKS 64
#define CACHE_BUCKETS 256
#define CACHE_MAX 384
#define MAX_PENDING 128
#define BTRFS_MAX_LEVEL 8
struct pending_block { uint64_t bytenr; int level; uint64_t owner; bool free, recording; };   /* recording: its extent item is being inserted */

struct chunk_map { uint64_t logical, length, type; int nstripes; uint64_t stripe_dev[4], stripe_phys[4]; };
struct tree_block { uint64_t logical; uint8_t *data; bool dirty, pinned; uint64_t lru; struct tree_block *hnext; };
struct btrfs_tree { uint64_t objectid, bytenr; int level; uint64_t generation; };
struct btrfs_path { struct btrfs_tree *tree; struct tree_block *blocks[BTRFS_MAX_LEVEL]; int slots[BTRFS_MAX_LEVEL]; int top; };

struct btrfs_fs {
    struct blockdev *dev;
    struct btrfs_super_block sb;
    uint32_t nodesize, sectorsize;
    struct chunk_map chunks[MAX_CHUNKS]; int nchunks;
    uint8_t chunk_tree_uuid[16];
    struct tree_block *cache[CACHE_BUCKETS]; int ncached; uint64_t lru_clock;
    uint64_t op_start;            /* lru_clock at the start of the current operation: blocks used since may sit in live paths */
    struct btrfs_tree root_tree, extent_tree, csum_tree, fs_tree, bg_tree, fst, chunk_tree, dev_tree;
    bool in_chunk_alloc;
    bool fst_ok;
    uint64_t transid;
    bool readonly, dirty_super, any_dirty;
    uint64_t alloc_hint[2];
    struct pending_block pending[MAX_PENDING]; int npending; bool in_flush;
    struct mutex lock;
    struct vnode *root_vn;
    void *inode_cache[256];
    uint64_t next_ino;
};

struct btrfs_fs *btrfs_open(struct blockdev *dev);
uint64_t btrfs_logical_to_phys(struct btrfs_fs *fs, uint64_t logical, int stripe);
int btrfs_chunk_stripes(struct btrfs_fs *fs, uint64_t logical);
int btrfs_write_logical(struct btrfs_fs *fs, uint64_t logical, const void *buf, size_t n);
struct tree_block *btrfs_read_block(struct btrfs_fs *fs, uint64_t logical);
struct tree_block *btrfs_new_block(struct btrfs_fs *fs, uint64_t logical, uint64_t owner, int level);
void btrfs_mark_dirty(struct btrfs_fs *fs, struct tree_block *b);
void btrfs_forget_block(struct btrfs_fs *fs, uint64_t logical);
int btrfs_cmp_key(const struct btrfs_key *a, const struct btrfs_key *b);
struct btrfs_item *btrfs_leaf_item(struct tree_block *b, int slot);
struct btrfs_key_ptr *btrfs_node_ptr(struct tree_block *b, int slot);
void *btrfs_item_data(struct tree_block *b, int slot);
int btrfs_nritems(struct tree_block *b);
int btrfs_level(struct tree_block *b);
uint32_t btrfs_leaf_free_space(struct btrfs_fs *fs, struct tree_block *b);
int btrfs_search(struct btrfs_fs *fs, struct btrfs_tree *tree, const struct btrfs_key *key, struct btrfs_path *p);
int btrfs_next_item(struct btrfs_fs *fs, struct btrfs_path *p);
int btrfs_prev_item(struct btrfs_fs *fs, struct btrfs_path *p);
int btrfs_path_normalize(struct btrfs_fs *fs, struct btrfs_path *p);
struct btrfs_key *btrfs_path_key(struct btrfs_path *p);
void *btrfs_path_data(struct btrfs_path *p);
uint32_t btrfs_path_size(struct btrfs_path *p);
int btrfs_load_tree(struct btrfs_fs *fs, uint64_t objectid, struct btrfs_tree *out);
int btrfs_insert_item(struct btrfs_fs *fs, struct btrfs_tree *tree, const struct btrfs_key *key, const void *data, uint32_t size, struct btrfs_path *out);
int btrfs_delete_item(struct btrfs_fs *fs, struct btrfs_path *p);
int btrfs_resize_item(struct btrfs_fs *fs, struct btrfs_path *p, uint32_t new_size);
uint64_t btrfs_alloc_extent(struct btrfs_fs *fs, uint64_t len, uint64_t type);
uint64_t btrfs_alloc_tree_block(struct btrfs_fs *fs, uint64_t owner);
void btrfs_set_metadata_level(struct btrfs_fs *fs, uint64_t bytenr, int level);
void btrfs_free_tree_block(struct btrfs_fs *fs, uint64_t bytenr, uint64_t owner, int level);
int btrfs_record_data_extent(struct btrfs_fs *fs, uint64_t bytenr, uint64_t len, uint64_t root, uint64_t ino, uint64_t file_off);
uint64_t btrfs_alloc_data(struct btrfs_fs *fs, uint64_t len);
int btrfs_free_data_extent(struct btrfs_fs *fs, uint64_t bytenr, uint64_t len, uint64_t root, uint64_t ino, uint64_t file_off);
int btrfs_csum_insert(struct btrfs_fs *fs, uint64_t bytenr, const void *data, uint64_t len);
int btrfs_csum_remove(struct btrfs_fs *fs, uint64_t bytenr, uint64_t len);
int btrfs_commit(struct btrfs_fs *fs);
void btrfs_begin(struct btrfs_fs *fs);
void btrfs_op_begin(struct btrfs_fs *fs);   /* call with fs->lock held, before any tree access of a new operation */
void btrfs_flush_pending(struct btrfs_fs *fs);
int btrfs_alloc_chunk(struct btrfs_fs *fs, uint64_t type, uint64_t want);
int btrfs_mkfs(struct blockdev *d, const char *label);
