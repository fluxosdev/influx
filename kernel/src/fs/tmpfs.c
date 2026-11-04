#include "fs.h"
#include "mm.h"
#include "string.h"

/* Locking: tree_lock guards every children/sibling list, names and next_ino (lookup/readdir run without
 * vfs_mutex); each node's own mutex guards its data/cap/size. The two are never nested, tmpfs never calls
 * back into the vfs, and device nodes in /dev use their own ops, so no tmpfs lock is held across a
 * blocking device read/write. Order with the vfs: vfs_mutex -> tree_lock. */
struct tnode {
    struct vnode vn;
    struct list_head children;
    struct list_head sibling;
    struct tnode *parent;
    struct mutex lock;
    uint8_t *data; size_t cap;
};
static struct mutex tree_lock = MUTEX_INIT;
static uint64_t next_ino = 1;
#define TMPFS_FILE_MAX (256ull << 20)
/* caller holds t->lock; refuses growth past the per-file cap or half of free memory, and fails
 * (instead of panicking) when no contiguous block of the new size exists */
static int grow(struct tnode *t, uint64_t need) {
    if (need <= t->cap) return 0;
    if (need > TMPFS_FILE_MAX) return -EFBIG;
    uint64_t nc = t->cap ? t->cap : 256;
    while (nc < need) nc *= 2;
    nc = MIN(nc, TMPFS_FILE_MAX);
    if (need - t->cap >= pmm_free_bytes() / 2) return -ENOSPC;
    uint8_t *d = krealloc_try(t->data, nc);
    if (!d && nc > need) d = krealloc_try(t->data, nc = need);   /* the doubled size may not fit contiguously */
    if (!d) return -ENOMEM;
    t->data = d; t->cap = nc;
    return 0;
}
static const struct vnode_ops tmpfs_ops;

/* caller holds tree_lock (when parent is set) */
static struct tnode *tn_new(struct tnode *parent, const char *name, int type) {
    struct tnode *t = kzalloc(sizeof *t);
    strlcpy(t->vn.name, name, sizeof t->vn.name);
    t->vn.type = type; t->vn.ops = &tmpfs_ops; t->vn.refcount = 1;   /* held by the tree */
    t->vn.ino = next_ino++; t->vn.mtime = time_unix(); t->vn.mode = type == VN_DIR ? 0755 : type == VN_LNK ? 0777 : 0644;
    t->vn.parent = parent ? &parent->vn : NULL;
    t->vn.fs_name = "tmpfs";
    t->parent = parent;
    mutex_init(&t->lock);
    list_init(&t->children); list_init(&t->sibling);
    if (parent) list_add_tail(&t->sibling, &parent->children);
    return t;
}

static int64_t t_read(struct vnode *v, uint64_t off, void *buf, size_t n) {
    struct tnode *t = (struct tnode *)v;
    mutex_lock(&t->lock);
    if (off >= v->size) n = 0;
    else if (n > v->size - off) n = v->size - off;
    if (n) memcpy(buf, t->data + off, n);
    mutex_unlock(&t->lock);
    return n;
}
/* caller holds t->lock */
static int64_t write_locked(struct tnode *t, uint64_t off, const void *buf, size_t n) {
    struct vnode *v = &t->vn;
    if (n == 0) return 0;
    if (off > TMPFS_FILE_MAX || n > TMPFS_FILE_MAX - off) return -EFBIG;
    int r = grow(t, off + n); if (r < 0) return r;
    if (off > v->size) memset(t->data + v->size, 0, off - v->size);
    memcpy(t->data + off, buf, n);
    if (off + n > v->size) v->size = off + n;
    v->mtime = time_unix();
    return n;
}
static int64_t t_write(struct vnode *v, uint64_t off, const void *buf, size_t n) {
    struct tnode *t = (struct tnode *)v;
    mutex_lock(&t->lock); int64_t r = write_locked(t, off, buf, n); mutex_unlock(&t->lock);
    return r;
}
static int64_t t_append(struct vnode *v, const void *buf, size_t n, uint64_t *end) {
    struct tnode *t = (struct tnode *)v;
    mutex_lock(&t->lock);
    uint64_t off = v->size;
    int64_t r = write_locked(t, off, buf, n);
    *end = r > 0 ? off + r : off;
    mutex_unlock(&t->lock);
    return r;
}
static int t_truncate(struct vnode *v, uint64_t size) {
    struct tnode *t = (struct tnode *)v;
    mutex_lock(&t->lock);
    int r = grow(t, size);
    if (r == 0) {
        if (size > v->size) memset(t->data + v->size, 0, size - v->size);
        v->size = size; v->mtime = time_unix();
    }
    mutex_unlock(&t->lock);
    return r;
}
static struct vnode *t_lookup(struct vnode *dir, const char *name) {
    struct tnode *d = (struct tnode *)dir, *c; struct vnode *r = NULL;
    mutex_lock(&tree_lock);
    /* the reference is taken under the lock, so a concurrent unlink can't drop the last one first */
    list_for_each_entry(c, &d->children, sibling) if (!strcmp(c->vn.name, name)) { vnode_ref(&c->vn); r = &c->vn; break; }
    mutex_unlock(&tree_lock);
    return r;
}
static int t_readdir(struct vnode *dir, int index, struct dirent *out) {
    struct tnode *d = (struct tnode *)dir, *c; int i = 0, r = -1;
    mutex_lock(&tree_lock);
    list_for_each_entry(c, &d->children, sibling) {
        if (i++ == index) {
            strlcpy(out->name, c->vn.name, sizeof out->name);
            out->type = c->vn.mount_root ? VN_DIR : c->vn.type; out->size = c->vn.size; out->mtime = c->vn.mtime; out->ino = c->vn.ino;
            r = 0; break;
        }
    }
    mutex_unlock(&tree_lock);
    return r;
}
static struct vnode *t_create(struct vnode *dir, const char *name, int type) {
    mutex_lock(&tree_lock);
    struct tnode *t = tn_new((struct tnode *)dir, name, type);
    dir->mtime = time_unix();
    vnode_ref(&t->vn);
    mutex_unlock(&tree_lock);
    return &t->vn;
}
static int t_unlink(struct vnode *dir, const char *name) {
    struct tnode *d = (struct tnode *)dir, *c, *victim = NULL; int r = -ENOENT;
    mutex_lock(&tree_lock);
    list_for_each_entry(c, &d->children, sibling) {
        if (!strcmp(c->vn.name, name)) {
            if (c->vn.type == VN_DIR && !list_empty(&c->children)) { r = -ENOTEMPTY; break; }
            list_del(&c->sibling); list_init(&c->sibling);
            dir->mtime = time_unix();
            victim = c; r = 0; break;
        }
    }
    mutex_unlock(&tree_lock);
    /* drop the tree's reference outside the lock; open files and in-flight lookups keep their own, so the
     * node (and its data) is only freed by whoever releases the last one */
    if (victim) vnode_unref(&victim->vn);
    return r;
}
static void t_release(struct vnode *v) {
    struct tnode *t = (struct tnode *)v;
    if (t->vn.type == VN_DEV || t->vn.type == VN_BLK) return;
    kfree(t->data);
    kfree(t);
}
static const struct vnode_ops tmpfs_ops = { t_read, t_write, t_truncate, t_lookup, t_readdir, t_create, t_unlink, NULL, t_release, NULL, t_append };

struct vnode *tmpfs_create_root(const char *fsname) {
    mutex_lock(&tree_lock); struct tnode *r = tn_new(NULL, "", VN_DIR); mutex_unlock(&tree_lock);
    r->vn.fs_name = fsname;
    return &r->vn;
}

/* devfs: device nodes live as tmpfs children of /dev with their own ops */
struct vnode *devfs_add(const char *name, int type, const struct vnode_ops *ops, void *priv, uint64_t size) {
    struct vnode *dev = vfs_lookup("/dev");
    if (!dev) return NULL;
    mutex_lock(&tree_lock);
    struct tnode *t = tn_new((struct tnode *)dev, name, type);
    t->vn.ops = ops; t->vn.priv = priv; t->vn.size = size;   /* set before any lookup can see the node */
    mutex_unlock(&tree_lock);
    vnode_unref(dev);
    return &t->vn;
}
