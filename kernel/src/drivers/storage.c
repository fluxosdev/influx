#include "audio.h"
#include "blockdev.h"
#include "fs.h"
#include "string.h"
#include "mm.h"
#include "gpu.h"

struct vnode *btrfs_mount(struct blockdev *d);
struct vnode *fat32_mount(struct blockdev *d);
int btrfs_unmount(struct vnode *root);   /* final write-back, leaves the background flusher */

void drivers_init(void) { gpu_init(); audio_init(); }

void storage_init(void) {
    extern void fb_boot_progress(const char *);
    fb_boot_progress("Probing storage");
    ahci_init();
    nvme_init();
    for (int i = 0; i < blockdev_count(); i++) { struct blockdev *d = blockdev_at(i); if (d && !d->parent) storage_mount(d); }
}

static void mount_one(struct blockdev *d) {
    if (vfs_mount_count() >= VFS_MAX_MOUNTS) { kprintf("storage: no free mount slot for %s\n", d->name); return; }   /* a mounted fs cannot be torn down again */
    struct vnode *root = btrfs_mount(d);
    if (!root) root = fat32_mount(d);
    if (!root) return;
    char mp[64];
    snprintf(mp, sizeof mp, d->esp ? "/mnt/.%s" : "/mnt/%s", d->name);   /* dot-name hides the ESP */
    vfs_mkdir("/mnt");
    /* an installed inFlux root volume becomes the persistent home directory */
    struct vnode *marker = NULL;
    if (!strcmp(root->fs_name, "btrfs")) { struct vnode *m = root->ops->lookup(root, ".influx_root"); if (m) { marker = m; } }
    if (marker) {
        bool home_taken = false;
        for (int i = 0; i < vfs_mount_count(); i++) { char p[64]; if (vfs_mount_at(i, p, sizeof p) && !strcmp(p, "/home")) home_taken = true; }
        if (!home_taken) { strcpy(mp, "/home"); kprintf("storage: %s is the installed inFlux root, mounting at /home\n", d->name); }
        vnode_unref(marker);
    }
    if (vfs_mount(mp, root) == 0) root->priv = d;
    else { kprintf("storage: mounting %s on %s failed\n", d->name, mp); btrfs_unmount(root); }
}

/* a whole disk with partitions mounts each partition, otherwise the disk itself */
void storage_mount(struct blockdev *d) {
    bool has_parts = false;
    for (int j = 0; j < blockdev_count(); j++) { struct blockdev *p = blockdev_at(j); if (p && p->parent == d && !p->removed) { has_parts = true; mount_one(p); } }
    if (!has_parts) mount_one(d);
}

/* mounted volumes carry their blockdev in the root's priv (see mount_one);
 * other filesystems keep their own data there, so only compare pointers */
static bool on_disk(void *priv, struct blockdev *d) {
    if (priv == d) return true;
    for (int i = 0; i < blockdev_count(); i++) { struct blockdev *p = blockdev_at(i); if (p && p->parent == d && priv == p) return true; }
    return false;
}

void storage_unmount(struct blockdev *d) {
    for (int i = 0; i < vfs_mount_count(); ) {
        char mp[64]; struct vnode *root = vfs_mount_at(i, mp, sizeof mp);
        if (root && on_disk(root->priv, d) && strcmp(mp, "/")) {
            kprintf("storage: unmounting %s (%s removed)\n", mp, d->name);
            vfs_unmount(mp);
            btrfs_unmount(root);   /* no-op for other filesystems */
            if (!strncmp(mp, "/mnt/", 5)) vfs_unlink(mp);
            continue;
        }
        i++;
    }
}
