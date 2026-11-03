/* inFlux installer: GPT + BIOS boot partition + FAT32 ESP (Limine, kernel, initrd) + btrfs root. */
#include "installer.h"
#include "fs.h"
#include "mm.h"
#include "string.h"
#include "btrfs.h"
#include "io.h"
#include "sched.h"
#include "../../../third_party/limine/limine-bios-hdd.h"

int fat32_format(struct blockdev *d, const char *label);
struct vnode *fat32_mount(struct blockdev *d);
struct vnode *btrfs_mount(struct blockdev *d);
int btrfs_mkfs(struct blockdev *d, const char *label);
int btrfs_unmount(struct vnode *root);

struct gpt_header { char sig[8]; uint32_t rev, hdr_size, crc, res; uint64_t cur_lba, backup_lba, first_usable, last_usable; uint8_t guid[16]; uint64_t part_lba; uint32_t nparts, part_size, parts_crc; } PACKED;
struct gpt_entry { uint8_t type[16], guid[16]; uint64_t first, last, attrs; uint16_t name[36]; } PACKED;
static const uint8_t GUID_BIOS_BOOT[16] = { 0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E, 0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 };
static const uint8_t GUID_ESP[16] = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };
static const uint8_t GUID_LINUX_FS[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };

#define LBA_BIOS_START 2048
#define LBA_ESP_START 4096
#define ESP_SECTORS (128 * 1024)      /* 64 MiB */
#define LBA_ROOT_START (LBA_ESP_START + ESP_SECTORS)

static void rand_bytes(uint8_t *p, int n) { static uint64_t seed; seed ^= rdtsc() ^ (time_unix() << 20); for (int i = 0; i < n; i++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; p[i] = seed >> 56; } }
static void set_name(struct gpt_entry *e, const char *n) { memset(e->name, 0, sizeof e->name); for (int i = 0; n[i] && i < 36; i++) e->name[i] = (uint8_t)n[i]; }

static int write_gpt(struct blockdev *d, install_progress_fn progress, void *ctx) {
    uint64_t total = d->sectors;
    if (total < LBA_ROOT_START + 131072) return -ENOSPC;   /* need at least 64 MiB for the root */
    uint8_t *sec = kzalloc(512);
    /* protective MBR */
    rand_bytes(sec + 440, 4);
    sec[446 + 4] = 0xEE; sec[446 + 1] = 0x00; sec[446 + 2] = 0x02; sec[446 + 3] = 0x00;
    sec[446 + 5] = 0xFF; sec[446 + 6] = 0xFF; sec[446 + 7] = 0xFF;
    *(uint32_t *)(sec + 446 + 8) = 1; *(uint32_t *)(sec + 446 + 12) = (uint32_t)MIN(total - 1, 0xFFFFFFFFull);
    sec[510] = 0x55; sec[511] = 0xAA;
    int r = blockdev_write(d, 0, 1, sec);
    /* entries */
    struct gpt_entry *ents = kzalloc(128 * sizeof(struct gpt_entry));
    uint64_t last_usable = total - 34;
    uint64_t root_end = ALIGN_DOWN(last_usable + 1, 2048) - 1;
    memcpy(ents[0].type, GUID_BIOS_BOOT, 16); rand_bytes(ents[0].guid, 16); ents[0].first = LBA_BIOS_START; ents[0].last = LBA_ESP_START - 1; set_name(&ents[0], "BIOS boot");
    memcpy(ents[1].type, GUID_ESP, 16); rand_bytes(ents[1].guid, 16); ents[1].first = LBA_ESP_START; ents[1].last = LBA_ROOT_START - 1; set_name(&ents[1], "EFI System");
    memcpy(ents[2].type, GUID_LINUX_FS, 16); rand_bytes(ents[2].guid, 16); ents[2].first = LBA_ROOT_START; ents[2].last = root_end; set_name(&ents[2], "inFlux");
    uint32_t parts_crc = crc32_ieee(ents, 128 * sizeof(struct gpt_entry));
    if (r == 0) r = blockdev_write(d, 2, 32, ents);
    if (r == 0) r = blockdev_write(d, total - 33, 32, ents);
    /* headers */
    struct gpt_header h; memset(&h, 0, sizeof h);
    memcpy(h.sig, "EFI PART", 8); h.rev = 0x00010000; h.hdr_size = 92; h.cur_lba = 1; h.backup_lba = total - 1; h.first_usable = 34; h.last_usable = last_usable;
    rand_bytes(h.guid, 16); h.part_lba = 2; h.nparts = 128; h.part_size = 128; h.parts_crc = parts_crc;
    h.crc = 0; h.crc = crc32_ieee(&h, 92);
    memset(sec, 0, 512); memcpy(sec, &h, sizeof h);
    if (r == 0) r = blockdev_write(d, 1, 1, sec);
    struct gpt_header b = h; b.cur_lba = total - 1; b.backup_lba = 1; b.part_lba = total - 33; b.crc = 0; b.crc = crc32_ieee(&b, 92);
    memset(sec, 0, 512); memcpy(sec, &b, sizeof b);
    if (r == 0) r = blockdev_write(d, total - 1, 1, sec);
    kfree(ents); kfree(sec);
    return r;
}

/* Port of Limine's `bios-install` using a dedicated BIOS boot partition for stage 2. */
static int bios_install(struct blockdev *d, uint64_t part_start_lba) {
    const uint8_t *img = binary_limine_hdd_bin_data;
    size_t img_size = sizeof(binary_limine_hdd_bin_data);
    size_t stage2_size = img_size - 512, stage2_sects = (stage2_size + 511) / 512;
    uint16_t size_a = (stage2_sects / 2) * 512 + (stage2_sects % 2 ? 512 : 0), size_b = (stage2_sects / 2) * 512;
    uint64_t loc_a = part_start_lba * 512, loc_b = ALIGN_UP(loc_a + size_a, 512);
    uint8_t *mbr = kmalloc(512);
    int r = blockdev_read(d, 0, 1, mbr);
    if (r < 0) { kfree(mbr); return r; }
    uint8_t saved_pt[70], saved_ts[6];
    memcpy(saved_pt, mbr + 440, 70); memcpy(saved_ts, mbr + 218, 6);
    memcpy(mbr, img, 512);
    memcpy(mbr + 440, saved_pt, 70); memcpy(mbr + 218, saved_ts, 6);
    memcpy(mbr + 0x1a4, &size_a, 2); memcpy(mbr + 0x1a6, &size_b, 2); memcpy(mbr + 0x1a8, &loc_a, 8); memcpy(mbr + 0x1b0, &loc_b, 8);
    r = blockdev_write(d, 0, 1, mbr);
    kfree(mbr);
    if (r == 0) r = blockdev_write_bytes(d, loc_a, img + 512, size_a);
    if (r == 0 && stage2_size > size_a) r = blockdev_write_bytes(d, loc_b, img + 512 + size_a, stage2_size - size_a);
    return r;
}

static int copy_file(const char *src, const char *dst) {
    void *data; size_t sz;
    int r = vfs_read_file(src, &data, &sz);
    if (r < 0) return r;
    r = vfs_write_file(dst, data, sz);
    kfree(data);
    return r;
}
static int copy_tree(const char *src, const char *dst) {
    struct stat st; if (vfs_stat(src, &st) < 0) return -ENOENT;
    if (st.type != VN_DIR) { int r = copy_file(src, dst); if (r < 0) kprintf("install: copying %s failed (%d)\n", src, r); return r; }
    int r = vfs_mkdir(dst);
    if (r < 0 && r != -EEXIST) { kprintf("install: creating %s failed (%d)\n", dst, r); return r; }
    struct vnode *d = vfs_lookup(src); if (!d) return -ENOENT;
    struct dirent de; r = 0;
    for (int i = 0; vfs_readdir(d, i, &de) == 0 && r == 0; i++) {
        char a[PATH_MAX], b[PATH_MAX];
        if ((size_t)snprintf(a, sizeof a, "%s/%s", src, de.name) >= sizeof a || (size_t)snprintf(b, sizeof b, "%s/%s", dst, de.name) >= sizeof b) { r = -ENAMETOOLONG; break; }
        r = copy_tree(a, b);
    }
    vnode_unref(d);
    return r;
}

bool install_is_installed_system(void) {
    for (int i = 0; i < vfs_mount_count(); i++) { char mp[PATH_MAX]; vfs_mount_at(i, mp, sizeof mp); if (!strcmp(mp, "/home")) return true; }
    return false;
}

int install_run(struct blockdev *disk, install_progress_fn progress, void *ctx) {
    if (!disk || disk->parent) return -EINVAL;
    char msg[160];
    #define STEP(pct, ...) do { snprintf(msg, sizeof msg, __VA_ARGS__); kprintf("install: %s\n", msg); if (progress) progress(ctx, pct, msg); } while (0)
    if (disk->sector_size != 512) { STEP(100, "%s has %u-byte sectors; only 512-byte sector disks are supported", disk->name, disk->sector_size); return -EINVAL; }
    /* never install onto a disk that is currently mounted */
    for (int i = 0; i < blockdev_count(); i++) { struct blockdev *b = blockdev_at(i); if (b && (b == disk || b->parent == disk) && b->fs_type) { for (int m = 0; m < vfs_mount_count(); m++) { char mp[PATH_MAX]; struct vnode *r = vfs_mount_at(m, mp, sizeof mp); if (r && r->priv == b) { STEP(0, "%s is mounted at %s; unmounting", b->name, mp); if (vfs_unmount(mp) == 0) { btrfs_unmount(r); m--; } } } } }
    STEP(2, "Writing GPT partition table to %s", disk->name);
    int r = write_gpt(disk, progress, ctx);
    if (r < 0) { STEP(100, "Partitioning failed (%d)", r); return r; }
    blockdev_rescan(disk);
    char n[24];
    snprintf(n, sizeof n, "%s%s2", disk->name, isdigit(disk->name[strlen(disk->name) - 1]) ? "p" : "");
    struct blockdev *esp = blockdev_find(n);
    snprintf(n, sizeof n, "%s%s3", disk->name, isdigit(disk->name[strlen(disk->name) - 1]) ? "p" : "");
    struct blockdev *rootp = blockdev_find(n);
    if (!esp || !rootp) { STEP(100, "Partitions did not appear after rescan"); return -EIO; }
    STEP(10, "Formatting EFI System Partition (%s, FAT32)", esp->name);
    if ((r = fat32_format(esp, "EFI")) < 0) { STEP(100, "FAT32 format failed (%d)", r); return r; }
    STEP(18, "Formatting root volume (%s, btrfs)", rootp->name);
    if ((r = btrfs_mkfs(rootp, "inFlux")) < 0) { STEP(100, "btrfs format failed (%d)", r); return r; }
    /* boot files */
    struct vnode *esp_root = fat32_mount(esp);
    if (!esp_root) { STEP(100, "Cannot mount the new EFI partition"); return -EIO; }
    vfs_mkdir("/mnt");
    if ((r = vfs_mount("/mnt/.install-esp", esp_root)) < 0) { STEP(100, "Cannot mount the new EFI partition (%d)", r); return r; }
    STEP(28, "Copying bootloader");
    static const char *const esp_dirs[] = { "/mnt/.install-esp/EFI", "/mnt/.install-esp/EFI/BOOT", "/mnt/.install-esp/boot" };
    for (int i = 0; i < 3; i++) if ((r = vfs_mkdir(esp_dirs[i])) < 0 && r != -EEXIST) { STEP(100, "Creating %s failed (%d)", esp_dirs[i], r); vfs_unmount("/mnt/.install-esp"); return r; }
    r = copy_file("/boot/limine/BOOTX64.EFI", "/mnt/.install-esp/EFI/BOOT/BOOTX64.EFI");
    if (r == 0) r = copy_file("/boot/limine/BOOTIA32.EFI", "/mnt/.install-esp/EFI/BOOT/BOOTIA32.EFI");
    if (r == 0) r = copy_file("/boot/limine/limine-bios.sys", "/mnt/.install-esp/limine-bios.sys");
    if (r < 0) { STEP(100, "Bootloader files are missing from the live system (%d)", r); vfs_unmount("/mnt/.install-esp"); return r; }
    STEP(40, "Copying kernel");
    r = copy_file("/boot/influx.elf", "/mnt/.install-esp/boot/influx.elf");
    if (r == 0) { STEP(52, "Copying system image (%lu KiB)", boot_info.initrd_size / 1024); r = vfs_write_file("/mnt/.install-esp/boot/initrd.tar", boot_info.initrd, boot_info.initrd_size); }
    if (r < 0) { STEP(100, "Copying the system failed (%d)", r); vfs_unmount("/mnt/.install-esp"); return r; }
    STEP(66, "Writing bootloader configuration");
    const char *conf = "timeout: 3\ndefault_entry: 1\n\n/inFlux\n    protocol: limine\n    path: boot():/boot/influx.elf\n    module_path: boot():/boot/initrd.tar\n    module_string: initrd\n";
    r = vfs_write_file("/mnt/.install-esp/limine.conf", conf, strlen(conf));
    if (r < 0) { STEP(100, "Writing limine.conf failed (%d)", r); vfs_unmount("/mnt/.install-esp"); return r; }
    if (esp_root->ops->sync && (r = esp_root->ops->sync(esp_root)) < 0) { STEP(100, "Writing the EFI partition failed (%d)", r); vfs_unmount("/mnt/.install-esp"); return r; }
    vfs_unmount("/mnt/.install-esp");
    /* root volume contents */
    struct vnode *root_vn = btrfs_mount(rootp);
    if (!root_vn) { STEP(100, "Cannot mount the new root volume"); return -EIO; }
    if ((r = vfs_mount("/mnt/.install-root", root_vn)) < 0) { STEP(100, "Cannot mount the new root volume (%d)", r); btrfs_unmount(root_vn); return r; }
    STEP(74, "Creating home directory");
    r = vfs_write_file("/mnt/.install-root/.influx_root", "inFlux root volume\n", 19);
    if (r < 0) STEP(100, "Writing the root volume marker failed (%d)", r);
    else if ((r = copy_tree("/home/flux", "/mnt/.install-root/flux")) < 0) STEP(100, "Copying the home directory failed (%d)", r);
    /* write-back errors of buffered btrfs data only show up here: sync the whole volume and check */
    int s = root_vn->ops->sync(root_vn);
    vfs_unmount("/mnt/.install-root");
    int s2 = btrfs_unmount(root_vn);
    if (r == 0 && (s < 0 || s2 < 0)) { r = s < 0 ? s : s2; STEP(100, "Writing the root volume failed (%d)", r); }
    if (r < 0) return r;
    STEP(90, "Installing BIOS boot code");
    if ((r = bios_install(disk, LBA_BIOS_START)) < 0) { STEP(100, "BIOS boot install failed (%d)", r); return r; }
    if ((r = blockdev_flush(disk)) < 0) { STEP(100, "Flushing the disk cache failed (%d)", r); return r; }
    blockdev_rescan(disk);
    STEP(100, "Installation complete. Remove the installation medium and restart.");
    return 0;
}

/* ---- persistent display resolution: `resolution:` in the installed limine.conf ---- */
int display_persist_resolution(int w, int h) {
    for (int i = 0; i < vfs_mount_count(); i++) {
        char mp[PATH_MAX]; struct vnode *root = vfs_mount_at(i, mp, sizeof mp);
        if (!root || !root->fs_name || strcmp(root->fs_name, "fat32")) continue;
        char path[PATH_MAX]; snprintf(path, sizeof path, "%s/limine.conf", mp);
        void *data; size_t sz;
        if (vfs_read_file(path, &data, &sz) < 0) continue;
        char *out = kmalloc(sz + 64); size_t o = 0; bool done = false;
        char *save, *line = strtok_r(data, "\n", &save);
        while (line) {
            const char *t = line; while (*t == ' ') t++;
            if (!strncmp(t, "resolution:", 11)) { if (!done) { o += snprintf(out + o, sz + 64 - o, "    resolution: %dx%d\n", w, h); done = true; } }
            else {
                o += snprintf(out + o, sz + 64 - o, "%s\n", line);
                if (!done && !strncmp(t, "protocol:", 9)) { o += snprintf(out + o, sz + 64 - o, "    resolution: %dx%d\n", w, h); done = true; }
            }
            line = strtok_r(NULL, "\n", &save);
        }
        kfree(data);
        int r = done ? vfs_write_file(path, out, o) : -EINVAL;
        kfree(out);
        if (r == 0) kprintf("display: boot resolution %dx%d written to %s\n", w, h, path);
        return r;
    }
    return -ENOENT;
}
