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

/* Lexical normalization (. and .. resolved, slashes collapsed). Fails instead of truncating: a prefix of
 * the requested path names a different file. An empty path is ENOENT (POSIX), never "the cwd". */
int vfs_normalize(const char *cwd, const char *path, char *out, size_t n) {
    char tmp[PATH_MAX];
    if (n) out[0] = 0;
    if (!path || !path[0]) return -ENOENT;
    if (n < 2) return -ENAMETOOLONG;
    if (path[0] == '/') { if (strlcpy(tmp, path, sizeof tmp) >= sizeof tmp) return -ENAMETOOLONG; }
    else {
        if (!cwd || !cwd[0]) cwd = "/";
        size_t cl = strlen(cwd), pl = strlen(path);
        if (cl + 1 + pl >= sizeof tmp) return -ENAMETOOLONG;
        memcpy(tmp, cwd, cl); tmp[cl] = '/'; memcpy(tmp + cl + 1, path, pl + 1);
    }
    /* build out component by component; ".." pops back to the previous '/' */
    size_t pos = 0;
    char *save, *tok = strtok_r(tmp, "/", &save);
    while (tok) {
        if (!strcmp(tok, ".")) {}
        else if (!strcmp(tok, "..")) { while (pos && out[pos - 1] != '/') pos--; if (pos) pos--; }
        else {
            size_t l = strlen(tok);
            if (pos + 1 + l >= n) { out[0] = 0; return -ENAMETOOLONG; }
            out[pos++] = '/'; memcpy(out + pos, tok, l); pos += l;
        }
        tok = strtok_r(NULL, "/", &save);
    }
    if (!pos) out[pos++] = '/';
    out[pos] = 0;
    return 0;
}

/* Walk from root; result is referenced. When only the FINAL component is missing, *parent_out gets its
 * (referenced) directory and last_comp its name; on any other failure both are cleared, so callers
 * can't create the entry somewhere else. */
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
        if (l > NAME_MAX) goto fail;
        memcpy(comp, p, l); comp[l] = 0;
        if (last_comp) strcpy(last_comp, comp);
        if (cur->type != VN_DIR || !cur->ops->lookup) goto fail;
        struct vnode *next = cur->ops->lookup(cur, comp);
        if (!next) {
            while (*e == '/') e++;
            if (*e || !parent_out) goto fail;   /* an intermediate directory is missing */
            if (*parent_out) vnode_unref(*parent_out);
            *parent_out = cur;
            return NULL;
        }
        if (next->mount_root) { struct vnode *m = next->mount_root; vnode_ref(m); vnode_unref(next); next = m; }
        if (parent_out) { if (*parent_out) vnode_unref(*parent_out); *parent_out = cur; } else vnode_unref(cur);
        cur = next;
        p = e; while (*p == '/') p++;
    }
    return cur;
fail:
    vnode_unref(cur);
    if (parent_out && *parent_out) { vnode_unref(*parent_out); *parent_out = NULL; }
    if (last_comp) last_comp[0] = 0;
    return NULL;
}

struct vnode *vfs_lookup_rel(const char *cwd, const char *path) {
    char norm[PATH_MAX];
    if (vfs_normalize(cwd, path, norm, sizeof norm) < 0) return NULL;
    struct vnode *parent = NULL;
    struct vnode *v = walk(norm, &parent, NULL);
    if (parent) vnode_unref(parent);
    return v;
}
struct vnode *vfs_lookup(const char *path) { return vfs_lookup_rel("/", path); }

int vfs_mount(const char *path, struct vnode *fsroot) {
    if (nmounts >= VFS_MAX_MOUNTS) return -ENOSPC;
    char norm[PATH_MAX]; int r = vfs_normalize("/", path, norm, sizeof norm); if (r < 0) return r;
    struct vnode *mp = vfs_lookup(norm);
    if (!mp) { vfs_mkdir(norm); mp = vfs_lookup(norm); }   /* only creates the last component: walk() refuses a missing parent */
    if (!mp || mp->type != VN_DIR) { vnode_unref(mp); return -ENOTDIR; }
    mutex_lock(&mount_lock);
    if (nmounts >= VFS_MAX_MOUNTS) { mutex_unlock(&mount_lock); vnode_unref(mp); return -ENOSPC; }
    mp->mount_root = fsroot;
    vnode_ref(fsroot);
    fsroot->parent = mp->parent;
    strlcpy(mounts[nmounts].path, norm, PATH_MAX);
    mounts[nmounts].mountpoint = mp;
    mounts[nmounts].root = fsroot;
    nmounts++;
    mutex_unlock(&mount_lock);
    kprintf("vfs: mounted %s on %s\n", fsroot->fs_name ? fsroot->fs_name : "?", path);
    return 0;
}
int vfs_unmount(const char *path) {
    char norm[PATH_MAX]; int r = vfs_normalize("/", path, norm, sizeof norm); if (r < 0) return r;
    mutex_lock(&mount_lock);
    for (int i = 0; i < nmounts; i++) {
        if (!strcmp(mounts[i].path, norm)) {
            if (mounts[i].root->ops->sync && mounts[i].root->ops->sync(mounts[i].root) < 0) kprintf("vfs: %s: sync failed on unmount\n", norm);
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
    int r = vfs_normalize(cwd, path, norm, sizeof norm); if (r < 0) return r;
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
    if ((flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY && v->type == VN_FILE && v->ops->truncate && (r = v->ops->truncate(v, 0)) < 0) { vnode_unref(v); return r; }
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
    if (v->type == VN_DIR) return -EISDIR;   /* FAT would rewrite the directory's clusters as file data */
    if (!v->ops->write) return -EROFS;
    return v->ops->write(v, off, buf, n);
}
int64_t vfs_read(struct file *f, void *buf, size_t n) {
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (f->vn->type == VN_DIR) return -EISDIR;
    int64_t r = vfs_pread(f->vn, f->pos, buf, n);
    if (r > 0) f->pos += r;
    return r;
}
int64_t vfs_write(struct file *f, const void *buf, size_t n) {
    if (f->vn->type == VN_DIR) return -EISDIR;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (f->flags & O_APPEND) {
        /* the fs picks the offset under its own lock: vn->size may be stale (another FAT handle) or racing */
        if (f->vn->ops->append) { uint64_t end; int64_t r = f->vn->ops->append(f->vn, buf, n, &end); if (r >= 0) f->pos = end; return r; }
        f->pos = f->vn->size;
    }
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
int vfs_close(struct file *f) {
    if (!f) return 0;
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST) > 0) return 0;
    int r = 0;
    if ((f->flags & O_ACCMODE) != O_RDONLY && f->vn->type == VN_FILE && f->vn->ops->sync) r = f->vn->ops->sync(f->vn);
    vnode_unref(f->vn);
    kfree(f);
    return r < 0 ? r : 0;
}
struct file *vfs_dup(struct file *f) { if (f) __atomic_add_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST); return f; }

int vfs_stat_rel(const char *cwd, const char *path, struct stat *st) {
    char norm[PATH_MAX]; int r = vfs_normalize(cwd, path, norm, sizeof norm); if (r < 0) return r;
    struct vnode *v = vfs_lookup(norm);
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
    char norm[PATH_MAX]; int nr = vfs_normalize("/", path, norm, sizeof norm); if (nr < 0) return nr;
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
    char norm[PATH_MAX]; int nr = vfs_normalize("/", path, norm, sizeof norm); if (nr < 0) return nr;
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

int vfs_truncate(struct vnode *v, uint64_t size) {
    if (v->type == VN_DIR) return -EISDIR;
    if (v->type != VN_FILE) return -EINVAL;
    return v->ops->truncate ? v->ops->truncate(v, size) : -EROFS;
}

int vfs_read_file(const char *path, void **data, size_t *size) {
    struct file *f; int r = vfs_open(path, O_RDONLY, &f);
    if (r < 0) return r;
    size_t sz = f->vn->size;
    char *buf = sz != (size_t)-1 ? kmalloc_try(sz + 1) : NULL;   /* size comes from disk */
    if (!buf) { vfs_close(f); return -ENOMEM; }
    size_t got = 0;
    while (got < sz) {
        int64_t n = vfs_read(f, buf + got, sz - got);
        /* A whole-file copy must not turn a failed or incomplete read into
         * success: rename would then delete the only complete copy. */
        if (n <= 0) { kfree(buf); vfs_close(f); return n < 0 ? (int)n : -EIO; }
        got += n;
    }
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
    int c = vfs_close(f);   /* syncs: a buffered fs (btrfs) only reports write errors here */
    return r < 0 ? r : c;
}

/* rename = copy + unlink (works across filesystems; directories only when empty) */
int vfs_rename(const char *oldp, const char *newp) {
    char a[PATH_MAX], b[PATH_MAX]; int nr;
    if ((nr = vfs_normalize("/", oldp, a, sizeof a)) < 0 || (nr = vfs_normalize("/", newp, b, sizeof b)) < 0) return nr;
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
    int r = 0;
    mutex_lock(&mount_lock);
    for (int i = 0; i < nmounts; i++) if (mounts[i].root->ops->sync) { int e = mounts[i].root->ops->sync(mounts[i].root); if (e < 0 && !r) r = e; }
    mutex_unlock(&mount_lock);
    return r;
}

const char *vfs_basename(const char *path) { const char *s = strrchr(path, '/'); return s ? s + 1 : path; }
void vfs_dirname(const char *path, char *out, size_t n) {
    strlcpy(out, path, n);
    char *s = strrchr(out, '/');
    if (!s) { strlcpy(out, ".", n); return; }
    if (s == out) s[1] = 0; else *s = 0;
}
