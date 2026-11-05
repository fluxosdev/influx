/*
 * The root filesystem of an installed system.
 *
 * The installed disk's btrfs volume (marked by /.influx_root) becomes "/", so whatever is written
 * anywhere survives a reboot. Only /dev, /tmp and /mnt stay in memory: device nodes, scratch files
 * that start empty at each boot, and the mount points of other disks.
 *
 * The system image (initrd.tar on the EFI partition) is still what the system *is*: at boot, when
 * it differs from the image of the last boot, it is copied onto the volume. A manifest of what the
 * previous image installed (/var/lib/influx/image.manifest) decides file by file, as a package
 * manager would:
 *   - a file the image did not change is not touched;
 *   - a file the image changed is replaced, unless it was changed on the disk too (kept, logged);
 *   - a file no longer in the image is removed, unless it was changed on the disk;
 *   - files under /home are seeded once and never replaced or recreated.
 * Writes go to a temporary name and are renamed into place, and the stamp
 * (/var/lib/influx/image) is written last, so an interrupted update simply runs again.
 *
 * Packages (a boot module such as "devtools", or `pkg install file.tar`) are kept the same way,
 * each with a manifest and stamp of its own (/var/lib/influx/<name>.manifest, <name>); the system
 * image's are "image".
 *
 * Volumes made by an installer from before this (the home directory at the top of the volume,
 * mounted at /home) are moved to the new layout once.
 */
#include "fs.h"
#include "mm.h"
#include "string.h"
#include "blockdev.h"

uint32_t crc32c(uint32_t seed, const void *data, size_t n);
void fb_boot_progress(const char *msg);

#define STAGE      "/mnt/.sysroot"
#define STATE_DIR  "var/lib/influx"
#define MARKER     STAGE "/.influx_root"
#define MARKER_V2  "inFlux root volume\nlayout 2\n"
#define TMP_SUFFIX ".influx-new"

static bool boot_done, attached;
void sysroot_boot_done(void) { boot_done = true; }

static uint32_t crc_of(const void *d, size_t n) { return crc32c(~0u, d, n) ^ ~0u; }

/* ---- the manifest: path -> crc of the image's version of that file ---- */
struct mf { char *path; uint32_t crc; bool seen; };
struct manifest { struct mf *e; size_t n, cap; uint32_t *slot; size_t nslot; char *text; };

static uint32_t path_hash(const char *p) { return crc32c(~0u, p, strlen(p)); }
static struct mf *mf_find(struct manifest *m, const char *path) {
    if (!m->nslot) return NULL;
    for (size_t i = path_hash(path) & (m->nslot - 1); m->slot[i]; i = (i + 1) & (m->nslot - 1))
        if (!strcmp(m->e[m->slot[i] - 1].path, path)) return &m->e[m->slot[i] - 1];
    return NULL;
}
/* "crc8hex path\n" lines; the paths point into m->text */
static void mf_load(struct manifest *m, const char *file) {
    memset(m, 0, sizeof *m);
    size_t sz;
    if (vfs_read_file(file, (void **)&m->text, &sz) < 0) { m->text = NULL; return; }
    for (char *line = m->text, *next; line && *line; line = next) {
        next = strchr(line, '\n'); if (next) *next++ = 0;
        if (strlen(line) < 10 || line[8] != ' ') continue;
        uint32_t crc = 0; bool ok = true;
        for (int i = 0; i < 8; i++) { char c = line[i]; int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; if (v < 0) ok = false; crc = crc << 4 | (uint32_t)(v & 15); }
        if (!ok) continue;
        if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 256; struct mf *ne = krealloc_try(m->e, m->cap * sizeof *m->e); if (!ne) break; m->e = ne; }
        m->e[m->n++] = (struct mf){ line + 9, crc, false };
    }
    for (m->nslot = 64; m->nslot < m->n * 2 + 1; m->nslot *= 2) {}
    m->slot = kzalloc(m->nslot * sizeof *m->slot);
    for (size_t k = 0; k < m->n; k++) {
        size_t i = path_hash(m->e[k].path) & (m->nslot - 1);
        while (m->slot[i]) i = (i + 1) & (m->nslot - 1);
        m->slot[i] = (uint32_t)k + 1;
    }
}
static void mf_free(struct manifest *m) { kfree(m->e); kfree(m->slot); kfree(m->text); }

/* the new manifest, built while syncing */
struct text { char *buf; size_t len, cap; };
static void text_add(struct text *t, uint32_t crc, const char *path) {
    size_t need = t->len + strlen(path) + 11;
    if (need > t->cap) { size_t nc = MAX(need, t->cap * 2 + 4096); char *nb = krealloc_try(t->buf, nc); if (!nb) return; t->buf = nb; t->cap = nc; }
    t->len += (size_t)snprintf(t->buf + t->len, t->cap - t->len, "%08x %s\n", crc, path);
}

/* ---- files ---- */
static int file_crc(const char *path, uint64_t expect_size, uint32_t *out, bool *size_differs) {
    struct file *f;
    int r = vfs_open(path, O_RDONLY, &f);
    if (r < 0) return r;
    *size_differs = f->vn->size != expect_size;
    uint8_t *buf = kmalloc(64 * 1024);
    uint32_t c = ~0u; int64_t n;
    while ((n = vfs_read(f, buf, 64 * 1024)) > 0) c = crc32c(c, buf, (size_t)n);
    kfree(buf); vfs_close(f);
    if (n < 0) return (int)n;
    *out = c ^ ~0u;
    return 0;
}
/* the image's version of a file, through a temporary name, so the old one stays whole until the rename */
static int install_file(const char *path, const struct initrd_entry *e) {
    char tmp[PATH_MAX];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s%s", path, TMP_SUFFIX) >= sizeof tmp) return -ENAMETOOLONG;
    struct file *f;
    int r = vfs_open_mode("/", tmp, O_WRONLY | O_CREAT | O_TRUNC, e->mode, &f);
    if (r < 0) return r;
    uint64_t done = 0;
    while (done < e->size) {
        int64_t n = vfs_write(f, (const uint8_t *)e->data + done, (size_t)(e->size - done));
        if (n <= 0) { r = n < 0 ? (int)n : -EIO; break; }
        done += (uint64_t)n;
    }
    int c = vfs_close(f);
    if (r == 0) r = c;
    if (r == 0) {
        struct vattr a = { VA_MODE | VA_MTIME, e->mode, e->mtime, 0 };
        struct vnode *v = vfs_lookup(tmp);
        if (v) { vfs_setattr(v, &a); vnode_unref(v); }
        r = vfs_rename_posix(tmp, path);
    }
    if (r < 0) vfs_unlink(tmp);
    return r;
}

struct sync { const char *root; struct manifest old; struct text manifest; int written, kept, skipped, failed; };

/* root + "/" + rel, for root "/" too */
static bool join(char *out, size_t n, const char *root, const char *rel) {
    return (size_t)snprintf(out, n, "%s/%s", strcmp(root, "/") ? root : "", rel) < n;
}

static bool under_home(const char *rel) { return !strncmp(rel, "home", 4) && (rel[4] == '/' || !rel[4]); }

static int sync_one(void *ctx, const struct initrd_entry *e) {
    struct sync *s = ctx;
    if (!e->path[0]) return 0;   /* the root directory itself */
    char path[PATH_MAX];
    if (!join(path, sizeof path, s->root, e->path)) { s->failed++; return 0; }
    bool home = under_home(e->path);
    struct stat st;
    bool exists = vfs_lstat(path, &st) == 0;
    int r = 0;
    switch (e->type) {
    case '5':
        if (!exists) r = vfs_mkdir_mode(path, e->mode);
        break;
    case '2': {   /* symbolic links follow the image, unless something else took the name */
        if (exists && st.type != VN_LNK) { if (!home) kprintf("sysroot: /%s: kept (the image has a symbolic link there)\n", e->path); s->kept++; break; }
        if (exists) {
            char t[PATH_MAX]; int l = vfs_readlink(path, t, sizeof t - 1);
            if (l >= 0) { t[l] = 0; if (!strcmp(t, e->link) || home) { s->skipped++; break; } }
            vfs_unlink(path);
        }
        r = vfs_symlink(e->link, path);
        if (r == 0) s->written++;
        break; }
    case '1': {   /* a hard link: the same file as an earlier entry, unless it already is */
        char target[PATH_MAX]; struct stat ts;
        join(target, sizeof target, s->root, e->link);
        if (exists && vfs_stat(target, &ts) == 0 && ts.ino == st.ino) { s->skipped++; break; }
        if (exists) { if (home || st.type != VN_FILE) { s->kept++; break; } vfs_unlink(path); }
        r = vfs_link(target, path);
        if (r == 0) s->written++;
        break; }
    case '0': {
        uint32_t crc = crc_of(e->data, (size_t)e->size);
        struct mf *old = mf_find(&s->old, e->path);
        if (old) old->seen = true;
        text_add(&s->manifest, crc, e->path);
        if (!exists) {
            if (home && old) { s->skipped++; break; }   /* seeded once, deleted by its owner since */
            r = install_file(path, e);
            if (r == 0) s->written++;
            break;
        }
        if (st.type != VN_FILE) { kprintf("sysroot: /%s: kept (not a file on the disk)\n", e->path); s->kept++; break; }
        if (old && old->crc == crc) { s->skipped++; break; }   /* the image did not change it */
        uint32_t disk; bool size_differs;
        if ((r = file_crc(path, e->size, &disk, &size_differs)) < 0) break;
        if (!size_differs && disk == crc) { s->skipped++; break; }   /* already the image's version */
        if (old && disk == old->crc && !home) {   /* unchanged since the last image: update it */
            r = install_file(path, e);
            if (r == 0) s->written++;
            break;
        }
        kprintf("sysroot: /%s: kept your version (the system image has a different one)\n", e->path);
        s->kept++;
        break; }
    }
    if (r < 0) { kprintf("sysroot: /%s: cannot update (%d)\n", e->path, r); s->failed++; }
    return 0;
}

/* files the previous image installed and this one no longer has, unless changed on the disk */
static void remove_dropped(struct sync *s) {
    for (size_t i = 0; i < s->old.n; i++) {
        struct mf *m = &s->old.e[i];
        if (m->seen || under_home(m->path)) continue;
        char path[PATH_MAX]; struct stat st;
        if (!join(path, sizeof path, s->root, m->path) || vfs_lstat(path, &st) < 0 || st.type != VN_FILE) continue;
        uint32_t disk; bool sd;
        if (file_crc(path, st.size, &disk, &sd) == 0 && disk == m->crc && vfs_unlink(path) == 0) kprintf("sysroot: /%s: removed (no longer in the system image)\n", m->path);
    }
}

static void mkdirs(const char *path) {
    char p[PATH_MAX]; strlcpy(p, path, sizeof p);
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; vfs_mkdir(p); *s = '/'; }
    vfs_mkdir(p);
}

int sysroot_sync(const char *root, const char *name, const void *tar, size_t size) {
    char dir[PATH_MAX], manifest[PATH_MAX], stampf[PATH_MAX], stamp[48], *old = NULL; size_t sz;
    join(dir, sizeof dir, root, STATE_DIR);
    snprintf(manifest, sizeof manifest, "%s/%s.manifest", dir, name);
    snprintf(stampf, sizeof stampf, "%s/%s", dir, name);
    snprintf(stamp, sizeof stamp, "%08x %lu\n", crc_of(tar, size), (unsigned long)size);
    bool same = vfs_read_file(stampf, (void **)&old, &sz) == 0 && !strcmp(old, stamp);
    kfree(old);
    if (same) { kprintf("sysroot: '%s' is unchanged since it was last installed\n", name); return 0; }
    if (!boot_done) fb_boot_progress("Updating the system");   /* the boot splash; never over the desktop */
    struct sync *s = kzalloc(sizeof *s);
    s->root = root;
    mf_load(&s->old, manifest);
    kprintf("sysroot: installing '%s' (%lu KiB; the last version listed %lu files)\n", name, (unsigned long)(size / 1024), s->old.n);
    initrd_foreach(tar, size, sync_one, s);
    remove_dropped(s);
    int r = 0;
    mkdirs(dir);
    if (s->failed) r = -EIO;   /* no new stamp: the next attempt redoes it */
    else if ((r = vfs_write_file(manifest, s->manifest.buf ? s->manifest.buf : "", s->manifest.len)) == 0) r = vfs_write_file(stampf, stamp, strlen(stamp));
    kprintf("sysroot: '%s': %d written, %d unchanged, %d kept, %d failed\n", name, s->written, s->skipped, s->kept, s->failed);
    mf_free(&s->old); kfree(s->manifest.buf); kfree(s);
    return r;
}

/* the first layout kept the home directory at the top of the volume (mounted at /home) */
static int migrate_layout(void) {
    char *m; size_t sz;
    if (vfs_read_file(MARKER, (void **)&m, &sz) < 0) return -EIO;
    bool v2 = strstr(m, "layout 2") != NULL;
    kfree(m);
    if (v2) return 0;
    kprintf("sysroot: moving the home directory to /home (first boot with the whole system on the disk)\n");
    const char *tmp = STAGE "/.influx-home-migrate";
    int r = vfs_mkdir(tmp);
    if (r < 0 && r != -EEXIST) return r;
    struct vnode *root = vfs_lookup(STAGE);
    if (!root) return -EIO;
    struct dirent d; int i = 0;
    while (vfs_readdir(root, i, &d) == 0) {
        if (!strcmp(d.name, ".influx_root") || !strcmp(d.name, ".influx-home-migrate")) { i++; continue; }
        char a[PATH_MAX], b[PATH_MAX];
        snprintf(a, sizeof a, STAGE "/%s", d.name); snprintf(b, sizeof b, "%s/%s", tmp, d.name);
        if ((r = vfs_rename_posix(a, b)) < 0) { kprintf("sysroot: cannot move /%s into /home (%d)\n", d.name, r); vnode_unref(root); return r; }
    }
    vnode_unref(root);
    if ((r = vfs_rename_posix(tmp, STAGE "/home")) < 0) return r;
    return vfs_write_file(MARKER, MARKER_V2, strlen(MARKER_V2));
}

/* the volume belongs to the disk the system booted from (never a live CD's neighbour) */
static bool booted_from(struct blockdev *d) {
    if (boot_info.boot_media != 0) return false;   /* optical or network: a live system */
    struct blockdev *disk = d->parent ? d->parent : d;
    static const uint8_t zero[16];
    if (!memcmp(boot_info.boot_disk_guid, zero, 16)) return true;   /* the bootloader didn't say: the first installed volume */
    return !memcmp(boot_info.boot_disk_guid, disk->gpt_guid, 16);
}

bool sysroot_attach(struct blockdev *d, struct vnode *vol) {
    if (boot_done || attached || vfs_root_is_disk()) return false;
    if (kernel_cmdline_has("volatile")) { kprintf("sysroot: boot option 'volatile': the system runs from memory, %s stays under /mnt\n", d->name); return false; }
    if (!booted_from(d)) { kprintf("sysroot: %s holds an installed inFlux, but the system did not boot from it\n", d->name); return false; }
    vfs_mkdir("/mnt");
    if (vfs_mount(STAGE, vol) < 0) return false;
    int r = migrate_layout();
    if (r == 0) {
        int e = 0;
        if (boot_info.initrd) {
            e = sysroot_sync(STAGE, "image", boot_info.initrd, boot_info.initrd_size);   /* a failed file is retried at the next boot */
        }
        if (e < 0) kprintf("sysroot: the system image was not completely installed (%d)\n", e);
        vol->ops->sync(vol);
    }
    vfs_unmount(STAGE);
    vfs_unlink(STAGE);
    if (r < 0) { kprintf("sysroot: %s: the volume layout could not be updated (%d); the system runs from memory\n", d->name, r); return false; }
    static const char *const keep[] = { "/dev", "/tmp", "/mnt", NULL };
    attached = vfs_switch_root(vol, keep) >= 0 || vfs_root_is_disk();
    if (attached) kprintf("sysroot: %s is the root filesystem; everything outside /tmp is kept across restarts\n", d->name);
    return attached;
}
