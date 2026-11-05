#include "fs.h"
#include "mm.h"
#include "string.h"

struct tar_hdr {
    char name[100], mode[8], uid[8], gid[8], size[12], mtime[12], chksum[8];
    char typeflag, linkname[100], magic[6], version[2], uname[32], gname[32], devmajor[8], devminor[8], prefix[155], pad[12];
};

static uint64_t octal(const char *s, int n) { uint64_t v = 0; for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (s[i] - '0'); return v; }

/* Walks a ustar archive: calls fn for every entry, in archive order, with the path relative to the
 * archive's root ("" for the root itself, no "./" and no trailing '/'). Stops early when fn returns
 * nonzero and returns that. */
int initrd_foreach(const void *tar, size_t size, int (*fn)(void *ctx, const struct initrd_entry *e), void *ctx) {
    const uint8_t *p = tar, *end = p + size;
    while (p + 512 <= end) {
        const struct tar_hdr *h = (const struct tar_hdr *)p;
        if (h->name[0] == 0) break;
        uint64_t fsize = octal(h->size, 12);
        if (fsize > (uint64_t)(end - p) - 512) { kprintf("initrd: entry '%.100s' runs past the archive end\n", h->name); break; }
        struct initrd_entry e;
        char name[260];
        if (h->prefix[0]) snprintf(name, sizeof name, "%.155s/%.100s", h->prefix, h->name);
        else snprintf(name, sizeof name, "%.100s", h->name);
        const char *n = name;
        if (n[0] == '.' && n[1] == '/') n += 2;
        else if (n[0] == '.' && !n[1]) n++;
        strlcpy(e.path, n, sizeof e.path);
        size_t pl = strlen(e.path);
        while (pl && e.path[pl - 1] == '/') e.path[--pl] = 0;
        e.type = h->typeflag ? h->typeflag : '0';
        e.mode = (uint32_t)octal(h->mode, 8) & 07777;
        e.mtime = octal(h->mtime, 12);
        e.data = p + 512; e.size = fsize;
        snprintf(e.link, sizeof e.link, "%.100s", h->linkname);
        if (e.type == '1') {   /* hard links name archive paths: make them root-relative too */
            const char *t = e.link; if (t[0] == '.' && t[1] == '/') t += 2;
            memmove(e.link, t, strlen(t) + 1);
        }
        int r = fn(ctx, &e);
        if (r) return r;
        uint64_t adv = 512 + ALIGN_UP(fsize, 512);
        if (adv > (uint64_t)(end - p)) break;   /* last entry's padding is missing */
        p += adv;
    }
    return 0;
}

struct unpack { const char *dest; int files, dirs; };
static int unpack_one(void *ctx, const struct initrd_entry *e) {
    struct unpack *u = ctx;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", strcmp(u->dest, "/") ? u->dest : "", e->path);
    if (!path[0]) strcpy(path, "/");
    if (e->type == '5') {
        if (e->path[0] && vfs_mkdir_mode(path, e->mode) != -EEXIST) { u->dirs++; return 0; }
        struct vnode *v = vfs_lookup(path);   /* there already (the root, or /dev): just its mode */
        if (v) { struct vattr a = { VA_MODE, e->mode, 0, 0 }; vfs_setattr(v, &a); vnode_unref(v); }
    } else if (e->type == '2' || e->type == '1') {   /* symbolic link, hard link (to an earlier entry) */
        int r;
        if (e->type == '2') r = vfs_symlink(e->link, path);
        else { char tp[PATH_MAX]; snprintf(tp, sizeof tp, "%s/%s", strcmp(u->dest, "/") ? u->dest : "", e->link); r = vfs_link(tp, path); }
        if (r == 0) u->files++; else kprintf("initrd: %s: cannot create link (%d)\n", path, r);
    } else if (e->type == '0') {
        if (vfs_write_file(path, e->data, e->size) == 0) {
            struct vnode *v = vfs_lookup(path);
            if (v) { v->mode = e->mode; v->mtime = e->mtime; v->mtime_nsec = 0; vnode_unref(v); }
            u->files++;
        }
    }
    return 0;
}

void initrd_unpack(const void *tar, size_t size, const char *dest) {
    struct unpack u = { dest, 0, 0 };
    initrd_foreach(tar, size, unpack_one, &u);
    kprintf("initrd: unpacked %d files, %d directories\n", u.files, u.dirs);
}
