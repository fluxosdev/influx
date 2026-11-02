#include "blockdev.h"
#include "fs.h"
#include "string.h"
#include "mm.h"
#include "gpu.h"

struct vnode *btrfs_mount(struct blockdev *d);

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
        if (!root) continue;
        char mp[64];
        snprintf(mp, sizeof mp, "/mnt/%s", d->name);
        vfs_mkdir("/mnt");
        if (vfs_mount(mp, root) == 0) root->priv = d;
    }
}
