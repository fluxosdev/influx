#include "fs.h"
#include "mm.h"
#include "string.h"

static struct vnode *root_vnode;
static struct mutex vfs_mutex = MUTEX_INIT;

struct mount { char path[PATH_MAX]; struct vnode *mountpoint; struct vnode *root; };
static struct mount mounts[VFS_MAX_MOUNTS];
static int nmounts;
static struct mutex mount_lock = MUTEX_INIT;   /* mounts[] / nmounts (USB hotplug vs. iteration) */

void vfs_init(void) {
    root_vnode = tmpfs_create_root("tmpfs");
    vfs_mkdir("/dev");
    kprintf("vfs: tmpfs root mounted\n");
}
struct vnode *vfs_root(void) { return root_vnode; }
void vnode_ref(struct vnode *v) { if (v) __atomic_add_fetch(&v->refcount, 1, __ATOMIC_SEQ_CST); }
void vnode_unref(struct vnode *v) {
    if (!v) return;
    if (__atomic_sub_fetch(&v->refcount, 1, __ATOMIC_SEQ_CST) == 0 && v->ops && v->ops->release) v->ops->release(v);
}

void vfs_normalize(const char *cwd, const char *path, char *out, size_t n) {
    char tmp[PATH_MAX];
    if (path[0] == '/') strlcpy(tmp, path, sizeof tmp);
    else { strlcpy(tmp, cwd ? cwd : "/", sizeof tmp); if (tmp[strlen(tmp) - 1] != '/') strlcat(tmp, "/", sizeof tmp); strlcat(tmp, path, sizeof tmp); }
    /* split into components, resolve . and .. lexically */
    char *comps[256]; int nc = 0;
    char *save, *tok = strtok_r(tmp, "/", &save);
    while (tok) {
        if (!strcmp(tok, ".")) {}
        else if (!strcmp(tok, "..")) { if (nc) nc--; }
        else if (nc < 256) comps[nc++] = tok;
        tok = strtok_r(NULL, "/", &save);
    }
    size_t pos = 0;
    out[pos++] = '/';
    for (int i = 0; i < nc && pos < n - 1; i++) {
        size_t l = strlen(comps[i]);
        if (pos + l + 1 >= n) break;
        if (i) out[pos++] = '/';
        memcpy(out + pos, comps[i], l); pos += l;
    }
    out[pos] = 0;
}

/* Walk from root; result is referenced. */
static struct vnode *walk(const char *norm, struct vnode **parent_out, char *last_comp) {
    struct vnode *cur = root_vnode;
    if (cur->mount_root) cur = cur->mount_root;
    vnode_ref(cur);
    if (parent_out) *parent_out = NULL;
    if (last_comp) last_comp[0] = 0;
    const char *p = norm;
    while (*p == '/') p++;
    while (*p) {
        const char *e = p; while (*e && *e != '/') e++;
        size_t l = e - p;
        char comp[NAME_MAX + 1];
        if (l > NAME_MAX) { vnode_unref(cur); return NULL; }
        memcpy(comp, p, l); comp[l] = 0;
        if (last_comp) strcpy(last_comp, comp);
        if (cur->type != VN_DIR || !cur->ops->lookup) { vnode_unref(cur); return NULL; }
        struct vnode *next = cur->ops->lookup(cur, comp);
        if (!next) {
            /* keep parent only if this was the final component */
            while (*e == '/') e++;
            if (*e == 0 && parent_out) *parent_out = cur; else vnode_unref(cur);
            return NULL;
        }
        if (next->mount_root) { struct vnode *m = next->mount_root; vnode_ref(m); vnode_unref(next); next = m; }
        if (parent_out) { if (*parent_out) vnode_unref(*parent_out); *parent_out = cur; } else vnode_unref(cur);
        cur = next;
        p = e; while (*p == '/') p++;
    }
    return cur;
}

struct vnode *vfs_lookup_rel(const char *cwd, const char *path) {
    char norm[PATH_MAX];
    vfs_normalize(cwd, path, norm, sizeof norm);
    struct vnode *parent = NULL;
    struct vnode *v = walk(norm, &parent, NULL);
    if (parent) vnode_unref(parent);
    return v;
}
struct vnode *vfs_lookup(const char *path) { return vfs_lookup_rel("/", path); }

int vfs_mount(const char *path, struct vnode *fsroot) {
    if (nmounts >= VFS_MAX_MOUNTS) return -ENOSPC;
    struct vnode *mp = vfs_lookup(path);
    if (!mp) { vfs_mkdir(path); mp = vfs_lookup(path); }
    if (!mp || mp->type != VN_DIR) { vnode_unref(mp); return -ENOTDIR; }
    mutex_lock(&mount_lock);
    if (nmounts >= VFS_MAX_MOUNTS) { mutex_unlock(&mount_lock); vnode_unref(mp); return -ENOSPC; }
    mp->mount_root = fsroot;
    vnode_ref(fsroot);
    fsroot->parent = mp->parent;
    vfs_normalize("/", path, mounts[nmounts].path, PATH_MAX);
    mounts[nmounts].mountpoint = mp;
    mounts[nmounts].root = fsroot;
    nmounts++;
    mutex_unlock(&mount_lock);
    kprintf("vfs: mounted %s on %s\n", fsroot->fs_name ? fsroot->fs_name : "?", path);
    return 0;
}
int vfs_unmount(const char *path) {
    char norm[PATH_MAX]; vfs_normalize("/", path, norm, sizeof norm);
    mutex_lock(&mount_lock);
    for (int i = 0; i < nmounts; i++) {
        if (!strcmp(mounts[i].path, norm)) {
            if (mounts[i].root->ops->sync) mounts[i].root->ops->sync(mounts[i].root);
            mounts[i].mountpoint->mount_root = NULL;
            vnode_unref(mounts[i].root);
            vnode_unref(mounts[i].mountpoint);
            for (int j = i; j < nmounts - 1; j++) mounts[j] = mounts[j + 1];
            nmounts--;
            mutex_unlock(&mount_lock);
            return 0;
        }
    }
    mutex_unlock(&mount_lock);
    return -ENOENT;
}
int vfs_mount_count(void) { return nmounts; }
static bool is_mount_root(struct vnode *v) { mutex_lock(&mount_lock); bool r = false; for (int i = 0; i < nmounts; i++) if (mounts[i].root == v) r = true; mutex_unlock(&mount_lock); return r; }
/* may return NULL if the table shrank since vfs_mount_count(); fs roots are never freed */
struct vnode *vfs_mount_at(int i, char *path, size_t n) {
    mutex_lock(&mount_lock);
    struct vnode *r = NULL;
    if (i >= 0 && i < nmounts) { if (path) strlcpy(path, mounts[i].path, n); r = mounts[i].root; }
    mutex_unlock(&mount_lock);
    return r;
}

int vfs_open_rel(const char *cwd, const char *path, int flags, struct file **out) {
    char norm[PATH_MAX];
    vfs_normalize(cwd, path, norm, sizeof norm);
    struct vnode *parent = NULL; char last[NAME_MAX + 1];
    mutex_lock(&vfs_mutex);
    struct vnode *v = walk(norm, &parent, last);
    if (!v) {
        if (!(flags & O_CREAT) || !parent || !last[0]) { if (parent) vnode_unref(parent); mutex_unlock(&vfs_mutex); return -ENOENT; }
        if (!parent->ops->create) { vnode_unref(parent); mutex_unlock(&vfs_mutex); return -EROFS; }
        v = parent->ops->create(parent, last, VN_FILE);
        if (!v) { vnode_unref(parent); mutex_unlock(&vfs_mutex); return -EIO; }
    }
    if (parent) vnode_unref(parent);
    mutex_unlock(&vfs_mutex);
    if ((flags & O_DIRECTORY) && v->type != VN_DIR) { vnode_unref(v); return -ENOTDIR; }
    if (v->type == VN_DIR && (flags & O_ACCMODE) != O_RDONLY) { vnode_unref(v); return -EISDIR; }
    if ((flags & O_TRUNC) && v->type == VN_FILE && v->ops->truncate) v->ops->truncate(v, 0);
    struct file *f = kzalloc(sizeof *f);
    f->vn = v; f->flags = flags; f->refcount = 1;
    f->pos = (flags & O_APPEND) ? v->size : 0;
    *out = f;
    return 0;
}
int vfs_open(const char *path, int flags, struct file **out) { return vfs_open_rel("/", path, flags, out); }

int64_t vfs_pread(struct vnode *v, uint64_t off, void *buf, size_t n) {
    if (!v->ops->read) return -EINVAL;
    return v->ops->read(v, off, buf, n);
}
int64_t vfs_pwrite(struct vnode *v, uint64_t off, const void *buf, size_t n) {
    if (!v->ops->write) return -EROFS;
    return v->ops->write(v, off, buf, n);
}
int64_t vfs_read(struct file *f, void *buf, size_t n) {
    if (f->vn->type == VN_DIR) return -EISDIR;
    int64_t r = vfs_pread(f->vn, f->pos, buf, n);
    if (r > 0) f->pos += r;
    return r;
}
int64_t vfs_write(struct file *f, const void *buf, size_t n) {
    if ((f->flags & O_ACCMODE) == O_RDONLY && f->vn->type == VN_FILE) return -EBADF;
    if (f->flags & O_APPEND) f->pos = f->vn->size;
    int64_t r = vfs_pwrite(f->vn, f->pos, buf, n);
    if (r > 0) f->pos += r;
    return r;
}
int64_t vfs_seek(struct file *f, int64_t off, int whence) {
    int64_t np;
    switch (whence) {
    case SEEK_SET: np = off; break;
    case SEEK_CUR: np = (int64_t)f->pos + off; break;
    case SEEK_END: np = (int64_t)f->vn->size + off; break;
    default: return -EINVAL;
    }
    if (np < 0) return -EINVAL;
    f->pos = np;
    return np;
}
void vfs_close(struct file *f) {
    if (!f) return;
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST) > 0) return;
    if ((f->flags & O_ACCMODE) != O_RDONLY && f->vn->type == VN_FILE && f->vn->ops->sync) f->vn->ops->sync(f->vn);
    vnode_unref(f->vn);
    kfree(f);
}
struct file *vfs_dup(struct file *f) { if (f) __atomic_add_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST); return f; }

int vfs_stat_rel(const char *cwd, const char *path, struct stat *st) {
    struct vnode *v = vfs_lookup_rel(cwd, path);
    if (!v) return -ENOENT;
    st->type = v->type; st->size = v->size; st->mtime = v->mtime; st->mode = v->mode; st->ino = v->ino; st->nlink = 1;
    vnode_unref(v);
    return 0;
}
int vfs_stat(const char *path, struct stat *st) { return vfs_stat_rel("/", path, st); }

int vfs_readdir(struct vnode *dir, int index, struct dirent *d) {
    if (dir->mount_root) dir = dir->mount_root;
    if (dir->type != VN_DIR || !dir->ops->readdir) return -ENOTDIR;
    return dir->ops->readdir(dir, index, d);
}

static int create_node(const char *path, int type) {
    char norm[PATH_MAX]; vfs_normalize("/", path, norm, sizeof norm);
    struct vnode *parent = NULL; char last[NAME_MAX + 1];
    mutex_lock(&vfs_mutex);
    struct vnode *v = walk(norm, &parent, last);
    if (v) { vnode_unref(v); if (parent) vnode_unref(parent); mutex_unlock(&vfs_mutex); return -EEXIST; }
    if (!parent || !last[0]) { mutex_unlock(&vfs_mutex); return -ENOENT; }
    int r = 0;
    if (!parent->ops->create) r = -EROFS;
    else { struct vnode *n = parent->ops->create(parent, last, type); if (n) vnode_unref(n); else r = -EIO; }
    vnode_unref(parent);
    mutex_unlock(&vfs_mutex);
    return r;
}
int vfs_mkdir(const char *path) { return create_node(path, VN_DIR); }
int vfs_create(const char *path) { return create_node(path, VN_FILE); }

int vfs_unlink(const char *path) {
    char norm[PATH_MAX]; vfs_normalize("/", path, norm, sizeof norm);
    if (!strcmp(norm, "/")) return -EACCES;
    struct vnode *parent = NULL; char last[NAME_MAX + 1];
    mutex_lock(&vfs_mutex);
    struct vnode *v = walk(norm, &parent, last);
    if (!v) { if (parent) vnode_unref(parent); mutex_unlock(&vfs_mutex); return -ENOENT; }
    int r = 0;
    if (v->mount_root || is_mount_root(v)) r = -EBUSY;   /* walk() already stepped into the mounted fs */
    else if (v->type == VN_DIR) { struct dirent d; if (v->ops->readdir && v->ops->readdir(v, 0, &d) == 0) r = -ENOTEMPTY; }
    vnode_unref(v);
    if (r == 0) {
        if (!parent || !parent->ops->unlink) r = -EROFS;
        else r = parent->ops->unlink(parent, last);
    }
    if (parent) vnode_unref(parent);
    mutex_unlock(&vfs_mutex);
    return r;
}

int vfs_truncate(struct vnode *v, uint64_t size) { return v->ops->truncate ? v->ops->truncate(v, size) : -EROFS; }

int vfs_read_file(const char *path, void **data, size_t *size) {
    struct file *f; int r = vfs_open(path, O_RDONLY, &f);
    if (r < 0) return r;
    size_t sz = f->vn->size;
    if (sz >= pmm_free_bytes() / 2) { vfs_close(f); return -ENOMEM; }   /* kmalloc panics rather than fail */
    char *buf = kmalloc(sz + 1);
    size_t got = 0;
    while (got < sz) { int64_t n = vfs_read(f, buf + got, sz - got); if (n <= 0) break; got += n; }
    buf[got] = 0;
    vfs_close(f);
    *data = buf; if (size) *size = got;
    return 0;
}
int vfs_write_file(const char *path, const void *data, size_t size) {
    struct file *f; int r = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f);
    if (r < 0) return r;
    size_t done = 0;
    while (done < size) { int64_t n = vfs_write(f, (const char *)data + done, size - done); if (n <= 0) { r = n < 0 ? (int)n : -EIO; break; } done += n; }
    if (f->vn->ops->sync) f->vn->ops->sync(f->vn);
    vfs_close(f);
    return r;
}

/* rename = copy + unlink (works across filesystems; directories only when empty) */
int vfs_rename(const char *oldp, const char *newp) {
    char a[PATH_MAX], b[PATH_MAX]; vfs_normalize("/", oldp, a, sizeof a); vfs_normalize("/", newp, b, sizeof b);
    struct vnode *va = vfs_lookup(a); if (!va) return -ENOENT;
    if (!strcmp(a, b)) { vnode_unref(va); return 0; }
    /* the same file under another name (FAT is case-insensitive): copying onto itself would lose it */
    struct vnode *vb = vfs_lookup(b);
    bool same = vb && (vb == va || (va->fs && va->fs == vb->fs && va->fs_name && !strcmp(va->fs_name, "fat32") && !strcasecmp(a, b)));
    int type = va->type; struct dirent de;
    bool nonempty = type == VN_DIR && (va->ops->readdir && va->ops->readdir(va, 0, &de) == 0), busy = is_mount_root(va);
    vnode_unref(va); vnode_unref(vb);
    if (busy) return -EBUSY;
    if (nonempty) return -ENOTEMPTY;   /* no recursive move: refuse instead of leaving a half-copied tree */
    if (type == VN_DIR) {
        if (same) return -EEXIST;
        int r = vfs_mkdir(b); if (r < 0) return r;
        r = vfs_unlink(a);
        if (r < 0) vfs_unlink(b);
        return r;
    }
    void *data; size_t sz;
    int r = vfs_read_file(a, &data, &sz); if (r < 0) return r;
    if (same) {   /* case-only change: the data is held in memory across the unlink, restored on failure */
        r = vfs_unlink(a);
        if (r == 0 && (r = vfs_write_file(b, data, sz)) < 0) vfs_write_file(a, data, sz);
        kfree(data);
        return r;
    }
    r = vfs_write_file(b, data, sz);
    kfree(data);
    if (r < 0) return r;
    return vfs_unlink(a);
}

int vfs_sync(void) {
    mutex_lock(&mount_lock);
    for (int i = 0; i < nmounts; i++) if (mounts[i].root->ops->sync) mounts[i].root->ops->sync(mounts[i].root);
    mutex_unlock(&mount_lock);
    return 0;
}

const char *vfs_basename(const char *path) { const char *s = strrchr(path, '/'); return s ? s + 1 : path; }
void vfs_dirname(const char *path, char *out, size_t n) {
    strlcpy(out, path, n);
    char *s = strrchr(out, '/');
    if (!s) { strlcpy(out, ".", n); return; }
    if (s == out) s[1] = 0; else *s = 0;
}
