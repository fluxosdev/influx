/*
 * btrfs core: superblock, logical->physical chunk mapping, tree block cache,
 * B-tree search/iteration and (in-place) modification with leaf/node splits,
 * extent allocation/freeing, checksums and transaction commit.
 *
 * Tree blocks are modified in place (not copy-on-write). Every commit rewrites
 * the superblock with a new generation, keeps the extent tree consistent and
 * marks the free space tree as invalid so Linux rebuilds it on next mount.
 */
#include "btrfs_internal.h"
#include "mm.h"
#include "string.h"

/* ------------------------------------------------------------ chunk map */
static void add_chunk(struct btrfs_fs *fs, uint64_t logical, const struct btrfs_chunk *c, uint32_t size) {
    if (size < sizeof *c || !c->num_stripes || size < sizeof *c + (uint64_t)c->num_stripes * sizeof(struct btrfs_stripe) || !c->length || logical + c->length < logical) {
        kprintf("btrfs: malformed chunk item at %lx, mounting read-only\n", logical); fs->readonly = true; return;
    }
    if (fs->nchunks >= MAX_CHUNKS) { if (!fs->readonly) kprintf("btrfs: more than %d chunks, mounting read-only\n", MAX_CHUNKS); fs->readonly = true; return; }
    struct chunk_map *m = &fs->chunks[fs->nchunks++];
    m->logical = logical; m->length = c->length; m->type = c->type;
    m->nstripes = MIN(c->num_stripes, 4);
    for (int i = 0; i < m->nstripes; i++) { m->stripe_dev[i] = c->stripe[i].devid; m->stripe_phys[i] = c->stripe[i].offset; }
}
static struct chunk_map *find_chunk(struct btrfs_fs *fs, uint64_t logical) {
    for (int i = 0; i < fs->nchunks; i++) if (logical >= fs->chunks[i].logical && logical < fs->chunks[i].logical + fs->chunks[i].length) return &fs->chunks[i];
    return NULL;
}
uint64_t btrfs_logical_to_phys(struct btrfs_fs *fs, uint64_t logical, int stripe) {
    struct chunk_map *m = find_chunk(fs, logical);
    if (!m || stripe >= m->nstripes) return 0;
    return m->stripe_phys[stripe] + (logical - m->logical);
}
int btrfs_chunk_stripes(struct btrfs_fs *fs, uint64_t logical) { struct chunk_map *m = find_chunk(fs, logical); return m ? m->nstripes : 0; }

static int read_logical(struct btrfs_fs *fs, uint64_t logical, void *buf, size_t n) {
    uint64_t phys = btrfs_logical_to_phys(fs, logical, 0);
    if (!phys) return -EIO;
    return blockdev_read_bytes(fs->dev, phys, buf, n);
}
int btrfs_write_logical(struct btrfs_fs *fs, uint64_t logical, const void *buf, size_t n) {
    int ns = btrfs_chunk_stripes(fs, logical);
    if (!ns) return -EIO;
    struct chunk_map *m = find_chunk(fs, logical);
    if (m->type & (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID10 | BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6)) return -EROFS;
    for (int s = 0; s < ns; s++) {
        uint64_t phys = btrfs_logical_to_phys(fs, logical, s);
        int r = blockdev_write_bytes(fs->dev, phys, buf, n);
        if (r < 0) return r;
    }
    return 0;
}

/* ------------------------------------------------------------ tree block cache */
static struct tree_block *cache_lookup(struct btrfs_fs *fs, uint64_t logical) {
    uint32_t h = (uint32_t)(logical / fs->nodesize) % CACHE_BUCKETS;
    for (struct tree_block *b = fs->cache[h]; b; b = b->hnext) if (b->logical == logical) { b->lru = ++fs->lru_clock; return b; }
    return NULL;
}
static void cache_insert(struct btrfs_fs *fs, struct tree_block *b) {
    uint32_t h = (uint32_t)(b->logical / fs->nodesize) % CACHE_BUCKETS;
    b->hnext = fs->cache[h]; fs->cache[h] = b; fs->ncached++;
    b->lru = ++fs->lru_clock;
}
static void cache_remove(struct btrfs_fs *fs, struct tree_block *b) {
    uint32_t h = (uint32_t)(b->logical / fs->nodesize) % CACHE_BUCKETS;
    for (struct tree_block **p = &fs->cache[h]; *p; p = &(*p)->hnext) if (*p == b) { *p = b->hnext; break; }
    fs->ncached--;
    kfree(b->data); kfree(b);
}
/* blocks touched during the current operation may be referenced by a live btrfs_path: never evict those */
static void cache_evict(struct btrfs_fs *fs) {
    while (fs->ncached > CACHE_MAX) {
        struct tree_block *victim = NULL;
        for (int i = 0; i < CACHE_BUCKETS; i++) for (struct tree_block *b = fs->cache[i]; b; b = b->hnext) if (!b->dirty && !b->pinned && b->lru < fs->op_start && (!victim || b->lru < victim->lru)) victim = b;
        if (!victim) break;
        cache_remove(fs, victim);
    }
}
void btrfs_op_begin(struct btrfs_fs *fs) { fs->op_start = fs->lru_clock + 1; cache_evict(fs); }
static uint32_t block_csum(struct btrfs_fs *fs, const uint8_t *data) { return ~crc32c(~0u, data + BTRFS_CSUM_SIZE, fs->nodesize - BTRFS_CSUM_SIZE); }
/* structural checks (like Linux's tree-checker) so later code can trust counts and item bounds */
static bool block_ok(struct btrfs_fs *fs, struct tree_block *b) {
    struct btrfs_header *h = (struct btrfs_header *)b->data;
    uint32_t n = h->nritems, space = fs->nodesize - sizeof(struct btrfs_header);
    if (h->level >= BTRFS_MAX_LEVEL) return false;
    if (h->level) return n >= 1 && n <= space / sizeof(struct btrfs_key_ptr);
    if ((uint64_t)n * sizeof(struct btrfs_item) > space) return false;
    uint32_t end = space;
    for (uint32_t i = 0; i < n; i++) { struct btrfs_item *it = btrfs_leaf_item(b, i); if ((uint64_t)it->offset + it->size != end) return false; end = it->offset; }
    return end >= n * sizeof(struct btrfs_item);
}

struct tree_block *btrfs_read_block(struct btrfs_fs *fs, uint64_t logical) {
    struct tree_block *b = cache_lookup(fs, logical);
    if (b) return b;
    b = kzalloc(sizeof *b);
    b->data = kmalloc(fs->nodesize);
    b->logical = logical;
    if (read_logical(fs, logical, b->data, fs->nodesize) < 0) { kfree(b->data); kfree(b); return NULL; }
    struct btrfs_header *h = (struct btrfs_header *)b->data;
    if (h->bytenr != logical) { kprintf("btrfs: block %lx has bytenr %lx\n", logical, h->bytenr); kfree(b->data); kfree(b); return NULL; }
    uint32_t want; memcpy(&want, h->csum, 4);
    if (want != block_csum(fs, b->data)) { kprintf("btrfs: checksum mismatch on tree block %lx\n", logical); kfree(b->data); kfree(b); return NULL; }
    if (!block_ok(fs, b)) { kprintf("btrfs: corrupt tree block %lx\n", logical); kfree(b->data); kfree(b); return NULL; }
    cache_evict(fs);
    cache_insert(fs, b);
    return b;
}
void btrfs_mark_dirty(struct btrfs_fs *fs, struct tree_block *b) { b->dirty = true; }

struct tree_block *btrfs_new_block(struct btrfs_fs *fs, uint64_t logical, uint64_t owner, int level) {
    struct tree_block *b = cache_lookup(fs, logical);
    if (!b) { b = kzalloc(sizeof *b); b->data = kmalloc(fs->nodesize); b->logical = logical; cache_evict(fs); cache_insert(fs, b); }
    memset(b->data, 0, fs->nodesize);
    struct btrfs_header *h = (struct btrfs_header *)b->data;
    memcpy(h->fsid, fs->sb.fsid, 16);
    if (fs->sb.incompat_flags & BTRFS_FEATURE_INCOMPAT_METADATA_UUID) memcpy(h->fsid, fs->sb.metadata_uuid, 16);
    memcpy(h->chunk_tree_uuid, fs->sb.dev_item.uuid, 16);   /* overwritten below */
    memcpy(h->chunk_tree_uuid, fs->chunk_tree_uuid, 16);
    h->bytenr = logical; h->flags = BTRFS_HEADER_FLAG_WRITTEN | BTRFS_HEADER_FLAG_MIXED_BACKREF; h->generation = fs->transid; h->owner = owner; h->nritems = 0; h->level = level;
    b->dirty = true;
    return b;
}
void btrfs_forget_block(struct btrfs_fs *fs, uint64_t logical) { struct tree_block *b = cache_lookup(fs, logical); if (b) cache_remove(fs, b); }

/* ------------------------------------------------------------ keys / leaves */
int btrfs_cmp_key(const struct btrfs_key *a, const struct btrfs_key *b) {
    if (a->objectid != b->objectid) return a->objectid < b->objectid ? -1 : 1;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    return 0;
}
static inline struct btrfs_header *hdr(struct tree_block *b) { return (struct btrfs_header *)b->data; }
struct btrfs_item *btrfs_leaf_item(struct tree_block *b, int slot) { return (struct btrfs_item *)(b->data + sizeof(struct btrfs_header)) + slot; }
struct btrfs_key_ptr *btrfs_node_ptr(struct tree_block *b, int slot) { return (struct btrfs_key_ptr *)(b->data + sizeof(struct btrfs_header)) + slot; }
void *btrfs_item_data(struct tree_block *b, int slot) { return b->data + sizeof(struct btrfs_header) + btrfs_leaf_item(b, slot)->offset; }
int btrfs_nritems(struct tree_block *b) { return hdr(b)->nritems; }
int btrfs_level(struct tree_block *b) { return hdr(b)->level; }
static uint32_t leaf_data_size(struct btrfs_fs *fs) { return fs->nodesize - sizeof(struct btrfs_header); }
uint32_t btrfs_leaf_free_space(struct btrfs_fs *fs, struct tree_block *b) {
    int n = btrfs_nritems(b);
    uint32_t data_start = n ? btrfs_leaf_item(b, n - 1)->offset : leaf_data_size(fs);
    uint32_t items_end = n * sizeof(struct btrfs_item);
    return data_start > items_end ? data_start - items_end : 0;
}
static int node_capacity(struct btrfs_fs *fs) { return leaf_data_size(fs) / sizeof(struct btrfs_key_ptr); }

/* binary search in a leaf (items) or node (key ptrs). Returns slot of first key >= wanted. */
static int bin_search(struct tree_block *b, const struct btrfs_key *key, int *exact) {
    int n = btrfs_nritems(b), lo = 0, hi = n;
    bool leaf = btrfs_level(b) == 0;
    *exact = 0;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        const struct btrfs_key *k = leaf ? &btrfs_leaf_item(b, mid)->key : &btrfs_node_ptr(b, mid)->key;
        int c = btrfs_cmp_key(k, key);
        if (c == 0) { *exact = 1; return mid; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* ------------------------------------------------------------ search / iterate */
int btrfs_search(struct btrfs_fs *fs, struct btrfs_tree *tree, const struct btrfs_key *key, struct btrfs_path *p) {
    memset(p, 0, sizeof *p);
    uint64_t bytenr = tree->bytenr; int level = tree->level;
    if (level < 0 || level >= BTRFS_MAX_LEVEL) { kprintf("btrfs: tree %lu has bad level %d\n", tree->objectid, level); return -EIO; }
    p->tree = tree; p->top = level;
    for (;;) {
        struct tree_block *b = btrfs_read_block(fs, bytenr);
        if (!b) return -EIO;
        if (btrfs_level(b) != level) { kprintf("btrfs: level mismatch at %lx (want %d got %d)\n", bytenr, level, btrfs_level(b)); return -EIO; }
        int exact; int slot = bin_search(b, key, &exact);
        p->blocks[level] = b;
        if (level == 0) { p->slots[0] = slot; return exact ? 0 : 1; }
        if (!exact && slot > 0) slot--;
        if (btrfs_nritems(b) == 0) return -EIO;
        if (slot >= btrfs_nritems(b)) slot = btrfs_nritems(b) - 1;
        p->slots[level] = slot;
        bytenr = btrfs_node_ptr(b, slot)->blockptr;
        level--;
    }
}

/* advance to the next item; returns 1 at end of tree */
int btrfs_next_item(struct btrfs_fs *fs, struct btrfs_path *p) {
    p->slots[0]++;
    if (p->slots[0] < btrfs_nritems(p->blocks[0])) return 0;
    /* climb */
    int level = 1;
    while (level <= p->top) {
        if (p->slots[level] + 1 < btrfs_nritems(p->blocks[level])) break;
        level++;
    }
    if (level > p->top) return 1;
    p->slots[level]++;
    for (; level > 0; level--) {
        uint64_t bytenr = btrfs_node_ptr(p->blocks[level], p->slots[level])->blockptr;
        struct tree_block *b = btrfs_read_block(fs, bytenr);
        if (!b) return -EIO;
        p->blocks[level - 1] = b; p->slots[level - 1] = 0;
    }
    return 0;
}
int btrfs_prev_item(struct btrfs_fs *fs, struct btrfs_path *p) {
    if (p->slots[0] > 0) { p->slots[0]--; return 0; }
    int level = 1;
    while (level <= p->top) { if (p->slots[level] > 0) break; level++; }
    if (level > p->top) return 1;
    p->slots[level]--;
    for (; level > 0; level--) {
        uint64_t bytenr = btrfs_node_ptr(p->blocks[level], p->slots[level])->blockptr;
        struct tree_block *b = btrfs_read_block(fs, bytenr);
        if (!b) return -EIO;
        p->blocks[level - 1] = b; p->slots[level - 1] = btrfs_nritems(b) - 1;
    }
    return 0;
}
/* if the search landed past the end of a leaf, move to the next real item */
int btrfs_path_normalize(struct btrfs_fs *fs, struct btrfs_path *p) {
    if (p->slots[0] < btrfs_nritems(p->blocks[0])) return 0;
    p->slots[0]--;
    return btrfs_next_item(fs, p);
}
struct btrfs_key *btrfs_path_key(struct btrfs_path *p) { return &btrfs_leaf_item(p->blocks[0], p->slots[0])->key; }
void *btrfs_path_data(struct btrfs_path *p) { return btrfs_item_data(p->blocks[0], p->slots[0]); }
uint32_t btrfs_path_size(struct btrfs_path *p) { return btrfs_leaf_item(p->blocks[0], p->slots[0])->size; }

/* ------------------------------------------------------------ roots */
static void persist_root(struct btrfs_fs *fs, struct btrfs_tree *tree) {
    if (tree->objectid == BTRFS_ROOT_TREE_OBJECTID) { fs->sb.root = tree->bytenr; fs->sb.root_level = tree->level; return; }
    if (tree->objectid == BTRFS_CHUNK_TREE_OBJECTID) { fs->sb.chunk_root = tree->bytenr; fs->sb.chunk_root_level = tree->level; fs->sb.chunk_root_generation = fs->transid; return; }
    struct btrfs_path p; struct btrfs_key k = { tree->objectid, BTRFS_ROOT_ITEM_KEY, 0 };
    if (btrfs_search(fs, &fs->root_tree, &k, &p) != 0) {
        /* root items may have a non-zero offset (generation): find by objectid+type */
        k.offset = (uint64_t)-1;
        if (btrfs_search(fs, &fs->root_tree, &k, &p) < 0 || btrfs_prev_item(fs, &p)) return;
        struct btrfs_key *fk = btrfs_path_key(&p);
        if (fk->objectid != tree->objectid || fk->type != BTRFS_ROOT_ITEM_KEY) return;
    }
    struct btrfs_root_item *ri = btrfs_path_data(&p); uint32_t isz = btrfs_path_size(&p);
    if (isz < offsetof(struct btrfs_root_item, level) + 1) return;
    ri->bytenr = tree->bytenr; ri->level = tree->level; ri->generation = tree->generation;
    if (isz >= offsetof(struct btrfs_root_item, generation_v2) + 8) ri->generation_v2 = tree->generation;   /* old, short root items lack it */
    btrfs_mark_dirty(fs, p.blocks[0]);
}

int btrfs_load_tree(struct btrfs_fs *fs, uint64_t objectid, struct btrfs_tree *out) {
    struct btrfs_path p; struct btrfs_key k = { objectid, BTRFS_ROOT_ITEM_KEY, (uint64_t)-1 };
    int r = btrfs_search(fs, &fs->root_tree, &k, &p);
    if (r < 0) return r;
    if (r == 1) { if (btrfs_prev_item(fs, &p)) return -ENOENT; }
    struct btrfs_key *fk = btrfs_path_key(&p);
    if (fk->objectid != objectid || fk->type != BTRFS_ROOT_ITEM_KEY) return -ENOENT;
    if (btrfs_path_size(&p) < offsetof(struct btrfs_root_item, level) + 1) return -EIO;
    struct btrfs_root_item *ri = btrfs_path_data(&p);
    out->objectid = objectid; out->bytenr = ri->bytenr; out->level = ri->level; out->generation = ri->generation;
    return 0;
}

/* ------------------------------------------------------------ node / leaf editing */
/* simpler, correct version: walk up while slot == 0 */
static void fixup_keys(struct btrfs_fs *fs, struct btrfs_path *p) {
    for (int l = 1; l <= p->top; l++) {
        struct tree_block *child = p->blocks[l - 1];
        const struct btrfs_key *first = (l - 1 == 0) ? &btrfs_leaf_item(child, 0)->key : &btrfs_node_ptr(child, 0)->key;
        struct btrfs_key_ptr *ptr = btrfs_node_ptr(p->blocks[l], p->slots[l]);
        if (btrfs_cmp_key(&ptr->key, first) == 0) break;
        ptr->key = *first; btrfs_mark_dirty(fs, p->blocks[l]);
        if (p->slots[l] != 0) break;
    }
}

static void leaf_insert_raw(struct btrfs_fs *fs, struct tree_block *leaf, int slot, const struct btrfs_key *key, const void *data, uint32_t size) {
    int n = btrfs_nritems(leaf);
    uint32_t data_end = n ? btrfs_leaf_item(leaf, n - 1)->offset : leaf_data_size(fs);
    uint32_t slot_end = slot ? btrfs_leaf_item(leaf, slot - 1)->offset : leaf_data_size(fs);
    uint8_t *base = leaf->data + sizeof(struct btrfs_header);
    /* shift item data of slots >= slot down by size */
    if (slot < n) {
        memmove(base + data_end - size, base + data_end, slot_end - data_end);
        for (int i = slot; i < n; i++) btrfs_leaf_item(leaf, i)->offset -= size;
        memmove(btrfs_leaf_item(leaf, slot + 1), btrfs_leaf_item(leaf, slot), (n - slot) * sizeof(struct btrfs_item));
    }
    struct btrfs_item *it = btrfs_leaf_item(leaf, slot);
    it->key = *key; it->offset = slot_end - size; it->size = size;
    if (data) memcpy(base + it->offset, data, size); else memset(base + it->offset, 0, size);
    hdr(leaf)->nritems = n + 1;
    btrfs_mark_dirty(fs, leaf);
}
static void leaf_delete_raw(struct btrfs_fs *fs, struct tree_block *leaf, int slot) {
    int n = btrfs_nritems(leaf);
    struct btrfs_item *it = btrfs_leaf_item(leaf, slot);
    uint32_t size = it->size, off = it->offset;
    uint32_t data_end = btrfs_leaf_item(leaf, n - 1)->offset;
    uint8_t *base = leaf->data + sizeof(struct btrfs_header);
    if (slot < n - 1) {
        memmove(base + data_end + size, base + data_end, off - data_end);
        for (int i = slot + 1; i < n; i++) btrfs_leaf_item(leaf, i)->offset += size;
        memmove(btrfs_leaf_item(leaf, slot), btrfs_leaf_item(leaf, slot + 1), (n - slot - 1) * sizeof(struct btrfs_item));
    }
    hdr(leaf)->nritems = n - 1;
    btrfs_mark_dirty(fs, leaf);
}
static void node_insert_raw(struct btrfs_fs *fs, struct tree_block *node, int slot, const struct btrfs_key *key, uint64_t blockptr, uint64_t gen) {
    int n = btrfs_nritems(node);
    if (slot < n) memmove(btrfs_node_ptr(node, slot + 1), btrfs_node_ptr(node, slot), (n - slot) * sizeof(struct btrfs_key_ptr));
    struct btrfs_key_ptr *kp = btrfs_node_ptr(node, slot);
    kp->key = *key; kp->blockptr = blockptr; kp->generation = gen;
    hdr(node)->nritems = n + 1;
    btrfs_mark_dirty(fs, node);
}
static void node_delete_raw(struct btrfs_fs *fs, struct tree_block *node, int slot) {
    int n = btrfs_nritems(node);
    if (slot < n - 1) memmove(btrfs_node_ptr(node, slot), btrfs_node_ptr(node, slot + 1), (n - slot - 1) * sizeof(struct btrfs_key_ptr));
    hdr(node)->nritems = n - 1;
    btrfs_mark_dirty(fs, node);
}

/* insert a pointer into the parent at 'level' (creating a new root if needed). Path slots above are updated. */
static int insert_ptr(struct btrfs_fs *fs, struct btrfs_path *p, int level, const struct btrfs_key *key, uint64_t blockptr) {
    struct btrfs_tree *tree = p->tree;
    if (level >= BTRFS_MAX_LEVEL) return -ENOSPC;
    if (level > p->top) {
        /* grow the tree: new root with two pointers */
        uint64_t nb = btrfs_alloc_tree_block(fs, tree->objectid);
        if (!nb) return -ENOSPC;
        struct tree_block *root = btrfs_new_block(fs, nb, tree->objectid, level);
        btrfs_set_metadata_level(fs, nb, level);
        struct tree_block *old = p->blocks[level - 1];
        const struct btrfs_key *first = (level - 1 == 0) ? &btrfs_leaf_item(old, 0)->key : &btrfs_node_ptr(old, 0)->key;
        node_insert_raw(fs, root, 0, first, old->logical, hdr(old)->generation);
        node_insert_raw(fs, root, 1, key, blockptr, fs->transid);
        p->blocks[level] = root; p->slots[level] = 0; p->top = level;
        tree->bytenr = nb; tree->level = level; tree->generation = fs->transid;
        persist_root(fs, tree);
        return 0;
    }
    struct tree_block *node = p->blocks[level];
    if (btrfs_nritems(node) >= node_capacity(fs)) {
        /* split the node */
        uint64_t nb = btrfs_alloc_tree_block(fs, tree->objectid);
        if (!nb) return -ENOSPC;
        struct tree_block *right = btrfs_new_block(fs, nb, tree->objectid, level);
        btrfs_set_metadata_level(fs, nb, level);
        int n = btrfs_nritems(node), mid = n / 2;
        memcpy(btrfs_node_ptr(right, 0), btrfs_node_ptr(node, mid), (n - mid) * sizeof(struct btrfs_key_ptr));
        hdr(right)->nritems = n - mid; hdr(node)->nritems = mid;
        btrfs_mark_dirty(fs, node);
        int r = insert_ptr(fs, p, level + 1, &btrfs_node_ptr(right, 0)->key, nb);
        if (r < 0) return r;
        if (p->slots[level] >= mid) { p->slots[level] -= mid; p->blocks[level] = right; p->slots[level + 1]++; node = right; }
    }
    int slot = p->slots[level] + 1;
    node_insert_raw(fs, node, slot, key, blockptr, fs->transid);
    return 0;
}

static int split_leaf(struct btrfs_fs *fs, struct btrfs_path *p, const struct btrfs_key *ins_key, uint32_t need) {
    struct tree_block *leaf = p->blocks[0];
    struct btrfs_tree *tree = p->tree;
    int n = btrfs_nritems(leaf);
    if (n == 0) return -EIO;
    uint64_t nb = btrfs_alloc_tree_block(fs, tree->objectid);
    if (!nb) return -ENOSPC;
    struct tree_block *right = btrfs_new_block(fs, nb, tree->objectid, 0);
    bool ins_before_all = btrfs_cmp_key(ins_key, &btrfs_leaf_item(leaf, 0)->key) < 0;
    bool ins_after_all = btrfs_cmp_key(ins_key, &btrfs_leaf_item(leaf, n - 1)->key) > 0;
    int mid;
    if (n == 1) mid = ins_before_all ? 0 : 1;
    else {
        uint32_t total = leaf_data_size(fs) - btrfs_leaf_free_space(fs, leaf), acc = 0;
        for (mid = 0; mid < n; mid++) { acc += btrfs_leaf_item(leaf, mid)->size + sizeof(struct btrfs_item); if (acc * 2 >= total) break; }
        mid = CLAMP(mid, 1, n - 1);
        if (ins_after_all) mid = n;          /* append: new leaf just for the insert keeps the left full */
        else if (ins_before_all) mid = 0;    /* prepend: move everything right, insert left */
    }
    for (int i = mid; i < n; i++) {
        struct btrfs_item *it = btrfs_leaf_item(leaf, i);
        leaf_insert_raw(fs, right, i - mid, &it->key, leaf->data + sizeof(struct btrfs_header) + it->offset, it->size);
    }
    for (int i = n - 1; i >= mid; i--) leaf_delete_raw(fs, leaf, i);
    bool to_right = (mid == 0) ? false : (btrfs_nritems(right) == 0 || btrfs_cmp_key(ins_key, &btrfs_leaf_item(right, 0)->key) >= 0);
    if (mid == 0) to_right = false;
    struct btrfs_key right_key = btrfs_nritems(right) ? btrfs_leaf_item(right, 0)->key : *ins_key;
    int r = insert_ptr(fs, p, 1, &right_key, nb);
    if (r < 0) return r;
    if (to_right) {
        p->blocks[0] = right; p->slots[1]++;
        int exact; p->slots[0] = bin_search(right, ins_key, &exact);
        if (btrfs_nritems(right) == 0) { btrfs_node_ptr(p->blocks[1], p->slots[1])->key = *ins_key; btrfs_mark_dirty(fs, p->blocks[1]); }
    } else {
        int exact; p->slots[0] = bin_search(leaf, ins_key, &exact);
        if (btrfs_nritems(leaf) == 0) { btrfs_node_ptr(p->blocks[1], p->slots[1])->key = *ins_key; btrfs_mark_dirty(fs, p->blocks[1]); }
    }
    if (btrfs_leaf_free_space(fs, p->blocks[0]) < need) return -ENOSPC;
    return 0;
}

int btrfs_insert_item(struct btrfs_fs *fs, struct btrfs_tree *tree, const struct btrfs_key *key, const void *data, uint32_t size, struct btrfs_path *out) {
    struct btrfs_path p;
    int r = btrfs_search(fs, tree, key, &p);
    if (r < 0) return r;
    if (r == 0) return -EEXIST;
    uint32_t need = size + sizeof(struct btrfs_item);
    if (btrfs_leaf_free_space(fs, p.blocks[0]) < need) {
        r = split_leaf(fs, &p, key, need);
        if (r < 0) return r;
    }
    leaf_insert_raw(fs, p.blocks[0], p.slots[0], key, data, size);
    if (p.slots[0] == 0) fixup_keys(fs, &p);
    if (out) *out = p;
    btrfs_flush_pending(fs);
    if (out && out->blocks[0] && (fs->npending == 0)) { /* path may be stale after a flush that split this leaf: re-search */
        struct btrfs_path q; if (btrfs_search(fs, tree, key, &q) == 0) *out = q;
    }
    return 0;
}

static int delete_item_inner(struct btrfs_fs *fs, struct btrfs_path *p);
int btrfs_delete_item(struct btrfs_fs *fs, struct btrfs_path *p) {
    int r = delete_item_inner(fs, p);
    btrfs_flush_pending(fs);
    return r;
}
static int delete_item_inner(struct btrfs_fs *fs, struct btrfs_path *p) {
    struct tree_block *leaf = p->blocks[0];
    leaf_delete_raw(fs, leaf, p->slots[0]);
    if (btrfs_nritems(leaf) == 0) {
        if (p->top == 0) return 0;   /* empty root leaf is fine */
        /* remove from parent, free the block */
        int level = 1;
        for (;;) {
            struct tree_block *node = p->blocks[level];
            node_delete_raw(fs, node, p->slots[level]);
            btrfs_free_tree_block(fs, p->blocks[level - 1]->logical, p->tree->objectid, level - 1);
            if (btrfs_nritems(node) > 0 || level == p->top) {
                if (btrfs_nritems(node) > 0 && p->slots[level] == 0) {
                    /* first key changed */
                    struct btrfs_path q = *p; q.top = p->top;
                    for (int l = level; l < p->top; l++) { if (q.slots[l] != 0) break; btrfs_node_ptr(q.blocks[l + 1], q.slots[l + 1])->key = btrfs_node_ptr(q.blocks[l], 0)->key; btrfs_mark_dirty(fs, q.blocks[l + 1]); }
                }
                /* if the root node has a single child, collapse it */
                if (level == p->top && btrfs_nritems(node) == 1 && p->top > 0) {
                    struct btrfs_key_ptr *kp = btrfs_node_ptr(node, 0);
                    struct btrfs_tree *tree = p->tree;
                    uint64_t old_root = node->logical;
                    tree->bytenr = kp->blockptr; tree->level = level - 1; tree->generation = kp->generation;
                    persist_root(fs, tree);
                    btrfs_free_tree_block(fs, old_root, tree->objectid, level);
                }
                break;
            }
            level++;
        }
        return 0;
    }
    if (p->slots[0] == 0) fixup_keys(fs, p);
    return 0;
}

/* resize an item in place (grow requires leaf space; may split) */
int btrfs_resize_item(struct btrfs_fs *fs, struct btrfs_path *p, uint32_t new_size) {
    struct tree_block *leaf = p->blocks[0];
    struct btrfs_item *it = btrfs_leaf_item(leaf, p->slots[0]);
    if (new_size == it->size) return 0;
    if (new_size > it->size && btrfs_leaf_free_space(fs, leaf) < new_size - it->size) {
        /* re-insert: copy out, delete, insert */
        uint32_t old = it->size; struct btrfs_key key = it->key;
        uint8_t *tmp = kmalloc(new_size); memcpy(tmp, btrfs_item_data(leaf, p->slots[0]), old); memset(tmp + old, 0, new_size - old);
        btrfs_delete_item(fs, p);
        int r = btrfs_insert_item(fs, p->tree, &key, tmp, new_size, p);
        if (r < 0) btrfs_insert_item(fs, p->tree, &key, tmp, old, p);   /* best effort: put the old item back */
        kfree(tmp);
        return r;
    }
    /* the head of the item is kept (callers compact toward the front / append at the end). Its end is
     * fixed by the previous slot, so its kept bytes and all data of later slots move by -delta. */
    int n = btrfs_nritems(leaf), slot = p->slots[0];
    uint8_t *base = leaf->data + sizeof(struct btrfs_header);
    uint32_t data_end = btrfs_leaf_item(leaf, n - 1)->offset, old = it->size;
    int32_t delta = (int32_t)new_size - (int32_t)old;
    memmove(base + data_end - delta, base + data_end, it->offset + MIN(old, new_size) - data_end);
    for (int i = slot; i < n; i++) btrfs_leaf_item(leaf, i)->offset -= delta;
    it->size = new_size;
    if (delta > 0) memset(base + it->offset + old, 0, delta);
    btrfs_mark_dirty(fs, leaf);
    return 0;
}

/* ------------------------------------------------------------ block groups / allocation */
static int bg_tree_select(struct btrfs_fs *fs, struct btrfs_tree **tree) {
    *tree = (fs->sb.incompat_flags & BTRFS_FEATURE_INCOMPAT_BLOCK_GROUP_TREE) ? &fs->bg_tree : &fs->extent_tree;
    return 0;
}
static int adjust_block_group(struct btrfs_fs *fs, uint64_t bytenr, int64_t delta) {
    struct btrfs_tree *t; bg_tree_select(fs, &t);
    struct chunk_map *m = find_chunk(fs, bytenr);
    if (!m) return -EIO;
    struct btrfs_key k = { m->logical, BTRFS_BLOCK_GROUP_ITEM_KEY, m->length };
    struct btrfs_path p;
    if (btrfs_search(fs, t, &k, &p) != 0 || btrfs_path_size(&p) < sizeof(struct btrfs_block_group_item)) { kprintf("btrfs: block group %lx not found\n", m->logical); return -EIO; }
    struct btrfs_block_group_item *bgi = btrfs_path_data(&p);
    bgi->used += delta;
    btrfs_mark_dirty(fs, p.blocks[0]);
    fs->sb.bytes_used += delta;
    return 0;
}

/* ------------------------------------------------------------ free space tree */
#define BTRFS_FREE_SPACE_INFO_KEY 198
#define BTRFS_FREE_SPACE_EXTENT_KEY 199
#define BTRFS_FREE_SPACE_BITMAP_KEY 200
struct btrfs_free_space_info { uint32_t extent_count, flags; } PACKED;

static void fst_invalidate(struct btrfs_fs *fs, const char *why) {
    if (!fs->fst_ok) return;
    fs->fst_ok = false;
    kprintf("btrfs: free space tree no longer maintained (%s); Linux will rebuild it\n", why);
}
static int fst_adjust_count(struct btrfs_fs *fs, uint64_t start, int delta) {
    struct chunk_map *m = find_chunk(fs, start);
    if (!m) return -EIO;
    struct btrfs_key k = { m->logical, BTRFS_FREE_SPACE_INFO_KEY, m->length };
    struct btrfs_path p;
    if (btrfs_search(fs, &fs->fst, &k, &p) != 0 || btrfs_path_size(&p) < sizeof(struct btrfs_free_space_info)) { fst_invalidate(fs, "missing free space info"); return -EIO; }
    struct btrfs_free_space_info *info = btrfs_path_data(&p);
    if (info->flags & 1) { fst_invalidate(fs, "bitmap format"); return -EIO; }
    info->extent_count += delta;
    btrfs_mark_dirty(fs, p.blocks[0]);
    return 0;
}
/* mark [start, start+len) as allocated */
static void fst_alloc(struct btrfs_fs *fs, uint64_t start, uint64_t len) {
    if (!fs->fst_ok) return;
    struct btrfs_key k = { start, BTRFS_FREE_SPACE_EXTENT_KEY, (uint64_t)-1 };
    struct btrfs_path p;
    int r = btrfs_search(fs, &fs->fst, &k, &p);
    if (r < 0) { fst_invalidate(fs, "search failed"); return; }
    if (btrfs_prev_item(fs, &p)) { fst_invalidate(fs, "no covering extent"); return; }
    struct btrfs_key fk = *btrfs_path_key(&p);
    if (fk.type == BTRFS_FREE_SPACE_BITMAP_KEY) { fst_invalidate(fs, "bitmap format"); return; }
    if (fk.type != BTRFS_FREE_SPACE_EXTENT_KEY || fk.objectid > start || fk.objectid + fk.offset < start + len) { fst_invalidate(fs, "allocation not inside a free extent"); return; }
    uint64_t estart = fk.objectid, elen = fk.offset;
    btrfs_delete_item(fs, &p);
    int pieces = 0;
    if (start > estart) { struct btrfs_key lk = { estart, BTRFS_FREE_SPACE_EXTENT_KEY, start - estart }; btrfs_insert_item(fs, &fs->fst, &lk, NULL, 0, NULL); pieces++; }
    if (estart + elen > start + len) { struct btrfs_key rk = { start + len, BTRFS_FREE_SPACE_EXTENT_KEY, estart + elen - start - len }; btrfs_insert_item(fs, &fs->fst, &rk, NULL, 0, NULL); pieces++; }
    fst_adjust_count(fs, start, pieces - 1);
}
/* mark [start, start+len) as free, merging with neighbours */
static void fst_free(struct btrfs_fs *fs, uint64_t start, uint64_t len) {
    if (!fs->fst_ok) return;
    struct chunk_map *m = find_chunk(fs, start);
    if (!m) { fst_invalidate(fs, "free outside chunks"); return; }
    int delta = 1;
    struct btrfs_path p;
    /* left neighbour */
    struct btrfs_key k = { start, BTRFS_FREE_SPACE_EXTENT_KEY, 0 };
    if (btrfs_search(fs, &fs->fst, &k, &p) < 0) { fst_invalidate(fs, "search failed"); return; }
    if (btrfs_prev_item(fs, &p) == 0) {
        struct btrfs_key fk = *btrfs_path_key(&p);
        if (fk.type == BTRFS_FREE_SPACE_EXTENT_KEY && fk.objectid + fk.offset == start && fk.objectid >= m->logical) {
            btrfs_delete_item(fs, &p); start = fk.objectid; len += fk.offset; delta--;
        }
    }
    /* right neighbour */
    struct btrfs_key rk = { start + len, BTRFS_FREE_SPACE_EXTENT_KEY, 0 };
    int r = btrfs_search(fs, &fs->fst, &rk, &p);
    if (r >= 0 && !btrfs_path_normalize(fs, &p)) {
        struct btrfs_key fk = *btrfs_path_key(&p);
        if (fk.type == BTRFS_FREE_SPACE_EXTENT_KEY && fk.objectid == start + len && fk.objectid < m->logical + m->length) {
            btrfs_delete_item(fs, &p); len += fk.offset; delta--;
        }
    }
    struct btrfs_key nk = { start, BTRFS_FREE_SPACE_EXTENT_KEY, len };
    if (btrfs_insert_item(fs, &fs->fst, &nk, NULL, 0, NULL) < 0) { fst_invalidate(fs, "insert failed"); return; }
    fst_adjust_count(fs, start, delta);
}

static bool overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) { return a < b + blen && b < a + alen; }
/* push cursor past superblock mirrors and pending (not yet recorded) allocations */
static uint64_t skip_reserved(struct btrfs_fs *fs, uint64_t cursor, uint64_t len) {
    static const uint64_t supers[3] = { 0x10000, 0x4000000, 0x4000000000ULL };
    bool moved = true;
    while (moved) {
        moved = false;
        int ns = btrfs_chunk_stripes(fs, cursor);
        for (int st = 0; st < ns; st++) {
            uint64_t phys = btrfs_logical_to_phys(fs, cursor, st);
            for (int i = 0; i < 3; i++) if (overlaps(phys, len, supers[i], 4096)) { cursor += supers[i] + 4096 - phys; cursor = ALIGN_UP(cursor, fs->sectorsize); moved = true; }
        }
        for (int i = 0; i < fs->npending; i++) if (!fs->pending[i].free && overlaps(cursor, len, fs->pending[i].bytenr, fs->nodesize)) { cursor = fs->pending[i].bytenr + fs->nodesize; moved = true; }
    }
    return cursor;
}

/* find 'len' bytes of free space inside chunks of the given type by scanning the extent tree */
uint64_t btrfs_alloc_extent(struct btrfs_fs *fs, uint64_t len, uint64_t type) {
    for (int ci = 0; ci < fs->nchunks; ci++) {
        struct chunk_map *m = &fs->chunks[ci];
        if (!(m->type & type) || ((m->type & BTRFS_BLOCK_GROUP_SYSTEM) && !(type & BTRFS_BLOCK_GROUP_SYSTEM))) continue;
        if (m->type & (BTRFS_BLOCK_GROUP_RAID0 | BTRFS_BLOCK_GROUP_RAID10 | BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6)) continue;
        uint64_t chunk_end = m->logical + m->length;
        uint64_t hint = fs->alloc_hint[(type & BTRFS_BLOCK_GROUP_DATA) ? 0 : 1];
        for (int pass = 0; pass < 2; pass++) {
            uint64_t cursor = (pass == 0 && hint > m->logical && hint < chunk_end) ? hint : m->logical;
            if (pass == 1 && cursor == hint) break;
            struct btrfs_key k = { cursor, 0, 0 };
            struct btrfs_path p;
            if (btrfs_search(fs, &fs->extent_tree, &k, &p) < 0) return 0;
            if (btrfs_prev_item(fs, &p) == 0) {
                struct btrfs_key *pk = btrfs_path_key(&p);
                if (pk->type == BTRFS_EXTENT_ITEM_KEY || pk->type == BTRFS_METADATA_ITEM_KEY) {
                    uint64_t elen = pk->type == BTRFS_METADATA_ITEM_KEY ? fs->nodesize : pk->offset;
                    if (pk->objectid + elen > cursor) cursor = pk->objectid + elen;
                }
                btrfs_next_item(fs, &p);
            }
            for (int guard = 0; guard < 1000000; guard++) {
                bool at_end = btrfs_path_normalize(fs, &p) != 0;
                uint64_t estart = chunk_end, elen = 0;
                if (!at_end) {
                    struct btrfs_key *ek = btrfs_path_key(&p);
                    if (ek->type == BTRFS_EXTENT_ITEM_KEY || ek->type == BTRFS_METADATA_ITEM_KEY) { estart = ek->objectid; elen = ek->type == BTRFS_METADATA_ITEM_KEY ? fs->nodesize : ek->offset; }
                    else if (ek->objectid < chunk_end) { if (btrfs_next_item(fs, &p)) at_end = true; else continue; }
                }
                if (estart > chunk_end) estart = chunk_end;
                /* gap is [cursor, estart) */
                for (;;) {
                    uint64_t c2 = skip_reserved(fs, cursor, len);
                    if (c2 + len <= estart) return c2;
                    if (c2 >= estart) { cursor = c2; break; }
                    cursor = c2 + fs->sectorsize;   /* reserved hole in the middle: keep scanning */
                    if (cursor + len > estart) break;
                }
                if (estart >= chunk_end || at_end) break;
                if (estart + elen > cursor) cursor = estart + elen;
                if (btrfs_next_item(fs, &p)) { /* end of tree: tail gap */
                    uint64_t c2 = skip_reserved(fs, cursor, len);
                    if (c2 + len <= chunk_end) return c2;
                    break;
                }
            }
        }
    }
    return 0;
}

static void pending_add(struct btrfs_fs *fs, uint64_t bytenr, int level, uint64_t owner, bool free) {
    if (fs->npending >= MAX_PENDING) { kprintf("btrfs: pending list overflow\n"); return; }
    fs->pending[fs->npending++] = (struct pending_block){ bytenr, level, owner, free };
}

/* ---- chunk allocation: grow the filesystem with a new SINGLE chunk ---- */
int btrfs_alloc_chunk(struct btrfs_fs *fs, uint64_t type, uint64_t want) {
    if (fs->readonly || fs->nchunks >= MAX_CHUNKS || fs->in_chunk_alloc) return -ENOSPC;
    uint64_t dev_size = fs->sb.dev_item.total_bytes;
    /* physical: after the last chunk on the device; logical: after the highest logical address */
    uint64_t phys = 1ull << 20, logical = 1ull << 20;
    for (int i = 0; i < fs->nchunks; i++) {
        for (int st = 0; st < fs->chunks[i].nstripes; st++) if (fs->chunks[i].stripe_phys[st] + fs->chunks[i].length > phys) phys = fs->chunks[i].stripe_phys[st] + fs->chunks[i].length;
        if (fs->chunks[i].logical + fs->chunks[i].length > logical) logical = fs->chunks[i].logical + fs->chunks[i].length;
    }
    phys = ALIGN_UP(phys, 1 << 20); logical = ALIGN_UP(logical, 1 << 20);
    if (phys + (1ull << 20) > dev_size) return -ENOSPC;
    uint64_t len = MIN(want, ALIGN_DOWN(dev_size - phys, 1 << 20));
    if (len < (8ull << 20) && len < want) { if (len < 1ull << 20) return -ENOSPC; }
    fs->in_chunk_alloc = true;
    struct { struct btrfs_chunk c; struct btrfs_stripe s; } PACKED ch;
    memset(&ch, 0, sizeof ch);
    ch.c.length = len; ch.c.owner = BTRFS_EXTENT_TREE_OBJECTID; ch.c.stripe_len = 65536; ch.c.type = type;
    ch.c.io_align = 65536; ch.c.io_width = 65536; ch.c.sector_size = fs->sectorsize; ch.c.num_stripes = 1; ch.c.sub_stripes = 1;
    ch.s.devid = fs->sb.dev_item.devid; ch.s.offset = phys; memcpy(ch.s.dev_uuid, fs->sb.dev_item.uuid, 16);
    /* register in the chunk map first so tree blocks for the bookkeeping can land in it if needed */
    add_chunk(fs, logical, &ch.c, sizeof ch);
    struct btrfs_key k = { BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_CHUNK_ITEM_KEY, logical };
    int r = btrfs_insert_item(fs, &fs->chunk_tree, &k, &ch, sizeof ch, NULL);
    if (r == 0) {
        struct btrfs_dev_extent de = { BTRFS_CHUNK_TREE_OBJECTID, BTRFS_FIRST_CHUNK_TREE_OBJECTID, logical, len };
        memcpy(de.chunk_tree_uuid, fs->chunk_tree_uuid, 16);
        struct btrfs_key dk = { fs->sb.dev_item.devid, BTRFS_DEV_EXTENT_KEY, phys };
        r = btrfs_insert_item(fs, &fs->dev_tree, &dk, &de, sizeof de, NULL);
    }
    if (r == 0) {
        struct btrfs_tree *t; bg_tree_select(fs, &t);
        struct btrfs_block_group_item bgi = { 0, BTRFS_FIRST_CHUNK_TREE_OBJECTID, type };
        struct btrfs_key bk = { logical, BTRFS_BLOCK_GROUP_ITEM_KEY, len };
        r = btrfs_insert_item(fs, t, &bk, &bgi, sizeof bgi, NULL);
    }
    if (r == 0 && fs->fst_ok) {
        struct btrfs_free_space_info info = { 1, 0 };
        struct btrfs_key ik = { logical, BTRFS_FREE_SPACE_INFO_KEY, len }, ek = { logical, BTRFS_FREE_SPACE_EXTENT_KEY, len };
        if (btrfs_insert_item(fs, &fs->fst, &ik, &info, sizeof info, NULL) < 0 || btrfs_insert_item(fs, &fs->fst, &ek, NULL, 0, NULL) < 0) fst_invalidate(fs, "chunk insert failed");
    }
    fs->sb.dev_item.bytes_used += len;
    fs->sb.chunk_root_generation = fs->transid;
    fs->in_chunk_alloc = false;
    if (r < 0) { fs->nchunks--; kprintf("btrfs: chunk allocation failed (%d)\n", r); return r; }
    kprintf("btrfs: allocated %s chunk: logical %lx, physical %lx, %lu MiB\n", (type & BTRFS_BLOCK_GROUP_DATA) ? "data" : "metadata", logical, phys, len >> 20);
    return 0;
}

uint64_t btrfs_alloc_tree_block(struct btrfs_fs *fs, uint64_t owner) {
    uint64_t type = owner == BTRFS_CHUNK_TREE_OBJECTID ? BTRFS_BLOCK_GROUP_SYSTEM : BTRFS_BLOCK_GROUP_METADATA;
    /* metadata never goes into data chunks; chunk tree blocks only into SYSTEM chunks */
    uint64_t b = btrfs_alloc_extent(fs, fs->nodesize, type);
    if (!b && type == BTRFS_BLOCK_GROUP_METADATA && btrfs_alloc_chunk(fs, BTRFS_BLOCK_GROUP_METADATA, 256ull << 20) == 0) b = btrfs_alloc_extent(fs, fs->nodesize, type);
    if (!b) { kprintf("btrfs: out of metadata space\n"); return 0; }
    fs->alloc_hint[1] = b + fs->nodesize;
    pending_add(fs, b, 0, owner, false);
    return b;
}
void btrfs_set_metadata_level(struct btrfs_fs *fs, uint64_t bytenr, int level) {
    for (int i = 0; i < fs->npending; i++) if (fs->pending[i].bytenr == bytenr && !fs->pending[i].free) { fs->pending[i].level = level; return; }
}
void btrfs_free_tree_block(struct btrfs_fs *fs, uint64_t bytenr, uint64_t owner, int level) {
    /* cancel a pending allocation of the same block, otherwise queue the free */
    for (int i = 0; i < fs->npending; i++) if (fs->pending[i].bytenr == bytenr && !fs->pending[i].free) { fs->pending[i] = fs->pending[--fs->npending]; btrfs_forget_block(fs, bytenr); return; }
    pending_add(fs, bytenr, level, owner, true);
    btrfs_forget_block(fs, bytenr);
}

static void record_tree_block(struct btrfs_fs *fs, struct pending_block *pb) {
    uint8_t buf[64]; uint32_t size; struct btrfs_key k;
    struct btrfs_extent_item *ei = (struct btrfs_extent_item *)buf;
    ei->refs = 1; ei->generation = fs->transid; ei->flags = BTRFS_EXTENT_FLAG_TREE_BLOCK;
    if (fs->sb.incompat_flags & BTRFS_FEATURE_INCOMPAT_SKINNY_METADATA) {
        k = (struct btrfs_key){ pb->bytenr, BTRFS_METADATA_ITEM_KEY, (uint64_t)pb->level };
        buf[24] = BTRFS_TREE_BLOCK_REF_KEY; memcpy(buf + 25, &pb->owner, 8); size = 33;
    } else {
        k = (struct btrfs_key){ pb->bytenr, BTRFS_EXTENT_ITEM_KEY, fs->nodesize };
        struct btrfs_tree_block_info *tbi = (struct btrfs_tree_block_info *)(buf + 24);
        memset(tbi, 0, sizeof *tbi); tbi->level = pb->level;
        buf[24 + 18] = BTRFS_TREE_BLOCK_REF_KEY; memcpy(buf + 24 + 19, &pb->owner, 8); size = 24 + 18 + 9;
    }
    int r = btrfs_insert_item(fs, &fs->extent_tree, &k, buf, size, NULL);
    if (r < 0) kprintf("btrfs: failed to record tree block %lx (%d)\n", pb->bytenr, r);
    else { adjust_block_group(fs, pb->bytenr, fs->nodesize); fst_alloc(fs, pb->bytenr, fs->nodesize); }
}
static void unrecord_tree_block(struct btrfs_fs *fs, struct pending_block *pb) {
    struct btrfs_path p;
    struct btrfs_key k = { pb->bytenr, BTRFS_METADATA_ITEM_KEY, (uint64_t)pb->level };
    int r = btrfs_search(fs, &fs->extent_tree, &k, &p);
    if (r != 0) { k = (struct btrfs_key){ pb->bytenr, BTRFS_EXTENT_ITEM_KEY, fs->nodesize }; r = btrfs_search(fs, &fs->extent_tree, &k, &p); }
    if (r != 0) {
        /* level unknown: look for any metadata item at this bytenr */
        k = (struct btrfs_key){ pb->bytenr, BTRFS_METADATA_ITEM_KEY, 0 };
        r = btrfs_search(fs, &fs->extent_tree, &k, &p);
        if (r == 1 && !btrfs_path_normalize(fs, &p)) { struct btrfs_key *fk = btrfs_path_key(&p); r = (fk->objectid == pb->bytenr && fk->type == BTRFS_METADATA_ITEM_KEY) ? 0 : 1; }
    }
    if (r != 0 || btrfs_path_size(&p) < sizeof(struct btrfs_extent_item)) { kprintf("btrfs: no extent item for tree block %lx\n", pb->bytenr); return; }
    struct btrfs_extent_item *ei = btrfs_path_data(&p);
    if (ei->refs > 1) { ei->refs--; btrfs_mark_dirty(fs, p.blocks[0]); return; }
    btrfs_delete_item(fs, &p);
    adjust_block_group(fs, pb->bytenr, -(int64_t)fs->nodesize);
    fst_free(fs, pb->bytenr, fs->nodesize);
}
/* apply deferred extent-tree bookkeeping; safe to call only between tree operations */
void btrfs_flush_pending(struct btrfs_fs *fs) {
    if (fs->in_flush) return;
    fs->in_flush = true;
    while (fs->npending) {
        struct pending_block pb = fs->pending[0];
        fs->pending[0] = fs->pending[--fs->npending];
        if (pb.free) unrecord_tree_block(fs, &pb); else record_tree_block(fs, &pb);
    }
    fs->in_flush = false;
}

/* data extents */
static uint64_t hash_data_ref(uint64_t root, uint64_t owner, uint64_t offset) {
    uint32_t high = crc32c(~0u, &root, 8), low = crc32c(~0u, &owner, 8);
    low = crc32c(low, &offset, 8);
    return ((uint64_t)high << 31) ^ (uint64_t)low;
}
int btrfs_record_data_extent(struct btrfs_fs *fs, uint64_t bytenr, uint64_t len, uint64_t root, uint64_t ino, uint64_t file_off) {
    uint8_t buf[24 + 1 + 28];
    struct btrfs_extent_item *ei = (struct btrfs_extent_item *)buf;
    ei->refs = 1; ei->generation = fs->transid; ei->flags = BTRFS_EXTENT_FLAG_DATA;
    buf[24] = BTRFS_EXTENT_DATA_REF_KEY;
    struct btrfs_extent_data_ref ref = { root, ino, file_off, 1 };
    memcpy(buf + 25, &ref, 28);
    struct btrfs_key k = { bytenr, BTRFS_EXTENT_ITEM_KEY, len };
    int r = btrfs_insert_item(fs, &fs->extent_tree, &k, buf, sizeof buf, NULL);
    if (r < 0) return r;
    r = adjust_block_group(fs, bytenr, len);
    fst_alloc(fs, bytenr, len);
    return r;
}
uint64_t btrfs_alloc_data(struct btrfs_fs *fs, uint64_t len) {
    uint64_t b = btrfs_alloc_extent(fs, len, BTRFS_BLOCK_GROUP_DATA);
    if (!b && btrfs_alloc_chunk(fs, BTRFS_BLOCK_GROUP_DATA, 1ull << 30) == 0) b = btrfs_alloc_extent(fs, len, BTRFS_BLOCK_GROUP_DATA);
    if (b) fs->alloc_hint[0] = b + len;
    return b;
}

/* drop one data reference; frees the extent (and its csums) when unreferenced */
int btrfs_free_data_extent(struct btrfs_fs *fs, uint64_t bytenr, uint64_t len, uint64_t root, uint64_t ino, uint64_t file_off) {
    struct btrfs_key k = { bytenr, BTRFS_EXTENT_ITEM_KEY, len };
    struct btrfs_path p;
    if (btrfs_search(fs, &fs->extent_tree, &k, &p) != 0 || btrfs_path_size(&p) < sizeof(struct btrfs_extent_item)) { kprintf("btrfs: data extent %lx+%lx not found\n", bytenr, len); return -EIO; }
    struct btrfs_extent_item *ei = btrfs_path_data(&p);
    uint32_t size = btrfs_path_size(&p);
    if (ei->refs <= 1) {
        btrfs_delete_item(fs, &p);
        adjust_block_group(fs, bytenr, -(int64_t)len);
        btrfs_csum_remove(fs, bytenr, len);
        fst_free(fs, bytenr, len);
        return 0;
    }
    /* find our inline data ref and decrement */
    uint8_t *q = (uint8_t *)ei + 24, *end = (uint8_t *)ei + size;
    while (q < end) {
        uint8_t type = q[0];
        if (type == BTRFS_EXTENT_DATA_REF_KEY) {
            if (q + 29 > end) break;
            struct btrfs_extent_data_ref *ref = (struct btrfs_extent_data_ref *)(q + 1);
            if (ref->root == root && ref->objectid == ino && ref->offset == file_off) {
                ei->refs--;
                if (ref->count > 1) { ref->count--; btrfs_mark_dirty(fs, p.blocks[0]); return 0; }
                uint32_t off = (uint32_t)(q - (uint8_t *)ei);
                memmove(q, q + 29, end - (q + 29));
                btrfs_mark_dirty(fs, p.blocks[0]);
                btrfs_resize_item(fs, &p, size - 29);
                UNUSED(off);
                return 0;
            }
            q += 29;
        } else if (type == BTRFS_SHARED_DATA_REF_KEY) q += 13;
        else if (type == BTRFS_TREE_BLOCK_REF_KEY || type == BTRFS_SHARED_BLOCK_REF_KEY) q += 9;
        else break;
    }
    /* reference lives in a separate item or is unknown: just drop the count */
    ei->refs--; btrfs_mark_dirty(fs, p.blocks[0]);
    return 0;
}

/* ------------------------------------------------------------ checksums */
int btrfs_csum_insert(struct btrfs_fs *fs, uint64_t bytenr, const void *data, uint64_t len) {
    uint32_t ss = fs->sectorsize;
    uint64_t nsec = len / ss;
    /* at most ~1000 csums per item keeps items small */
    uint64_t per = 1024;
    for (uint64_t start = 0; start < nsec; start += per) {
        uint64_t cnt = MIN(per, nsec - start);
        uint32_t *sums = kmalloc(cnt * 4);
        for (uint64_t i = 0; i < cnt; i++) sums[i] = ~crc32c(~0u, (const uint8_t *)data + (start + i) * ss, ss);
        struct btrfs_key k = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, bytenr + start * ss };
        int r = btrfs_insert_item(fs, &fs->csum_tree, &k, sums, cnt * 4, NULL);
        kfree(sums);
        if (r < 0) return r;
    }
    return 0;
}
int btrfs_csum_remove(struct btrfs_fs *fs, uint64_t bytenr, uint64_t len) {
    uint32_t ss = fs->sectorsize;
    uint64_t end = bytenr + len;
    for (int guard = 0; guard < 10000; guard++) {
        struct btrfs_key k = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, bytenr };
        struct btrfs_path p;
        int r = btrfs_search(fs, &fs->csum_tree, &k, &p);
        if (r < 0) return r;
        if (r == 1) {
            /* item before bytenr may cover it */
            if (btrfs_prev_item(fs, &p)) { if (btrfs_path_normalize(fs, &p)) return 0; }
            struct btrfs_key *pk = btrfs_path_key(&p);
            if (pk->objectid != BTRFS_EXTENT_CSUM_OBJECTID || pk->type != BTRFS_EXTENT_CSUM_KEY) { if (btrfs_next_item(fs, &p)) return 0; }
        }
        if (btrfs_path_normalize(fs, &p)) return 0;
        struct btrfs_key *ik = btrfs_path_key(&p);
        if (ik->objectid != BTRFS_EXTENT_CSUM_OBJECTID || ik->type != BTRFS_EXTENT_CSUM_KEY) return 0;
        uint64_t istart = ik->offset, ilen = (uint64_t)btrfs_path_size(&p) / 4 * ss, iend = istart + ilen;
        if (istart >= end) return 0;
        if (iend <= bytenr) { if (btrfs_next_item(fs, &p)) return 0; if (btrfs_path_key(&p)->offset >= end) return 0; bytenr = MAX(bytenr, btrfs_path_key(&p)->offset); continue; }
        /* overlap: compute remaining pieces */
        uint64_t keep_left = bytenr > istart ? bytenr - istart : 0;
        uint64_t keep_right = iend > end ? iend - end : 0;
        uint32_t *sums = kmalloc(btrfs_path_size(&p)); memcpy(sums, btrfs_path_data(&p), btrfs_path_size(&p));
        btrfs_delete_item(fs, &p);
        if (keep_left) { struct btrfs_key lk = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, istart }; btrfs_insert_item(fs, &fs->csum_tree, &lk, sums, keep_left / ss * 4, NULL); }
        if (keep_right) { struct btrfs_key rk = { BTRFS_EXTENT_CSUM_OBJECTID, BTRFS_EXTENT_CSUM_KEY, end }; btrfs_insert_item(fs, &fs->csum_tree, &rk, sums + (end - istart) / ss, keep_right / ss * 4, NULL); }
        kfree(sums);
        if (iend >= end) return 0;
        bytenr = iend;
    }
    return 0;
}

/* ------------------------------------------------------------ commit */
static uint32_t super_csum(const struct btrfs_super_block *sb) { return ~crc32c(~0u, (const uint8_t *)sb + BTRFS_CSUM_SIZE, sizeof *sb - BTRFS_CSUM_SIZE); }

int btrfs_commit(struct btrfs_fs *fs) {
    btrfs_flush_pending(fs);
    if (!fs->dirty_super && !fs->any_dirty) return 0;
    /* root tree root must carry the superblock generation */
    struct tree_block *rt = btrfs_read_block(fs, fs->root_tree.bytenr);
    if (rt) { hdr(rt)->generation = fs->transid; rt->dirty = true; }
    if (fs->sb.chunk_root_generation == fs->transid) { struct tree_block *ct = btrfs_read_block(fs, fs->chunk_tree.bytenr); if (ct) { hdr(ct)->generation = fs->transid; ct->dirty = true; } }
    for (int i = 0; i < CACHE_BUCKETS; i++) for (struct tree_block *b = fs->cache[i]; b; b = b->hnext) {
        if (!b->dirty) continue;
        uint32_t c = block_csum(fs, b->data);
        memset(b->data, 0, BTRFS_CSUM_SIZE); memcpy(b->data, &c, 4);
        if (btrfs_write_logical(fs, b->logical, b->data, fs->nodesize) < 0) kprintf("btrfs: write of tree block %lx failed\n", b->logical);
        b->dirty = false;
    }
    fs->sb.generation = fs->transid;
    fs->sb.root_level = fs->root_tree.level; fs->sb.root = fs->root_tree.bytenr;
    fs->sb.chunk_root_generation = fs->sb.chunk_root_generation;
    fs->sb.cache_generation = (uint64_t)-1;
    if (!fs->fst_ok) fs->sb.compat_ro_flags &= ~(uint64_t)BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID;
    fs->sb.log_root = 0;
    /* refresh root backup slot 0 so the kernel's backup-root logic stays sane */
    struct btrfs_root_backup *bk = &fs->sb.super_roots[fs->transid % 4];
    bk->tree_root = fs->sb.root; bk->tree_root_gen = fs->transid; bk->tree_root_level = fs->sb.root_level;
    bk->chunk_root = fs->sb.chunk_root; bk->chunk_root_gen = fs->sb.chunk_root_generation; bk->chunk_root_level = fs->sb.chunk_root_level;
    bk->extent_root = fs->extent_tree.bytenr; bk->extent_root_gen = fs->extent_tree.generation; bk->extent_root_level = fs->extent_tree.level;
    bk->fs_root = fs->fs_tree.bytenr; bk->fs_root_gen = fs->fs_tree.generation; bk->fs_root_level = fs->fs_tree.level;
    bk->csum_root = fs->csum_tree.bytenr; bk->csum_root_gen = fs->csum_tree.generation; bk->csum_root_level = fs->csum_tree.level;
    bk->total_bytes = fs->sb.total_bytes; bk->bytes_used = fs->sb.bytes_used; bk->num_devices = fs->sb.num_devices;
    uint32_t c = super_csum(&fs->sb);
    memset(fs->sb.csum, 0, BTRFS_CSUM_SIZE); memcpy(fs->sb.csum, &c, 4);
    uint64_t dev_bytes = fs->dev->sectors * fs->dev->sector_size;
    static const uint64_t supers[3] = { 0x10000, 0x4000000, 0x4000000000ULL };
    for (int i = 0; i < 3; i++) {
        if (supers[i] + 4096 > dev_bytes) break;
        fs->sb.bytenr = supers[i];
        c = super_csum(&fs->sb); memset(fs->sb.csum, 0, BTRFS_CSUM_SIZE); memcpy(fs->sb.csum, &c, 4);
        blockdev_write_bytes(fs->dev, supers[i], &fs->sb, sizeof fs->sb);
    }
    fs->sb.bytenr = 0x10000;
    fs->dirty_super = false; fs->any_dirty = false;
    fs->transid = fs->sb.generation + 1;
    return 0;
}

void btrfs_begin(struct btrfs_fs *fs) { fs->any_dirty = true; fs->dirty_super = true; }

/* ------------------------------------------------------------ mount */
static int parse_sys_chunks(struct btrfs_fs *fs) {
    if (fs->sb.sys_chunk_array_size > sizeof fs->sb.sys_chunk_array) return -EIO;
    uint8_t *p = fs->sb.sys_chunk_array, *end = p + fs->sb.sys_chunk_array_size;
    while (p + sizeof(struct btrfs_key) + sizeof(struct btrfs_chunk) <= end) {
        struct btrfs_key *k = (struct btrfs_key *)p;
        struct btrfs_chunk *c = (struct btrfs_chunk *)(p + sizeof(struct btrfs_key));
        if (k->type != BTRFS_CHUNK_ITEM_KEY) break;
        uint32_t csz = sizeof(struct btrfs_chunk) + c->num_stripes * sizeof(struct btrfs_stripe);
        if (!c->num_stripes || csz > (uint32_t)(end - (uint8_t *)c)) return -EIO;
        add_chunk(fs, k->offset, c, csz);
        p += sizeof(struct btrfs_key) + csz;
    }
    return fs->nchunks ? 0 : -EIO;
}
static int load_chunk_tree(struct btrfs_fs *fs) {
    struct btrfs_tree ct = { BTRFS_CHUNK_TREE_OBJECTID, fs->sb.chunk_root, fs->sb.chunk_root_level, fs->sb.chunk_root_generation };
    struct btrfs_key k = { BTRFS_FIRST_CHUNK_TREE_OBJECTID, BTRFS_CHUNK_ITEM_KEY, 0 };
    struct btrfs_path p;
    int r = btrfs_search(fs, &ct, &k, &p);
    if (r < 0) return r;
    fs->nchunks = 0;   /* rebuild from the real tree */
    while (!btrfs_path_normalize(fs, &p)) {
        struct btrfs_key *ik = btrfs_path_key(&p);
        if (ik->objectid != BTRFS_FIRST_CHUNK_TREE_OBJECTID || ik->type != BTRFS_CHUNK_ITEM_KEY) { if (btrfs_next_item(fs, &p)) break; continue; }
        add_chunk(fs, ik->offset, btrfs_path_data(&p), btrfs_path_size(&p));
        if (btrfs_next_item(fs, &p)) break;
    }
    struct tree_block *root = btrfs_read_block(fs, fs->sb.chunk_root);
    if (root) memcpy(fs->chunk_tree_uuid, hdr(root)->chunk_tree_uuid, 16);
    return 0;
}

struct btrfs_fs *btrfs_open(struct blockdev *dev) {
    struct btrfs_fs *fs = kzalloc(sizeof *fs);
    fs->dev = dev;
    if (blockdev_read_bytes(dev, BTRFS_SUPER_OFFSET, &fs->sb, sizeof fs->sb) < 0 || memcmp(fs->sb.magic, BTRFS_MAGIC, 8)) { kfree(fs); return NULL; }
    if (fs->sb.csum_type != 0) { kprintf("btrfs: %s: unsupported checksum type %d\n", dev->name, fs->sb.csum_type); kfree(fs); return NULL; }
    uint32_t want; memcpy(&want, fs->sb.csum, 4);
    if (want != super_csum(&fs->sb)) { kprintf("btrfs: %s: superblock checksum mismatch\n", dev->name); kfree(fs); return NULL; }
    fs->nodesize = fs->sb.nodesize; fs->sectorsize = fs->sb.sectorsize;
    if (fs->nodesize < 4096 || fs->nodesize > 65536 || (fs->nodesize & (fs->nodesize - 1)) || fs->sectorsize < 512 || (fs->sectorsize & (fs->sectorsize - 1)) || fs->sectorsize > fs->nodesize || fs->sb.num_devices != 1) { kprintf("btrfs: %s: unsupported layout (nodesize %u, %lu devices)\n", dev->name, fs->nodesize, fs->sb.num_devices); kfree(fs); return NULL; }
    fs->transid = fs->sb.generation + 1;
    fs->readonly = (fs->sb.incompat_flags & (BTRFS_FEATURE_INCOMPAT_RAID56 | BTRFS_FEATURE_INCOMPAT_ZONED | BTRFS_FEATURE_INCOMPAT_EXTENT_TREE_V2)) != 0;
    if (parse_sys_chunks(fs) < 0) { kprintf("btrfs: %s: no system chunks\n", dev->name); kfree(fs); return NULL; }
    if (load_chunk_tree(fs) < 0) { kprintf("btrfs: %s: cannot read chunk tree\n", dev->name); kfree(fs); return NULL; }
    fs->root_tree = (struct btrfs_tree){ BTRFS_ROOT_TREE_OBJECTID, fs->sb.root, fs->sb.root_level, fs->sb.generation };
    fs->chunk_tree = (struct btrfs_tree){ BTRFS_CHUNK_TREE_OBJECTID, fs->sb.chunk_root, fs->sb.chunk_root_level, fs->sb.chunk_root_generation };
    if (btrfs_load_tree(fs, BTRFS_DEV_TREE_OBJECTID, &fs->dev_tree) < 0) { kprintf("btrfs: no dev tree\n"); fs->readonly = true; }
    if (btrfs_load_tree(fs, BTRFS_EXTENT_TREE_OBJECTID, &fs->extent_tree) < 0) { kprintf("btrfs: no extent tree\n"); kfree(fs); return NULL; }
    if (btrfs_load_tree(fs, BTRFS_CSUM_TREE_OBJECTID, &fs->csum_tree) < 0) { kprintf("btrfs: no csum tree\n"); fs->readonly = true; }
    if (fs->sb.incompat_flags & BTRFS_FEATURE_INCOMPAT_BLOCK_GROUP_TREE) { if (btrfs_load_tree(fs, BTRFS_BLOCK_GROUP_TREE_OBJECTID, &fs->bg_tree) < 0) fs->readonly = true; }
    if ((fs->sb.compat_ro_flags & (BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE | BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID)) == (BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE | BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID))
        fs->fst_ok = btrfs_load_tree(fs, BTRFS_FREE_SPACE_TREE_OBJECTID, &fs->fst) == 0;
    /* default subvolume: root dir item "default" in the root tree dir */
    uint64_t fs_id = BTRFS_FS_TREE_OBJECTID;
    {
        struct btrfs_key k = { BTRFS_ROOT_TREE_DIR_OBJECTID, BTRFS_DIR_ITEM_KEY, crc32c((uint32_t)~1, "default", 7) };
        struct btrfs_path p;
        if (btrfs_search(fs, &fs->root_tree, &k, &p) == 0) { struct btrfs_dir_item *di = btrfs_path_data(&p); if (di->location.objectid >= BTRFS_FIRST_FREE_OBJECTID || di->location.objectid == BTRFS_FS_TREE_OBJECTID) fs_id = di->location.objectid; }
    }
    if (btrfs_load_tree(fs, fs_id, &fs->fs_tree) < 0) { kprintf("btrfs: no fs tree\n"); kfree(fs); return NULL; }
    mutex_init(&fs->lock);
    char label[64]; strlcpy(label, fs->sb.label, sizeof label);
    kprintf("btrfs: %s: '%s' %lu MiB (%lu used), nodesize %u, gen %lu, %d chunks%s\n", dev->name, label, fs->sb.total_bytes >> 20, fs->sb.bytes_used >> 20, fs->nodesize, fs->sb.generation, fs->nchunks, fs->readonly ? ", read-only" : "");
    return fs;
}
