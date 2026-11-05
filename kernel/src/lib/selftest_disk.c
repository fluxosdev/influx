/*
 * Test hooks for `make test-persist` (tests/run-persist.sh), run by the selftest thread only when
 * the boot command line asks for them: install onto a blank disk, point the installed system's
 * limine.conf at the next test stage, and fake a system update by appending files to the
 * initrd.tar on its EFI partition.
 */
#include "fs.h"
#include "mm.h"
#include "string.h"
#include "installer.h"

struct vnode *fat32_mount(struct blockdev *d);
struct vnode *btrfs_mount(struct blockdev *d);
int btrfs_unmount(struct vnode *root);

/* the first disk, if its first 128 KiB are zeros (never a disk with anything on it) */
static struct blockdev *blank_sda(void) {
    struct blockdev *d = blockdev_find("sda");
    if (!d) return NULL;
    uint8_t *buf = kmalloc(128 * 1024);
    bool blank = buf && blockdev_read(d, 0, 256, buf) == 0;
    for (int i = 0; blank && i < 128 * 1024; i++) if (buf[i]) blank = false;
    kfree(buf);
    return blank ? d : NULL;
}

/* sets (or replaces) the cmdline of the inFlux entry in esp/limine.conf */
static int esp_set_cmdline(const char *esp, const char *cmdline) {
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/limine.conf", esp);
    char *conf; size_t sz;
    int r = vfs_read_file(path, (void **)&conf, &sz);
    if (r < 0) return r;
    size_t cap = sz + strlen(cmdline) + 64;
    char *out = kzalloc(cap); size_t o = 0;
    for (char *line = conf, *next; *line; line = next) {
        next = strchr(line, '\n'); next = next ? next + 1 : line + strlen(line);
        size_t l = (size_t)(next - line);
        if (!strncmp(line, "    cmdline:", 12)) continue;
        memcpy(out + o, line, l); o += l;
        if (!strncmp(line, "    module_string: initrd", 25) && cmdline[0]) o += (size_t)snprintf(out + o, cap - o, "    cmdline: %s\n", cmdline);
    }
    r = vfs_write_file(path, out, o);
    kfree(out); kfree(conf);
    return r;
}

/* turns a fresh install into the first layout (home directory at the top of the volume, which was
 * mounted at /home), to test the move to the current one */
static int make_layout_v1(void) {
    struct blockdev *p = blockdev_find("sda3");
    struct vnode *root = p ? btrfs_mount(p) : NULL;
    if (!root || vfs_mount("/mnt/.selftest-root", root) < 0) return -1;
    int r = vfs_rename_posix("/mnt/.selftest-root/home/flux", "/mnt/.selftest-root/flux");
    if (r == 0) r = vfs_unlink("/mnt/.selftest-root/home");
    if (r == 0) r = vfs_write_file("/mnt/.selftest-root/flux/from-v1", "written under the first layout\n", 31);
    if (r == 0) r = vfs_write_file("/mnt/.selftest-root/.influx_root", "inFlux root volume\n", 19);
    vfs_unmount("/mnt/.selftest-root");
    int s = btrfs_unmount(root);
    return r < 0 ? r : s;
}

int selftest_install(const char *next_cmdline) {
    struct blockdev *d = blank_sda();
    if (!d) { kprintf("selftest-install: sda is missing or not blank\n"); return -1; }
    int r = install_run(d, NULL, NULL);
    if (r < 0) return r;
    if (kernel_cmdline_has("selftest-v1") && (r = make_layout_v1()) < 0) { kprintf("selftest-install: cannot make the old layout (%d)\n", r); return r; }
    struct blockdev *esp = blockdev_find("sda2");
    struct vnode *root = esp ? fat32_mount(esp) : NULL;
    if (!root || (r = vfs_mount("/mnt/.selftest-esp", root)) < 0) { kprintf("selftest-install: cannot mount the new ESP\n"); return -1; }
    r = esp_set_cmdline("/mnt/.selftest-esp", next_cmdline);
    if (r == 0) r = root->ops->sync ? root->ops->sync(root) : 0;
    vfs_unmount("/mnt/.selftest-esp");
    return r;
}

int selftest_next_stage(const char *next_cmdline) {
    int r = esp_set_cmdline("/mnt/.sda2", next_cmdline);
    vfs_sync();
    return r;
}

/* a ustar header for a regular file (what tools/mkinitrd.c writes) */
static void tar_header(uint8_t *h, const char *name, size_t size) {
    memset(h, 0, 512);
    strlcpy((char *)h, name, 100);
    snprintf((char *)h + 100, 8, "%07o", 0644);
    snprintf((char *)h + 108, 8, "%07o", 0); snprintf((char *)h + 116, 8, "%07o", 0);
    snprintf((char *)h + 124, 12, "%011lo", (unsigned long)size);
    snprintf((char *)h + 136, 12, "%011lo", 1767225600ul);
    h[156] = '0';
    memcpy(h + 257, "ustar", 6); memcpy(h + 263, "00", 2);
    memset(h + 148, ' ', 8);
    unsigned sum = 0; for (int i = 0; i < 512; i++) sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum); h[155] = ' ';
}

/* a "system update": the image on the ESP gets later entries that add and replace files */
int selftest_update_image(int nfiles, const char *const *names, const char *const *contents) {
    char *tar; size_t sz;
    int r = vfs_read_file("/mnt/.sda2/boot/initrd.tar", (void **)&tar, &sz);
    if (r < 0) return r;
    size_t end = 0;   /* past the last entry */
    while (end + 512 <= sz && tar[end]) {
        uint64_t fsz = 0; for (int i = 124; i < 136 && tar[end + i] >= '0' && tar[end + i] <= '7'; i++) fsz = fsz * 8 + (uint64_t)(tar[end + i] - '0');
        end += 512 + ALIGN_UP(fsz, 512);
    }
    size_t extra = 1024; for (int i = 0; i < nfiles; i++) extra += 512 + ALIGN_UP(strlen(contents[i]), 512);
    uint8_t *out = kzalloc(end + extra);
    if (!out) { kfree(tar); return -ENOMEM; }
    memcpy(out, tar, end);
    size_t o = end;
    for (int i = 0; i < nfiles; i++) {
        size_t l = strlen(contents[i]);
        tar_header(out + o, names[i], l); o += 512;
        memcpy(out + o, contents[i], l); o += ALIGN_UP(l, 512);
    }
    o += 1024;
    r = vfs_write_file("/mnt/.sda2/boot/initrd.tar", out, o);
    kfree(out); kfree(tar);
    vfs_sync();
    return r;
}
