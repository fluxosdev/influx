#pragma once
#include "kernel.h"

struct blockdev {
    char name[16];
    char model[48];
    uint64_t sectors;
    uint32_t sector_size;
    uint64_t start_lba;              /* partition offset within parent */
    struct blockdev *parent;
    int (*read)(struct blockdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blockdev *d);   /* write back the device's volatile cache; NULL: nothing to flush */
    void *priv;
    const char *fs_type;
    const char *bus;                 /* "SATA" / "NVMe" / "USB" */
    struct vnode *vn;
    bool esp;                        /* EFI System Partition: mounted hidden */
    volatile bool removed;           /* unplugged: I/O fails, the struct stays for stale users */
};
void blockdev_register(struct blockdev *d);   /* registers the disk, scans partitions */
void blockdev_unregister(struct blockdev *d); /* hot unplug: drop the disk and its partitions from /dev */
void blockdev_alloc_name(const char *prefix, char *out, size_t n);   /* first free name: sda, sdb, ... */
void blockdev_rescan(struct blockdev *d);     /* drop and re-read the partition table */
void storage_mount(struct blockdev *d);       /* mount the disk's filesystems under /mnt (or /home) */
void storage_unmount(struct blockdev *d);     /* unmount everything on the disk */
int blockdev_count(void);
struct blockdev *blockdev_at(int i);
struct blockdev *blockdev_find(const char *name);
int blockdev_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf);
int blockdev_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf);
int blockdev_flush(struct blockdev *d);   /* make completed writes durable (partitions flush the whole disk) */
int blockdev_read_bytes(struct blockdev *d, uint64_t off, void *buf, size_t n);
int blockdev_write_bytes(struct blockdev *d, uint64_t off, const void *buf, size_t n);
void ahci_init(void);
void nvme_init(void);
