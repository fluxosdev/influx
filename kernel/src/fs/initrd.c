#include "fs.h"
#include "mm.h"
#include "string.h"

struct tar_hdr {
    char name[100], mode[8], uid[8], gid[8], size[12], mtime[12], chksum[8];
    char typeflag, linkname[100], magic[6], version[2], uname[32], gname[32], devmajor[8], devminor[8], prefix[155], pad[12];
};

static uint64_t octal(const char *s, int n) { uint64_t v = 0; for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (s[i] - '0'); return v; }

void initrd_unpack(const void *tar, size_t size, const char *dest) {
    const uint8_t *p = tar, *end = p + size;
    int files = 0, dirs = 0;
    while (p + 512 <= end) {
        const struct tar_hdr *h = (const struct tar_hdr *)p;
        if (h->name[0] == 0) break;
        uint64_t fsize = octal(h->size, 12);
        char path[PATH_MAX];
        char name[256];
        if (h->prefix[0]) snprintf(name, sizeof name, "%.155s/%.100s", h->prefix, h->name);
        else snprintf(name, sizeof name, "%.100s", h->name);
        const char *n = name;
        if (n[0] == '.' && n[1] == '/') n += 2;
        snprintf(path, sizeof path, "%s/%s", dest, n);
        size_t pl = strlen(path);
        while (pl > 1 && path[pl - 1] == '/') path[--pl] = 0;
        if (h->typeflag == '5') { if (strcmp(n, "") && strcmp(n, ".")) { vfs_mkdir(path); dirs++; } }
        else if (h->typeflag == '0' || h->typeflag == 0) {
            if (vfs_write_file(path, p + 512, fsize) == 0) {
                struct vnode *v = vfs_lookup(path);
                if (v) { v->mode = (uint32_t)octal(h->mode, 8); v->mtime = octal(h->mtime, 12); vnode_unref(v); }
                files++;
            }
        }
        p += 512 + ALIGN_UP(fsize, 512);
    }
    kprintf("initrd: unpacked %d files, %d directories\n", files, dirs);
}
