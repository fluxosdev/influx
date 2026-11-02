#include "blockdev.h"
#include "fs.h"
#include "string.h"
#include "mm.h"
#include "gpu.h"

struct vnode *btrfs_mount(struct blockdev *d);
struct vnode *fat32_mount(struct blockdev *d);

void drivers_init(void) { gpu_init(); }

void storage_init(void) {
    extern void fb_boot_progress(const char *);
    fb_boot_progress("Probing storage");
    ahci_init();
    nvme_init();
    for (int i = 0; i < blockdev_count(); i++) {
        struct blockdev *d = blockdev_at(i);
        /* skip whole disks that have partitions */
        bool has_parts = false;
        for (int j = 0; j < blockdev_count(); j++) if (blockdev_at(j)->parent == d) has_parts = true;
        if (has_parts) continue;
        struct vnode *root = btrfs_mount(d);
        if (!root) root = fat32_mount(d);
        if (!root) continue;
        char mp[64];
        snprintf(mp, sizeof mp, "/mnt/%s", d->name);
        vfs_mkdir("/mnt");
        /* an installed inFlux root volume becomes the persistent home directory */
        struct vnode *marker = NULL;
        if (!strcmp(root->fs_name, "btrfs")) { struct vnode *m = root->ops->lookup(root, ".influx_root"); if (m) { marker = m; } }
        if (marker) { vnode_unref(marker); strcpy(mp, "/home"); kprintf("storage: %s is the installed inFlux root, mounting at /home\n", d->name); }
        if (vfs_mount(mp, root) == 0) root->priv = d;
    }
}
