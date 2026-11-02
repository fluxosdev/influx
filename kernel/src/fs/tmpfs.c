#include "fs.h"
#include "mm.h"
#include "string.h"

struct tnode {
    struct vnode vn;
    struct list_head children;
    struct list_head sibling;
    struct tnode *parent;
    uint8_t *data; size_t cap;
};
static uint64_t next_ino = 1;
static const struct vnode_ops tmpfs_ops;

static struct tnode *tn_new(struct tnode *parent, const char *name, int type) {
    struct tnode *t = kzalloc(sizeof *t);
    strlcpy(t->vn.name, name, sizeof t->vn.name);
    t->vn.type = type; t->vn.ops = &tmpfs_ops; t->vn.refcount = 1;   /* held by the tree */
    t->vn.ino = next_ino++; t->vn.mtime = time_unix(); t->vn.mode = type == VN_DIR ? 0755 : 0644;
    t->vn.parent = parent ? &parent->vn : NULL;
    t->vn.fs_name = "tmpfs";
    t->parent = parent;
    list_init(&t->children); list_init(&t->sibling);
    if (parent) list_add_tail(&t->sibling, &parent->children);
    return t;
}

static int64_t t_read(struct vnode *v, uint64_t off, void *buf, size_t n) {
    struct tnode *t = (struct tnode *)v;
    if (off >= v->size) return 0;
    if (off + n > v->size) n = v->size - off;
    memcpy(buf, t->data + off, n);
    return n;
}
static int64_t t_write(struct vnode *v, uint64_t off, const void *buf, size_t n) {
    struct tnode *t = (struct tnode *)v;
    if (off + n > t->cap) {
        size_t nc = t->cap ? t->cap : 256;
        while (nc < off + n) nc *= 2;
        t->data = krealloc(t->data, nc);
        t->cap = nc;
    }
    if (off > v->size) memset(t->data + v->size, 0, off - v->size);
    memcpy(t->data + off, buf, n);
    if (off + n > v->size) v->size = off + n;
    v->mtime = time_unix();
    return n;
}
static int t_truncate(struct vnode *v, uint64_t size) {
    struct tnode *t = (struct tnode *)v;
    if (size > t->cap) { t->data = krealloc(t->data, size); t->cap = size; }
    if (size > v->size) memset(t->data + v->size, 0, size - v->size);
    v->size = size; v->mtime = time_unix();
    return 0;
}
static struct vnode *t_lookup(struct vnode *dir, const char *name) {
    struct tnode *d = (struct tnode *)dir, *c;
    list_for_each_entry(c, &d->children, sibling) if (!strcmp(c->vn.name, name)) { vnode_ref(&c->vn); return &c->vn; }
    return NULL;
}
static int t_readdir(struct vnode *dir, int index, struct dirent *out) {
    struct tnode *d = (struct tnode *)dir, *c; int i = 0;
    list_for_each_entry(c, &d->children, sibling) {
        if (i++ == index) {
            strlcpy(out->name, c->vn.name, sizeof out->name);
            out->type = c->vn.mount_root ? VN_DIR : c->vn.type; out->size = c->vn.size; out->mtime = c->vn.mtime; out->ino = c->vn.ino;
            return 0;
        }
    }
    return -1;
}
static struct vnode *t_create(struct vnode *dir, const char *name, int type) {
    struct tnode *t = tn_new((struct tnode *)dir, name, type);
    dir->mtime = time_unix();
    vnode_ref(&t->vn);
    return &t->vn;
}
static int t_unlink(struct vnode *dir, const char *name) {
    struct tnode *d = (struct tnode *)dir, *c;
    list_for_each_entry(c, &d->children, sibling) {
        if (!strcmp(c->vn.name, name)) {
            if (c->vn.type == VN_DIR && !list_empty(&c->children)) return -ENOTEMPTY;
            list_del(&c->sibling);
            dir->mtime = time_unix();
            vnode_unref(&c->vn);
            return 0;
        }
    }
    return -ENOENT;
}
static void t_release(struct vnode *v) {
    struct tnode *t = (struct tnode *)v;
    if (t->vn.type == VN_DEV || t->vn.type == VN_BLK) return;
    kfree(t->data);
    kfree(t);
}
static const struct vnode_ops tmpfs_ops = { t_read, t_write, t_truncate, t_lookup, t_readdir, t_create, t_unlink, NULL, t_release, NULL };

struct vnode *tmpfs_create_root(const char *fsname) {
    struct tnode *r = tn_new(NULL, "", VN_DIR);
    r->vn.fs_name = fsname;
    return &r->vn;
}

/* devfs: device nodes live as tmpfs children of /dev with their own ops */
struct vnode *devfs_add(const char *name, int type, const struct vnode_ops *ops, void *priv, uint64_t size) {
    struct vnode *dev = vfs_lookup("/dev");
    if (!dev) return NULL;
    struct tnode *t = tn_new((struct tnode *)dev, name, type);
    t->vn.ops = ops; t->vn.priv = priv; t->vn.size = size;
    vnode_unref(dev);
    return &t->vn;
}
